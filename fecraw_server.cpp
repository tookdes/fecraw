/*
 * fecraw_server.cpp - Unified server event loop
 *
 * Same dual-thread architecture as client:
 *   Thread 1 (main): TUN <-> FEC encode/decode <-> socketpair[0]
 *   Thread 2 (raw):  socketpair[1] <-> udp2raw encrypt/decrypt <-> raw socket
 */

#include "common.h"
#include "connection.h"
#include "misc.h"
#include "fd_manager.h"
#include "delay_manager.h"
#include "tun_dev.h"
#include "packet.h"
#include "raw_api.h"
#include "fecraw_config.h"
#include "adaptive_fec.h"
#include "small_packet.h"
#include "pacing.h"

#include <pthread.h>
#include <sys/socket.h>

extern fecraw_config_t g_cfg;

static int got_first_packet = 0;
static dest_t raw_dest;
static dest_t tun_dest;
static int bridge_fec_fd = -1;

static adaptive_fec_t   g_srv_adaptive;
static small_packet_sender_t   g_srv_sp_send;
static small_packet_receiver_t g_srv_sp_recv;
static pacing_t          g_srv_pacing;

static void *raw_thread_func(void *arg) {
    int fd = *(int *)arg;
    raw_api_server_loop(fd);
    return NULL;
}

static void bridge_recv_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    assert(!(revents & EV_ERROR));
    conn_info_t &conn_info = *((conn_info_t *)watcher->data);

    char data[buf_len];
    int len = recv(bridge_fec_fd, data, max_data_len + 1, 0);

    if (len == max_data_len + 1) {
        mylog(log_warn, "huge packet dropped\n");
        return;
    }
    if (len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        mylog(log_warn, "bridge recv error: %s\n", strerror(errno));
        return;
    }
    if (len == 0) return;

    if (de_cook(data, len) < 0) {
        mylog(log_warn, "de_cook failed\n");
        return;
    }

    char header = 0;
    if (get_header(header, data, len) != 0) {
        mylog(log_warn, "get_header failed\n");
        return;
    }

    if (header == header_keep_alive) return;

    if (header == header_new_connect || header == header_normal) {
        if (!got_first_packet) {
            got_first_packet = 1;
            mylog(log_info, "first client packet received\n");
        }
    } else {
        mylog(log_warn, "invalid header %d\n", int(header));
        return;
    }

    if (g_cfg.enable_pacing)
        g_srv_pacing.on_ack(len, g_srv_pacing.smoothed_rtt_s > 0 ? g_srv_pacing.smoothed_rtt_s : 0.05);

    if (g_cfg.small_packet_threshold > 0 && small_packet_receiver_t::is_small_packet(data, len)) {
        char payload[buf_len];
        int plen = g_srv_sp_recv.receive(data, len, payload, sizeof(payload));
        if (plen > 0) {
            int wlen = write(tun_dest.inner.fd, payload, plen);
            (void)wlen;
        }
        return;
    }

    from_fec_to_normal2(conn_info, tun_dest, data, len);
}

static void tun_fd_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    assert(!(revents & EV_ERROR));
    conn_info_t &conn_info = *((conn_info_t *)watcher->data);

    char data[buf_len];
    int len = read(watcher->fd, data, max_data_len + 1);

    if (len == max_data_len + 1) {
        mylog(log_warn, "huge packet dropped\n");
        return;
    }
    if (len < 0) {
        mylog(log_warn, "tun read error: %s\n", strerror(errno));
        return;
    }

    do_mssfix(data, len);

    if (g_cfg.enable_pacing)
        g_srv_pacing.wait(len);

    if (g_cfg.small_packet_threshold > 0 && len < g_cfg.small_packet_threshold) {
        char frame[buf_len];
        int flen = g_srv_sp_send.build_frame(data, len, frame, sizeof(frame));
        if (flen > 0) {
            int redundancy = g_srv_sp_send.get_redundancy();
            char cooked[buf_len];
            for (int i = 0; i < redundancy; i++) {
                int clen = flen;
                memcpy(cooked, frame, flen);
                do_cook(cooked, clen);
                send(bridge_fec_fd, cooked, clen, 0);
            }
            return;
        }
    }

    from_normal_to_fec2(conn_info, raw_dest, data, len, header_normal);
}

static void delay_manager_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    (void)loop; (void)watcher; (void)revents;
}

static void fec_encode_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    conn_info_t &conn_info = *((conn_info_t *)watcher->data);
    from_normal_to_fec2(conn_info, raw_dest, 0, 0, header_normal);
}

static u64_t srv_prev_fec_input = 0;
static u64_t srv_prev_fec_output = 0;

static void conn_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    conn_info_t &conn_info = *((conn_info_t *)watcher->data);
    conn_info.stat.report_as_server(conn_info.addr);
    do_keep_alive(raw_dest);

    if (g_cfg.fec_adaptive) {
        u64_t cur_in  = conn_info.stat.fec_to_normal.input_packet_num;
        u64_t cur_out = conn_info.stat.fec_to_normal.output_packet_num;
        u64_t delta_in  = cur_in - srv_prev_fec_input;
        u64_t delta_out = cur_out - srv_prev_fec_output;
        srv_prev_fec_input  = cur_in;
        srv_prev_fec_output = cur_out;

        if (delta_in > 0) {
            int sent = (int)delta_in;
            int recovered = (sent > (int)delta_out) ? sent - (int)delta_out : 0;
            g_srv_adaptive.record_sent(sent);
            if (recovered > 0)
                g_srv_adaptive.record_loss(recovered);
        }

        int d, p;
        if (g_srv_adaptive.adjust(d, p)) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%d:%d", d, p);
            conn_info.fec_encode_manager.get_fec_par().rs_from_str(buf);
            mylog(log_info, "adaptive FEC adjusted to %s\n", buf);
        }
    }
}

