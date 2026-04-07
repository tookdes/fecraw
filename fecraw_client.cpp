/*
 * fecraw_client.cpp - Unified client event loop
 *
 * Architecture: Two threads in one process, connected by socketpair.
 *   Thread 1 (main): TUN <-> FEC encode/decode <-> socketpair[0]
 *   Thread 2 (raw):  socketpair[1] <-> udp2raw encrypt/decrypt <-> raw socket
 *
 * Thread 2 runs udp2raw's client_event_loop() via the raw_api, with the
 * socketpair fd replacing its normal UDP fd.  All udp2raw symbols are
 * localized, so there are no naming conflicts with UDPspeeder.
 */

#include "common.h"
#include "connection.h"
#include "misc.h"
#include "fd_manager.h"
#include "delay_manager.h"
#include "../tinyfecVPN/tun_dev.h"
#include "raw_api.h"

#include <pthread.h>
#include <sys/socket.h>

static int got_feed_back = 0;
static dest_t raw_dest;
static dest_t tun_dest;
static int bridge_fec_fd = -1;

static void *raw_thread_func(void *arg) {
    int fd = *(int *)arg;
    raw_api_client_loop(fd);
    return NULL;
}

static void bridge_recv_cb(struct ev_loop *loop, struct ev_io *watcher, int revents) {
    assert(!(revents & EV_ERROR));
    conn_info_t &conn_info = *((conn_info_t *)watcher->data);

    char data[buf_len];
    int len = recv(bridge_fec_fd, data, max_data_len + 1, 0);

    if (len == max_data_len + 1) {
        mylog(log_warn, "huge packet, data_len > %d, dropped\n", max_data_len);
        return;
    }
    if (len < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        mylog(log_warn, "bridge recv return %d, errno=%s\n", len, strerror(errno));
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

    if (header == header_reject) {
        if (keep_reconnect == 0) {
            mylog(log_fatal, "server rejected, exiting\n");
            myexit(-1);
        } else {
            if (got_feed_back) mylog(log_warn, "server restarted, reconnecting\n");
            got_feed_back = 0;
        }
        return;
    } else if (header == header_normal) {
        if (!got_feed_back) mylog(log_info, "connection accepted by server\n");
        got_feed_back = 1;
    } else {
        mylog(log_warn, "invalid header %d\n", int(header));
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

    char header = got_feed_back ? header_normal : header_new_connect;
    from_normal_to_fec2(conn_info, raw_dest, data, len, header);
}

static void delay_manager_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    (void)loop; (void)watcher; (void)revents;
}

static void fec_encode_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    conn_info_t &conn_info = *((conn_info_t *)watcher->data);
    char header = got_feed_back ? header_normal : header_new_connect;
    from_normal_to_fec2(conn_info, raw_dest, 0, 0, header);
}

static void conn_timer_cb(struct ev_loop *loop, struct ev_timer *watcher, int revents) {
    conn_info_t &conn_info = *((conn_info_t *)watcher->data);
    conn_info.stat.report_as_client();
    if (got_feed_back) do_keep_alive(raw_dest);
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

int fecraw_client_event_loop() {
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
                htonl((ntohl(sub_net_uint32) & 0xFFFFFF00) | 2),
                htonl((ntohl(sub_net_uint32) & 0xFFFFFF00) | 1),
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

    mylog(log_info, "fecraw client event loop started\n");
    ev_run(loop, 0);

    ev_loop_destroy(loop);
    myexit(0);
    return 0;
}
