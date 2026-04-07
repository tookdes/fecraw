/*
 * fecraw - Unified FEC VPN + TCP Disguise Tool
 *
 * Merges tinyfecVPN (TUN + FEC) with udp2raw (raw socket TCP disguise)
 * into a single binary.
 *
 * Architecture:
 *   Thread 1: App -> TUN -> FEC encode -> socketpair -> Thread 2
 *   Thread 2: socketpair -> AES encrypt -> fakeTCP wrap -> raw socket -> network
 *   (and reverse for incoming)
 *
 * Symbol isolation: udp2raw is compiled separately, all its symbols
 * are localized via objcopy, and accessed only through raw_api.h.
 */

#include "common.h"
#include "misc.h"
#include "fd_manager.h"
#include "delay_manager.h"

#include "fecraw_config.h"
#include "raw_api.h"

#include <getopt.h>
#include <signal.h>

static fecraw_config_t g_cfg;

static void print_usage() {
    printf("fecraw - Unified FEC VPN + TCP Disguise Tool\n");
    printf("  Combines tinyfecVPN + udp2raw into one binary.\n\n");
    printf("Usage:\n");
    printf("  fecraw --config <file.toml>              Load from TOML config\n");
    printf("  fecraw --gen-config-server <file.toml>   Generate server config template\n");
    printf("  fecraw --gen-config-client <file.toml>   Generate client config template\n");
    printf("  fecraw --tui                             Interactive TUI configuration\n\n");
    printf("Quick mode:\n");
    printf("  fecraw -s --listen 0.0.0.0:4096 --key passwd\n");
    printf("  fecraw -c --remote 1.2.3.4:4096 --key passwd\n\n");
    printf("Options:\n");
    printf("  -s, --server              Run as server\n");
    printf("  -c, --client              Run as client\n");
    printf("  --listen <ip:port>        Listen address\n");
    printf("  --remote <ip:port>        Remote server address\n");
    printf("  --key <string>            Encryption key\n");
    printf("  --raw-mode <mode>         faketcp(default), udp, icmp\n");
    printf("  --cipher <mode>           aes128cbc(default), aes128cfb, xor, none\n");
    printf("  --auth <mode>             hmac_sha1(default), md5, crc32, simple, none\n");
    printf("  --subnet <ip>             VPN subnet, default: 10.22.22.0\n");
    printf("  --tun-dev <name>          TUN device name\n");
    printf("  --tun-mtu <n>             TUN MTU, default: 1380\n");
    printf("  --fec <x:y>              FEC ratio, default: 20:10\n");
    printf("  --fec-timeout <ms>        FEC timeout, default: 8\n");
    printf("  --seq-mode <n>            TCP seq mode 0-4, default: 3\n");
    printf("  --auto-rule               Auto add iptables rules\n");
    printf("  --log-level <n>           0=never .. 5=debug\n");
    printf("  -h, --help                Print this help\n");
}

static void apply_fec_globals(fecraw_config_t &cfg) {
    program_mode = cfg.is_server ? server_mode : client_mode;
    working_mode = tun_dev_mode;

    char addr_buf[200];
    snprintf(addr_buf, sizeof(addr_buf), "%s:%d", cfg.listen_addr, cfg.listen_port);
    local_addr.from_str(addr_buf);

    if (strlen(cfg.remote_addr) > 0) {
        snprintf(addr_buf, sizeof(addr_buf), "%s:%d", cfg.remote_addr, cfg.remote_port);
        remote_addr.from_str(addr_buf);
    }

    log_level = cfg.log_level;
    keep_reconnect = cfg.keep_reconnect;

    strncpy(sub_net, cfg.subnet, sizeof(sub_net) - 1);
    sub_net_uint32 = inet_addr(sub_net);

    if (strlen(cfg.tun_dev) > 0)
        strncpy(tun_dev, cfg.tun_dev, sizeof(tun_dev) - 1);

    tun_mtu = cfg.tun_mtu;
    mssfix = cfg.mssfix;
    disable_fec = cfg.disable_fec;

    delay_manager.set_capacity(delay_capacity);
}

