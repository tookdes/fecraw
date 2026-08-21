/*
 * packet.cpp
 *
 *  Created on: Sep 15, 2017
 *      Author: root
 */

#include "common.h"
#include "log.h"
#include "packet.h"
#include "misc.h"
#include "crc32/Crc32.h"
#include "telemetry.h"
#include "pacing.h"

int iv_min = 4;
int iv_max = 32;  //< 256;
u64_t packet_send_count = 0;
u64_t dup_packet_send_count = 0;
u64_t packet_recv_count = 0;
u64_t dup_packet_recv_count = 0;

typedef u64_t anti_replay_seq_t;
int disable_replay_filter = 0;

int disable_obscure = 0;
int disable_xor = 0;

int random_drop = 0;

char key_string[1000] = "";

void encrypt_0(char *input, int &len, char *key) {
    int i, j;
    if (key[0] == 0) return;
    for (i = 0, j = 0; i < len; i++, j++) {
        if (key[j] == 0) j = 0;
        input[i] ^= key[j];
    }
}

void decrypt_0(char *input, int &len, char *key) {
    int i, j;
    if (key[0] == 0) return;
    for (i = 0, j = 0; i < len; i++, j++) {
        if (key[j] == 0) j = 0;
        input[i] ^= key[j];
    }
}
int do_obscure_old(const char *input, int in_len, char *output, int &out_len) {
    int i, j, k;
    if (in_len > 65535 || in_len < 0)
        return -1;
    int iv_len = iv_min + rand() % (iv_max - iv_min);
    get_fake_random_chars(output, iv_len);
    memcpy(output + iv_len, input, in_len);

    output[iv_len + in_len] = (uint8_t)iv_len;

    output[iv_len + in_len] ^= output[0];
    output[iv_len + in_len] ^= key_string[0];

    for (i = 0, j = 0, k = 1; i < in_len; i++, j++, k++) {
        if (j == iv_len) j = 0;
        if (key_string[k] == 0) k = 0;
        output[iv_len + i] ^= output[j];
        output[iv_len + i] ^= key_string[k];
    }

    out_len = iv_len + in_len + 1;
    return 0;
}

int do_obscure(char *data, int &len) {
    assert(len >= 0);
    assert(len < buf_len);

    int iv_len = random_between(iv_min, iv_max);
    get_fake_random_chars(data + len, iv_len);
    data[iv_len + len] = (uint8_t)iv_len;
    for (int i = 0, j = 0; i < len; i++, j++) {
        if (j == iv_len) j = 0;
        data[i] ^= data[len + j];
    }

    len = len + iv_len + 1;
    return 0;
}

int de_obscure(char *data, int &len) {
    if (len < 1) return -1;
    int iv_len = int((uint8_t)data[len - 1]);

    if (len < 1 + iv_len) return -1;

    len = len - 1 - iv_len;
    for (int i = 0, j = 0; i < len; i++, j++) {
        if (j == iv_len) j = 0;
        data[i] ^= data[len + j];
    }

    return 0;
}
int de_obscure_old(const char *input, int in_len, char *output, int &out_len) {
    int i, j, k;
    if (in_len > 65535 || in_len < 0) {
        mylog(log_debug, "in_len > 65535||in_len<0 ,  %d", in_len);
        return -1;
    }
    int iv_len = int((uint8_t)(input[in_len - 1] ^ input[0] ^ key_string[0]));
    out_len = in_len - 1 - iv_len;
    if (out_len < 0) {
        mylog(log_debug, "%d %d\n", in_len, out_len);
        return -1;
    }
    for (i = 0, j = 0, k = 1; i < in_len; i++, j++, k++) {
        if (j == iv_len) j = 0;
        if (key_string[k] == 0) k = 0;
        output[i] = input[iv_len + i] ^ input[j] ^ key_string[k];
    }
    dup_packet_recv_count++;
    return 0;
}

int sendto_fd_addr(int fd, address_t addr, char *buf, int len, int flags) {
    return sendto(fd, buf,
                  len, 0,
                  (struct sockaddr *)&addr.inner,
                  addr.get_len());
}

int send_fd(int fd, char *buf, int len, int flags) {
    return send(fd, buf, len, flags);
}

static bool is_telemetry_ack_frame(const char *data, int len) {
    return len == telemetry_link_t::kAckFrame &&
           (unsigned char)data[0] == 0xF3 &&
           (unsigned char)data[1] == 0xEC &&
           (unsigned char)data[2] == 2 &&
           (unsigned char)data[3] == 2;
}