static void fifo_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    char buf[buf_len];
    int len = read(watcher->fd, buf, sizeof(buf) - 1);
    if (len < 0) return;
    buf[len] = 0;
    handle_command(buf);
}

static void prepare_cb(struct ev_loop *loop, struct ev_prepare *watcher, int revents) {
    delay_manager.check();
}

int fecraw_server_event_loop() {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) < 0) {
        mylog(log_fatal, "socketpair() failed: %s\n", strerror(errno));
        myexit(-1);
    }

    bridge_fec_fd = sv[0];
    int bridge_raw_fd = sv[1];

    setnonblocking(bridge_fec_fd);
    setnonblocking(bridge_raw_fd);

    int bufsize = 2 * 1024 * 1024;
    setsockopt(bridge_fec_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(bridge_fec_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(bridge_raw_fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    setsockopt(bridge_raw_fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));

    pthread_t raw_thread;
    if (pthread_create(&raw_thread, NULL, raw_thread_func, &bridge_raw_fd) != 0) {
        mylog(log_fatal, "Failed to create raw transport thread\n");
        myexit(-1);
    }
    pthread_detach(raw_thread);

    int tun_fd = get_tun_fd(tun_dev);
    if (tun_fd < 0) {
        mylog(log_fatal, "get_tun_fd failed for %s\n", tun_dev);
        myexit(-1);
    }

    if (set_tun(tun_dev,
                htonl((ntohl(sub_net_uint32) & 0xFFFFFF00) | 1),
                htonl((ntohl(sub_net_uint32) & 0xFFFFFF00) | 2),
                tun_mtu) != 0) {
        mylog(log_fatal, "set_tun failed\n");
        myexit(-1);
    }

    fd64_t bridge_fd64 = fd_manager.create(bridge_fec_fd);

    tun_dest.type = type_write_fd;
    tun_dest.inner.fd = tun_fd;

    raw_dest.cook = 1;
    raw_dest.type = type_fd64;
    raw_dest.inner.fd64 = bridge_fd64;

    conn_info_t *conn_info_p = new conn_info_t;
    conn_info_t &conn_info = *conn_info_p;

    struct ev_loop *loop = ev_loop_new(0);
    assert(loop != NULL);
    conn_info.loop = loop;

    struct ev_io bridge_recv_watcher;
    bridge_recv_watcher.data = &conn_info;
    bridge_recv_watcher.u64 = bridge_fd64;
    ev_io_init(&bridge_recv_watcher, bridge_recv_cb, bridge_fec_fd, EV_READ);
    ev_io_start(loop, &bridge_recv_watcher);

    struct ev_io tun_fd_watcher;
    tun_fd_watcher.data = &conn_info;
    ev_io_init(&tun_fd_watcher, tun_fd_cb, tun_fd, EV_READ);
    ev_io_start(loop, &tun_fd_watcher);

    delay_manager.set_loop_and_cb(loop, delay_manager_cb);
    conn_info.fec_encode_manager.set_data(&conn_info);
    conn_info.fec_encode_manager.set_loop_and_cb(loop, fec_encode_cb);

    conn_info.timer.data = &conn_info;
    ev_init(&conn_info.timer, conn_timer_cb);
    ev_timer_set(&conn_info.timer, 0, timer_interval / 1000.0);
    ev_timer_start(loop, &conn_info.timer);

    struct ev_io fifo_watcher;
    if (fifo_file[0] != 0) {
        int fifo_fd = create_fifo(fifo_file);
        ev_io_init(&fifo_watcher, fifo_cb, fifo_fd, EV_READ);
        ev_io_start(loop, &fifo_watcher);
    }

    ev_prepare prepare_watcher;
    ev_init(&prepare_watcher, prepare_cb);
    ev_prepare_start(loop, &prepare_watcher);

    if (g_cfg.fec_adaptive) {
        int d = 20, p = 10;
        sscanf(g_cfg.fec_str, "%d:%d", &d, &p);
        g_srv_adaptive.init(d, p);
        mylog(log_info, "adaptive FEC enabled (base %d:%d)\n", d, p);
    }
    if (g_cfg.small_packet_threshold > 0) {
        g_srv_sp_send.init(g_cfg.small_packet_threshold, g_cfg.small_packet_redundancy);
        g_srv_sp_recv.init();
        mylog(log_info, "small packet mode: threshold=%d redundancy=%d\n",
              g_cfg.small_packet_threshold, g_cfg.small_packet_redundancy);
    }
    if (g_cfg.enable_pacing) {
        g_srv_pacing.init(g_cfg.max_bandwidth);
        mylog(log_info, "BBR pacing enabled (max_bw=%lld)\n", (long long)g_cfg.max_bandwidth);
    }

    mylog(log_info, "fecraw server event loop started\n");
    ev_run(loop, 0);

    ev_loop_destroy(loop);
    myexit(0);
    return 0;
}