static void fill_raw_config(const fecraw_config_t &cfg, raw_api_config_t &rcfg) {
    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.is_server = cfg.is_server;
    rcfg.raw_mode = cfg.raw_mode;
    rcfg.cipher_mode = cfg.cipher_mode;
    rcfg.auth_mode = cfg.auth_mode;
    rcfg.seq_mode = cfg.seq_mode;
    rcfg.auto_iptables = cfg.auto_iptables;
    rcfg.disable_anti_replay = cfg.disable_anti_replay;
    rcfg.disable_bpf_filter = 0;
    rcfg.hb_mode = cfg.hb_mode;
    rcfg.hb_len = cfg.hb_len;
    rcfg.socket_buf_size = cfg.socket_buf_size * 1024;
    rcfg.log_level = cfg.log_level;
    rcfg.keep_reconnect = cfg.keep_reconnect;

    strncpy(rcfg.key, cfg.key, sizeof(rcfg.key) - 1);

    snprintf(rcfg.local_addr, sizeof(rcfg.local_addr), "%s:%d",
             cfg.listen_addr, cfg.listen_port);
    if (strlen(cfg.remote_addr) > 0)
        snprintf(rcfg.remote_addr, sizeof(rcfg.remote_addr), "%s:%d",
                 cfg.remote_addr, cfg.remote_port);
}

