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
#include "erasure_runtime.h"
#include "window_rlnc.h"

#include <pthread.h>
#include <sys/socket.h>
#include <vector>

extern fecraw_config_t g_cfg;

static int got_first_packet = 0;
static dest_t raw_dest;
static dest_t tun_dest;
static int bridge_fec_fd = -1;

static adaptive_fec_t g_srv_adaptive;
static small_packet_sender_t g_srv_sp_send;
static small_packet_receiver_t g_srv_sp_recv;
static window_rlnc_sender_t g_srv_rlnc_send;
static window_rlnc_receiver_t g_srv_rlnc_recv;
static int g_srv_tx_codec = FECRAW_CODEC_RS;

static void *raw_thread_func(void *arg) {
    int fd = *(int *)arg;
    raw_api_server_loop(fd);
    return NULL;
}

static int choose_tx_codec() {
    if (g_cfg.disable_fec) return FECRAW_CODEC_RS;
    if (g_cfg.fec_codec != FECRAW_CODEC_AUTO) return g_cfg.fec_codec;

    loss_snapshot_t s = g_fecraw_telemetry.snapshot();
    double rtt = g_fecraw_telemetry.smoothed_rtt_s();
    if (s.floor_trusted && s.floor >= 0.02 && rtt >= 0.060)
        return FECRAW_CODEC_RLNC;
    return FECRAW_CODEC_RS;
}

static void send_rlnc_packet(char *data, int len, char header) {
    if (g_cfg.fec_adaptive)
        g_srv_rlnc_send.set_rate(g_srv_adaptive.data_shards,
                                 g_srv_adaptive.parity_shards);

    std::vector<std::vector<char> > frames;
    if (g_srv_rlnc_send.encode_packet(data, len, frames) != 0) {
        mylog(log_warn, "server RLNC encode failed len=%d\n", len);
        return;
    }
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].size() + 1 >= (size_t)buf_len) {
            mylog(log_warn, "server RLNC frame too large: %d\n", (int)frames[i].size());
            continue;
        }
        char packet[buf_len];
        int plen = (int)frames[i].size();
        memcpy(packet, frames[i].data(), frames[i].size());
        put_header(header, packet, plen);
        my_send(raw_dest, packet, plen);
    }
}

static void send_data_packet(conn_info_t &conn_info, char *data, int len, char header) {
    int desired = choose_tx_codec();
    if (desired != g_srv_tx_codec) {
        if (g_srv_tx_codec == FECRAW_CODEC_RS)
            from_normal_to_fec2(conn_info, raw_dest, 0, 0, header);
        else
            g_srv_rlnc_send.reset_window();
        g_srv_tx_codec = desired;
        mylog(log_info, "[server] tx codec -> %s\n", fecraw_codec_name(g_srv_tx_codec));
    }

    if (g_srv_tx_codec == FECRAW_CODEC_RLNC)
        send_rlnc_packet(data, len, header);
    else
        from_normal_to_fec2(conn_info, raw_dest, data, len, header);
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

    if (!fecraw_process_wire_input(conn_info, raw_dest, data, len,
                                    g_srv_adaptive, g_srv_sp_send, "server"))
        return;

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

    if (g_cfg.small_packet_threshold > 0 && small_packet_receiver_t::is_small_packet(data, len)) {
        char payload[buf_len];
        int plen = g_srv_sp_recv.receive(data, len, payload, sizeof(payload));
        if (plen > 0) {
            int wlen = write(tun_dest.inner.fd, payload, plen);
            (void)wlen;
        }
        return;
    }

    if (window_rlnc_receiver_t::is_frame(data, len)) {
        std::vector<std::vector<char> > packets;
        if (g_srv_rlnc_recv.receive(data, len, packets) != 0) {
            mylog(log_warn, "server RLNC decode failed\n");
            return;
        }
        for (size_t i = 0; i < packets.size(); ++i) {
            if (!packets[i].empty()) {
                int wlen = write(tun_dest.inner.fd, packets[i].data(), packets[i].size());
                (void)wlen;
            }
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

    if (g_cfg.small_packet_threshold > 0 && len < g_cfg.small_packet_threshold) {
        char frame[buf_len];
        int flen = g_srv_sp_send.build_frame(data, len, frame, sizeof(frame));
        if (flen > 0) {
            int redundancy = g_srv_sp_send.get_redundancy();
            for (int i = 0; i < redundancy; i++) {
                char packet[buf_len];
                int plen = flen;
                memcpy(packet, frame, flen);
                put_header(header_normal, packet, plen);
                my_send(raw_dest, packet, plen);
            }
            return;
        }
    }

    send_data_packet(conn_info, data, len, header_normal);
}

static void delay_manager_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    (void)loop; (void)watcher; (void)revents;
}

static void fec_encode_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    conn_info_t &conn_info = *((conn_info_t *)watcher->data);
    if (g_srv_tx_codec != FECRAW_CODEC_RS) return;
    from_normal_to_fec2(conn_info, raw_dest, 0, 0, header_normal);
}

