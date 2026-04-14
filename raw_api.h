/*
 * raw_api.h - C-linkage API to access udp2raw functionality
 *
 * This header provides a clean boundary between the fecraw/UDPspeeder world
 * and the udp2raw world. udp2raw is built as a shared library with hidden
 * visibility; only these extern "C" functions are exported.
 */

#ifndef FECRAW_RAW_API_H_
#define FECRAW_RAW_API_H_

#ifdef RAW_API_BUILDING_SO
#define RAW_API __attribute__((visibility("default")))
#else
#define RAW_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct raw_api_config_t {
    int is_server;             /* 1=server, 0=client */
    int raw_mode;              /* 0=faketcp, 1=udp, 2=icmp */
    int cipher_mode;           /* 0=aes128cfb, 1=aes128cbc, 2=xor, 3=none, 4=aes256gcm, 5=chacha20poly1305 */
    int auth_mode;             /* 0=hmac_sha1, 1=md5, 2=crc32, 3=simple, 4=none */
    int seq_mode;              /* 0..4 */
    int auto_iptables;         /* auto add iptables rule */
    int disable_anti_replay;
    int disable_bpf_filter;
    int hb_mode;               /* heartbeat mode */
    int hb_len;                /* heartbeat payload length */
    int socket_buf_size;       /* in bytes */
    int log_level;             /* 0=never .. 5=debug */
    int keep_reconnect;
    char key[1000];            /* encryption passphrase */
    char local_addr[200];      /* "ip:port" for listen */
    char remote_addr[200];     /* "ip:port" for remote */
};

/*
 * Initialize udp2raw globals from config.
 * Must be called before raw_api_setup() and before launching loops.
 */
RAW_API void raw_api_init(const struct raw_api_config_t *cfg);

RAW_API void raw_api_setup(void);

RAW_API int raw_api_client_loop(int bridge_fd);

RAW_API int raw_api_server_loop(int bridge_fd);

RAW_API unsigned int raw_api_get_true_random_nz(void);

RAW_API const char *raw_api_mode_name(int mode);

#ifdef __cplusplus
}
#endif

#endif /* FECRAW_RAW_API_H_ */
