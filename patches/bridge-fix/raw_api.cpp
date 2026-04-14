/*
 * raw_api.cpp - Implementation of the C-linkage API for udp2raw
 *
 * This file is compiled with udp2raw include paths (-I$(UDPRAW_DIR)).
 * It accesses udp2raw globals directly and wraps them behind extern "C"
 * functions so that fecraw (compiled with UDPspeeder headers) can call
 * into udp2raw without any symbol conflicts.
 *
 * After compilation, this .o is combined with all other udp2raw .o files
 * via `ld -r`, and then `objcopy` localizes everything except the
 * raw_api_* symbols.
 */

#include "common.h"
#include "misc.h"
#include "network.h"
#include "encrypt.h"
#include "log.h"
#include "fd_manager.h"

#include "raw_api.h"

extern int client_event_loop();
extern int server_event_loop();
extern int seq_mode;
extern int disable_bpf_filter;
extern int use_tcp_dummy_socket;
extern int bridge_mode;

extern "C" {

void raw_api_init(const raw_api_config_t *cfg) {
    program_mode = cfg->is_server ? server_mode : client_mode;
    raw_mode = (raw_mode_t)cfg->raw_mode;

    /* Map fecraw config values to udp2raw internal enum values.
     * Config: 0=aes128cfb 1=aes128cbc 2=xor 3=none 4=aes256gcm 5=chacha20poly1305
     * udp2raw: cipher_none=0 cipher_aes128cbc=1 cipher_xor=2 cipher_aes128cfb=3 cipher_aes256gcm=4 cipher_chacha20poly1305=5 */
    static const cipher_mode_t cipher_map[] = {
        cipher_aes128cfb, cipher_aes128cbc, cipher_xor, cipher_none,
        cipher_aes256gcm, cipher_chacha20poly1305
    };
    cipher_mode = (cfg->cipher_mode >= 0 && cfg->cipher_mode < 6) ? cipher_map[cfg->cipher_mode] : cipher_aes128cbc;

    /* Config: auth 0=hmac_sha1 1=md5 2=crc32 3=simple 4=none
     * udp2raw: auth_none=0 auth_md5=1 auth_crc32=2 auth_simple=3 auth_hmac_sha1=4 */
    static const auth_mode_t auth_map[] = {auth_hmac_sha1, auth_md5, auth_crc32, auth_simple, auth_none};
    auth_mode = (cfg->auth_mode >= 0 && cfg->auth_mode < 5) ? auth_map[cfg->auth_mode] : auth_hmac_sha1;

    if (cipher_is_aead(cipher_mode))
        auth_mode = auth_none;

    seq_mode = cfg->seq_mode;
    auto_add_iptables_rule = cfg->auto_iptables;
    disable_anti_replay = cfg->disable_anti_replay;
    hb_mode = cfg->hb_mode;
    hb_len = cfg->hb_len;
    socket_buf_size = cfg->socket_buf_size;
    log_level = cfg->log_level;

    if (cfg->key[0] != '\0')
        strncpy(key_string, cfg->key, sizeof(key_string) - 1);

    if (cfg->local_addr[0] != '\0')
        local_addr.from_str((char *)cfg->local_addr);

    if (cfg->remote_addr[0] != '\0')
        remote_addr.from_str((char *)cfg->remote_addr);

    if (cfg->is_server)
        raw_ip_version = local_addr.get_type();
    else
        raw_ip_version = remote_addr.get_type();

    const_id = get_true_random_number_nz();
    srand(get_true_random_number_nz());
}

void raw_api_setup(void) {
    my_init_keys(key_string, program_mode == client_mode ? 1 : 0);
    iptables_rule();
    init_raw_socket();
}

int raw_api_client_loop(int bridge_fd) {
    bridge_mode = 1;
    udp_fd = bridge_fd;
    return client_event_loop();
}

int raw_api_server_loop(int bridge_fd) {
    bridge_mode = 1;
    udp_fd = bridge_fd;
    return server_event_loop();
}

unsigned int raw_api_get_true_random_nz(void) {
    return get_true_random_number_nz();
}

const char *raw_api_mode_name(int mode) {
    auto it = raw_mode_tostring.find(mode);
    if (it != raw_mode_tostring.end())
        return it->second;
    return "unknown";
}

} /* extern "C" */