static void conn_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    conn_info_t &conn_info = *((conn_info_t *)watcher->data);
    conn_info.stat.report_as_server(conn_info.addr);
    do_keep_alive(raw_dest);
}

static void feedback_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    (void)loop; (void)watcher; (void)revents;
    fecraw_flush_wire_feedback(raw_dest);
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
    g_fecraw_telemetry.init(true);

    auto tail = g_fec_par.get_tail();
    int base_data = (int)tail.x;
    int base_parity = (int)tail.y;
    g_srv_rlnc_send.init(g_cfg.rlnc_window, g_cfg.fec_mtu, base_data, base_parity);
    g_srv_rlnc_recv.init();
    g_srv_tx_codec = g_cfg.fec_codec == FECRAW_CODEC_RLNC ? FECRAW_CODEC_RLNC : FECRAW_CODEC_RS;

    if (g_cfg.fec_adaptive) {
        g_srv_adaptive.init(base_data, base_parity);
        mylog(log_info, "erasure-aware FEC enabled (base %d:%d)\n", base_data, base_parity);
    }
    if (g_cfg.small_packet_threshold > 0) {
        g_srv_sp_send.init(g_cfg.small_packet_threshold, g_cfg.small_packet_redundancy);
        g_srv_sp_recv.init();
        mylog(log_info, "small packet mode: threshold=%d redundancy=%d\n",
              g_cfg.small_packet_threshold, g_cfg.small_packet_redundancy);
    }
    if (g_cfg.enable_pacing) {
        g_fecraw_pacing.init(g_cfg.max_bandwidth);
        mylog(log_info, "erasure-aware pacing enabled (cold-start fail-open, max_bw=%lld)\n",
              (long long)g_cfg.max_bandwidth);
    }
    mylog(log_info, "fecraw wire protocol v2 telemetry enabled, codec=%s rlnc_window=%d\n",
          fecraw_codec_name(g_cfg.fec_codec), g_cfg.rlnc_window);

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

    ev_timer feedback_timer;
    ev_init(&feedback_timer, feedback_timer_cb);
    ev_timer_set(&feedback_timer, 0.010, 0.010);
    ev_timer_start(loop, &feedback_timer);

    struct ev_io fifo_watcher;
    if (fifo_file[0] != 0) {
        int fifo_fd = create_fifo(fifo_file);
        ev_io_init(&fifo_watcher, fifo_cb, fifo_fd, EV_READ);
        ev_io_start(loop, &fifo_watcher);
    }

    ev_prepare prepare_watcher;
    ev_init(&prepare_watcher, prepare_cb);
    ev_prepare_start(loop, &prepare_watcher);

    mylog(log_info, "fecraw server event loop started\n");
    ev_run(loop, 0);

    ev_loop_destroy(loop);
    myexit(0);
    return 0;
}