static int parse_cli(int argc, char *argv[], fecraw_config_t &cfg) {
    static struct option long_options[] = {
        {"config",            required_argument, 0, 'C'},
        {"gen-config-server", required_argument, 0, 'G'},
        {"gen-config-client", required_argument, 0, 'g'},
        {"tui",               no_argument,       0, 'T'},
        {"server",            no_argument,       0, 's'},
        {"client",            no_argument,       0, 'c'},
        {"listen",            required_argument, 0, 'l'},
        {"remote",            required_argument, 0, 'r'},
        {"key",               required_argument, 0, 'k'},
        {"raw-mode",          required_argument, 0, 1},
        {"cipher",            required_argument, 0, 2},
        {"auth",              required_argument, 0, 3},
        {"subnet",            required_argument, 0, 4},
        {"tun-dev",           required_argument, 0, 5},
        {"tun-mtu",           required_argument, 0, 6},
        {"fec",               required_argument, 0, 'f'},
        {"fec-timeout",       required_argument, 0, 8},
        {"seq-mode",          required_argument, 0, 9},
        {"auto-rule",         no_argument,       0, 'a'},
        {"keep-reconnect",    no_argument,       0, 10},
        {"log-level",         required_argument, 0, 11},
        {"disable-fec",       no_argument,       0, 12},
        {"help",              no_argument,       0, 'h'},
        {NULL, 0, 0, 0}
    };

    int opt, option_index = 0;
    while ((opt = getopt_long(argc, argv, "C:G:g:Tscl:r:k:f:ah", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'C':
                if (parse_toml_config(optarg, cfg) != 0) {
                    fprintf(stderr, "Failed to load config: %s\n", optarg);
                    return -1;
                }
                return 0;
            case 'G':
                generate_default_config(optarg, 1);
                printf("Generated server config: %s\n", optarg);
                exit(0);
            case 'g':
                generate_default_config(optarg, 0);
                printf("Generated client config: %s\n", optarg);
                exit(0);
            case 'T': return 2;
            case 's': cfg.is_server = 1; break;
            case 'c': cfg.is_server = 0; break;
            case 'l': {
                std::string s = optarg;
                size_t colon = s.rfind(':');
                if (colon != std::string::npos) {
                    strncpy(cfg.listen_addr, s.substr(0, colon).c_str(), sizeof(cfg.listen_addr) - 1);
                    cfg.listen_port = atoi(s.substr(colon + 1).c_str());
                }
                break;
            }
            case 'r': {
                std::string s = optarg;
                size_t colon = s.rfind(':');
                if (colon != std::string::npos) {
                    strncpy(cfg.remote_addr, s.substr(0, colon).c_str(), sizeof(cfg.remote_addr) - 1);
                    cfg.remote_port = atoi(s.substr(colon + 1).c_str());
                }
                break;
            }
            case 'k': strncpy(cfg.key, optarg, sizeof(cfg.key) - 1); break;
            case 'f': strncpy(cfg.fec_str, optarg, sizeof(cfg.fec_str) - 1); break;
            case 'a': cfg.auto_iptables = 1; break;
            case 1:
                if (strcmp(optarg, "faketcp") == 0) cfg.raw_mode = 0;
                else if (strcmp(optarg, "udp") == 0) cfg.raw_mode = 1;
                else if (strcmp(optarg, "icmp") == 0) cfg.raw_mode = 2;
                break;
            case 2:
                if (strcmp(optarg, "aes128cfb") == 0) cfg.cipher_mode = 0;
                else if (strcmp(optarg, "aes128cbc") == 0) cfg.cipher_mode = 1;
                else if (strcmp(optarg, "xor") == 0) cfg.cipher_mode = 2;
                else if (strcmp(optarg, "none") == 0) cfg.cipher_mode = 3;
                break;
            case 3:
                if (strcmp(optarg, "hmac_sha1") == 0) cfg.auth_mode = 0;
                else if (strcmp(optarg, "md5") == 0) cfg.auth_mode = 1;
                else if (strcmp(optarg, "crc32") == 0) cfg.auth_mode = 2;
                else if (strcmp(optarg, "simple") == 0) cfg.auth_mode = 3;
                else if (strcmp(optarg, "none") == 0) cfg.auth_mode = 4;
                break;
            case 4: strncpy(cfg.subnet, optarg, sizeof(cfg.subnet) - 1); break;
            case 5: strncpy(cfg.tun_dev, optarg, sizeof(cfg.tun_dev) - 1); break;
            case 6: cfg.tun_mtu = atoi(optarg); break;
            case 8: cfg.fec_timeout = atoi(optarg); break;
            case 9: cfg.seq_mode = atoi(optarg); break;
            case 10: cfg.keep_reconnect = 1; break;
            case 11: cfg.log_level = atoi(optarg); break;
            case 12: cfg.disable_fec = 1; break;
            case 'h': print_usage(); exit(0);
            default: print_usage(); exit(1);
        }
    }

    if (cfg.is_server < 0) {
        fprintf(stderr, "Error: specify -s (server) or -c (client)\n");
        print_usage();
        return -1;
    }
    return 0;
}

static void sigpipe_cb(struct ev_loop *l, ev_signal *w, int revents) {
    mylog(log_info, "got sigpipe, ignored");
}
static void sigterm_cb(struct ev_loop *l, ev_signal *w, int revents) {
    mylog(log_info, "got sigterm, exit");
    myexit(0);
}
static void sigint_cb(struct ev_loop *l, ev_signal *w, int revents) {
    mylog(log_info, "got sigint, exit");
    myexit(0);
}

int fecraw_client_event_loop();
int fecraw_server_event_loop();

int main(int argc, char *argv[]) {
    assert(sizeof(unsigned short) == 2);
    assert(sizeof(unsigned int) == 4);
    assert(sizeof(unsigned long long) == 8);

    dup2(1, 2);

    if (argc == 1) {
        print_usage();
        return 1;
    }

    int ret = parse_cli(argc, argv, g_cfg);
    if (ret < 0) return 1;
    if (ret == 2) {
        execlp("fecraw-tui", "fecraw-tui", NULL);
        fprintf(stderr, "fecraw-tui not found\n");
        return 1;
    }

    apply_fec_globals(g_cfg);

    raw_api_config_t rcfg;
    fill_raw_config(g_cfg, rcfg);
    raw_api_init(&rcfg);
    raw_api_setup();

    struct ev_loop *loop = ev_default_loop(0);

    ev_signal signal_watcher_sigpipe;
    ev_signal_init(&signal_watcher_sigpipe, sigpipe_cb, SIGPIPE);
    ev_signal_start(loop, &signal_watcher_sigpipe);

    ev_signal signal_watcher_sigterm;
    ev_signal_init(&signal_watcher_sigterm, sigterm_cb, SIGTERM);
    ev_signal_start(loop, &signal_watcher_sigterm);

    ev_signal signal_watcher_sigint;
    ev_signal_init(&signal_watcher_sigint, sigint_cb, SIGINT);
    ev_signal_start(loop, &signal_watcher_sigint);

    if (geteuid() != 0) {
        mylog(log_warn, "running as non-root; raw sockets and TUN require root or CAP_NET_RAW+CAP_NET_ADMIN\n");
    }

    mylog(log_info, "fecraw starting in %s mode\n", g_cfg.is_server ? "server" : "client");
    mylog(log_info, "raw_mode=%s fec=%s\n",
          raw_api_mode_name(g_cfg.raw_mode), g_cfg.fec_str);

    sub_net_uint32 = inet_addr(sub_net);

    if (strlen(tun_dev) == 0)
        sprintf(tun_dev, "fecraw%u", get_fake_random_number() % 1000);

    mylog(log_info, "using TUN interface %s\n", tun_dev);

    if (program_mode == client_mode)
        fecraw_client_event_loop();
    else
        fecraw_server_event_loop();

    return 0;
}