int my_send(const dest_t &dest, char *data, int len) {
    char wire_buf[buf_len];
    char *send_data = data;
    int send_len = len;
    uint64_t telemetry_seq = 0;
    bool telemetry_data = false;
    bool telemetry_ack = false;

    if (dest.cook && g_fecraw_telemetry.enabled()) {
        telemetry_ack = is_telemetry_ack_frame(data, len);
        if (!telemetry_ack) {
            int wrapped = g_fecraw_telemetry.prepare_data(
                data, len, wire_buf, (int)sizeof(wire_buf), telemetry_seq);
            if (wrapped < 0) {
                mylog(log_warn, "telemetry frame overflow, packet dropped len=%d\n", len);
                return -1;
            }
            send_data = wire_buf;
            send_len = wrapped;
            telemetry_data = true;
        }
    }

    if (dest.cook)
        do_cook(send_data, send_len);

    // ACK feedback is control traffic and deliberately bypasses the data pacer.
    // Data pacing is queued instead of sleeping this libev thread. The queued
    // packet is already telemetry-framed and cooked, so cook=0 prevents the
    // second my_send() from framing or pacing it again when its timer fires.
    bool pacing_reserved = false;
    if (telemetry_data && g_fecraw_pacing.enabled) {
        uint64_t pace_delay = g_fecraw_pacing.reserve_delay_us(send_len);
        pacing_reserved = true;
        if (pace_delay > 0) {
            dest_t paced_dest = dest;
            paced_dest.cook = 0;
            int queued = delay_manager.add((my_time_t)pace_delay, paced_dest, send_data, send_len);
            if (queued == 0) {
                g_fecraw_telemetry.commit_sent(
                    telemetry_seq, send_len, (double)pace_delay / 1e6);
                return 0;
            }
            g_fecraw_pacing.cancel_reserved(send_len);
            return -1;
        }
    }

    int ret = -1;
    switch (dest.type) {
        case type_fd_addr: {
            ret = sendto_fd_addr(dest.inner.fd, dest.inner.fd_addr.addr, send_data, send_len, 0);
            break;
        }
        case type_fd64_addr: {
            if (fd_manager.exist(dest.inner.fd64)) {
                int fd = fd_manager.to_fd(dest.inner.fd64);
                ret = sendto_fd_addr(fd, dest.inner.fd64_addr.addr, send_data, send_len, 0);
            }
            break;
        }
        case type_fd: {
            ret = send_fd(dest.inner.fd, send_data, send_len, 0);
            break;
        }
        case type_write_fd: {
            ret = write(dest.inner.fd, send_data, send_len);
            break;
        }
        case type_fd64: {
            if (fd_manager.exist(dest.inner.fd64)) {
                int fd = fd_manager.to_fd(dest.inner.fd64);
                ret = send_fd(fd, send_data, send_len, 0);
            }
            break;
        }
        default:
            assert(0 == 1);
    }

    if (telemetry_data && ret >= 0)
        g_fecraw_telemetry.commit_sent(telemetry_seq, send_len);
    else if (pacing_reserved && ret < 0)
        g_fecraw_pacing.cancel_reserved(send_len);
    return ret;
}

int put_conv0(u32_t conv, const char *input, int len_in, char *&output, int &len_out) {
    assert(len_in >= 0);
    static char buf[buf_len];
    output = buf;
    u32_t n_conv = htonl(conv);
    memcpy(output, &n_conv, sizeof(n_conv));
    memcpy(output + sizeof(n_conv), input, len_in);
    u32_t crc32 = (u32_t)crc32_fast(output, len_in + sizeof(crc32));
    u32_t crc32_n = htonl(crc32);
    len_out = len_in + (int)(sizeof(n_conv)) + (int)sizeof(crc32_n);
    memcpy(output + len_in + (int)(sizeof(n_conv)), &crc32_n, sizeof(crc32_n));
    return 0;
}
int get_conv0(u32_t &conv, const char *input, int len_in, char *&output, int &len_out) {
    assert(len_in >= 0);
    u32_t n_conv;
    memcpy(&n_conv, input, sizeof(n_conv));
    conv = ntohl(n_conv);
    output = (char *)input + sizeof(n_conv);
    u32_t crc32_n;
    len_out = len_in - (int)sizeof(n_conv) - (int)sizeof(crc32_n);
    if (len_out < 0) {
        mylog(log_debug, "len_out<0\n");
        return -1;
    }
    memcpy(&crc32_n, input + len_in - (int)sizeof(crc32_n), sizeof(crc32_n));
    u32_t crc32 = ntohl(crc32_n);
    if (crc32 != (u32_t)crc32_fast(input, len_in - sizeof(crc32_n))) {
        mylog(log_debug, "crc32 check failed\n");
        return -1;
    }
    return 0;
}
int put_crc32(char *s, int &len) {
    if (disable_checksum) return 0;
    assert(len >= 0);
    u32_t crc32 = (u32_t)crc32_fast(s, len);
    write_u32(s + len, crc32);
    len += sizeof(u32_t);

    return 0;
}

int do_cook(char *data, int &len) {
    put_crc32(data, len);
    if (!disable_obscure) do_obscure(data, len);
    if (!disable_xor) encrypt_0(data, len, key_string);
    return 0;
}

int de_cook(char *s, int &len) {
    if (!disable_xor) decrypt_0(s, len, key_string);
    if (!disable_obscure) {
        int ret = de_obscure(s, len);
        if (ret != 0) {
            mylog(log_debug, "de_obscure fail\n");
            return ret;
        }
    }
    int ret = rm_crc32(s, len);
    if (ret != 0) {
        mylog(log_debug, "rm_crc32 fail\n");
        return ret;
    }
    return 0;
}
int rm_crc32(char *s, int &len) {
    if (disable_checksum) return 0;
    assert(len >= 0);
    len -= sizeof(u32_t);
    if (len < 0) return -1;
    u32_t crc32_in = read_u32(s + len);
    u32_t crc32 = (u32_t)crc32_fast(s, len);
    if (crc32 != crc32_in) return -1;
    return 0;
}

int put_conv(u32_t conv, const char *input, int len_in, char *&output, int &len_out) {
    static char buf[buf_len];
    output = buf;
    u32_t n_conv = htonl(conv);
    memcpy(output, &n_conv, sizeof(n_conv));
    memcpy(output + sizeof(n_conv), input, len_in);
    len_out = len_in + (int)(sizeof(n_conv));

    return 0;
}
int get_conv(u32_t &conv, const char *input, int len_in, char *&output, int &len_out) {
    u32_t n_conv;
    memcpy(&n_conv, input, sizeof(n_conv));
    conv = ntohl(n_conv);
    output = (char *)input + sizeof(n_conv);
    len_out = len_in - (int)sizeof(n_conv);
    if (len_out < 0) {
        mylog(log_debug, "len_out<0\n");
        return -1;
    }
    return 0;
}
