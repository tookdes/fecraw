#ifndef FECRAW_CONFIG_H_
#define FECRAW_CONFIG_H_

#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cctype>

struct fecraw_config_t {
    // general
    int is_server;          // 0=client 1=server
    char key[1000];

    // network (raw transport)
    char listen_addr[100];
    int listen_port;
    char remote_addr[100];
    int remote_port;
    int raw_mode;           // 0=faketcp, 1=udp, 2=icmp
    int cipher_mode;        // 0=aes128cfb, 1=aes128cbc, 2=xor, 3=none, 4=aes256gcm, 5=chacha20poly1305, 6=auto
    int auth_mode;          // 0=hmac_sha1, 1=md5, 2=crc32, 3=simple, 4=none (auto-none for AEAD ciphers)

    // vpn
    char subnet[32];
    char tun_dev[32];
    int tun_mtu;
    int mssfix;
    int persist_tun;
    int manual_set_tun;

    // fec
    int fec_mode;           // 0=blob, 1=per-packet
    char fec_str[64];       // e.g. "20:10"
    int fec_timeout;        // ms
    int fec_mtu;
    int disable_fec;
    int fec_adaptive;       // 1=enable adaptive FEC ratio
    int small_packet_threshold; // bytes; packets below bypass FEC (0=disabled)
    int small_packet_redundancy; // base redundancy for small packets

    // advanced (raw)
    int seq_mode;
    int auto_iptables;
    int keep_reconnect;
    int log_level;
    int disable_anti_replay;
    int disable_bpf;
    int socket_buf_size;
    int hb_mode;
    int hb_len;
    int enable_pacing;      // 1=enable BBR pacing
    long long max_bandwidth; // bytes/s hard cap (0=unlimited)

    fecraw_config_t() {
        is_server = -1;
        strcpy(key, "");
        strcpy(listen_addr, "0.0.0.0");
        listen_port = 4096;
        strcpy(remote_addr, "");
        remote_port = 4096;
        raw_mode = 0;
        cipher_mode = 1;
        auth_mode = 0;

        strcpy(subnet, "10.22.22.0");
        strcpy(tun_dev, "");
        tun_mtu = 1380;
        mssfix = 1300;
        persist_tun = 0;
        manual_set_tun = 0;

        fec_mode = 0;
        strcpy(fec_str, "20:10");
        fec_timeout = 8;
        fec_mtu = 1250;
        disable_fec = 0;
        fec_adaptive = 0;
        small_packet_threshold = 0;
        small_packet_redundancy = 2;

        seq_mode = 3;
        auto_iptables = 1;
        keep_reconnect = 1;
        log_level = 4;
        disable_anti_replay = 0;
        disable_bpf = 0;
        socket_buf_size = 1024;
        hb_mode = 1;
        hb_len = 1200;
        enable_pacing = 0;
        max_bandwidth = 0;
    }
};

static inline std::string trim_ws(const std::string &s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static inline std::string strip_quotes(const std::string &s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

static inline std::string strip_inline_comment(const std::string &s) {
    bool in_quote = false;
    bool escaped = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_quote && c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') {
            in_quote = !in_quote;
            continue;
        }
        if (!in_quote && c == '#')
            return trim_ws(s.substr(0, i));
    }
    return trim_ws(s);
}

static inline std::string to_lower_ascii(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = (char)std::tolower((unsigned char)s[i]);
    }
    return s;
}

static inline int parse_bool_like(const std::string &raw, int &out) {
    std::string v = to_lower_ascii(trim_ws(raw));
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        out = 1;
        return 0;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        out = 0;
        return 0;
    }
    return -1;
}

static int parse_toml_config(const char *path, fecraw_config_t &cfg) {
    std::ifstream f(path);
    if (!f.is_open()) return -1;

    std::string line, section;
    while (std::getline(f, line)) {
        line = strip_inline_comment(line);
        if (line.empty() || line[0] == '#') continue;

        if (line[0] == '[') {
            size_t end = line.find(']');
            if (end != std::string::npos)
                section = line.substr(1, end - 1);
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim_ws(line.substr(0, eq));
        std::string val = strip_inline_comment(line.substr(eq + 1));
        val = strip_quotes(val);

        if (section == "general") {
            if (key == "mode") {
                if (val == "server") cfg.is_server = 1;
                else if (val == "client") cfg.is_server = 0;
                else {
                    fprintf(stderr, "Invalid mode in [general]: %s (expected server/client)\n", val.c_str());
                    return -1;
                }
            }
            else if (key == "key") strncpy(cfg.key, val.c_str(), sizeof(cfg.key) - 1);
        }
        else if (section == "network") {
            if (key == "listen") {
                size_t colon = val.rfind(':');
                if (colon != std::string::npos) {
                    strncpy(cfg.listen_addr, val.substr(0, colon).c_str(), sizeof(cfg.listen_addr) - 1);
                    cfg.listen_port = atoi(val.substr(colon + 1).c_str());
                }
            }
            else if (key == "remote") {
                size_t colon = val.rfind(':');
                if (colon != std::string::npos) {
                    strncpy(cfg.remote_addr, val.substr(0, colon).c_str(), sizeof(cfg.remote_addr) - 1);
                    cfg.remote_port = atoi(val.substr(colon + 1).c_str());
                }
            }
            else if (key == "raw_mode") {
                if (val == "faketcp") cfg.raw_mode = 0;
                else if (val == "udp") cfg.raw_mode = 1;
                else if (val == "icmp") cfg.raw_mode = 2;
            }
            else if (key == "cipher") {
                if (val == "aes128cfb") cfg.cipher_mode = 0;
                else if (val == "aes128cbc") cfg.cipher_mode = 1;
                else if (val == "xor") cfg.cipher_mode = 2;
                else if (val == "none") cfg.cipher_mode = 3;
                else if (val == "aes256gcm") cfg.cipher_mode = 4;
                else if (val == "chacha20poly1305") cfg.cipher_mode = 5;
                else if (val == "auto") cfg.cipher_mode = 6;
            }
            else if (key == "auth") {
                if (val == "hmac_sha1") cfg.auth_mode = 0;
                else if (val == "md5") cfg.auth_mode = 1;
                else if (val == "crc32") cfg.auth_mode = 2;
                else if (val == "simple") cfg.auth_mode = 3;
                else if (val == "none") cfg.auth_mode = 4;
            }
        }
        else if (section == "vpn") {
            if (key == "subnet") strncpy(cfg.subnet, val.c_str(), sizeof(cfg.subnet) - 1);
            else if (key == "tun_dev") strncpy(cfg.tun_dev, val.c_str(), sizeof(cfg.tun_dev) - 1);
            else if (key == "tun_mtu") cfg.tun_mtu = atoi(val.c_str());
        }
        else if (section == "fec") {
            if (key == "mode") cfg.fec_mode = atoi(val.c_str());
            else if (key == "fec") strncpy(cfg.fec_str, val.c_str(), sizeof(cfg.fec_str) - 1);
            else if (key == "timeout") cfg.fec_timeout = atoi(val.c_str());
            else if (key == "mtu") cfg.fec_mtu = atoi(val.c_str());
            else if (key == "adaptive") {
                int b = 0;
                if (parse_bool_like(val, b) == 0) cfg.fec_adaptive = b;
            }
            else if (key == "small_packet_threshold") cfg.small_packet_threshold = atoi(val.c_str());
            else if (key == "small_packet_redundancy") cfg.small_packet_redundancy = atoi(val.c_str());
        }
        else if (section == "advanced") {
            if (key == "seq_mode") cfg.seq_mode = atoi(val.c_str());
            else if (key == "auto_iptables") {
                int b = 0;
                if (parse_bool_like(val, b) == 0) cfg.auto_iptables = b;
                else cfg.auto_iptables = atoi(val.c_str()) != 0;
            }
            else if (key == "keep_reconnect") {
                int b = 0;
                if (parse_bool_like(val, b) == 0) cfg.keep_reconnect = b;
                else cfg.keep_reconnect = atoi(val.c_str()) != 0;
            }
            else if (key == "log_level") cfg.log_level = atoi(val.c_str());
            else if (key == "enable_pacing") {
                int b = 0;
                if (parse_bool_like(val, b) == 0) cfg.enable_pacing = b;
            }
            else if (key == "max_bandwidth") cfg.max_bandwidth = atoll(val.c_str());
        }
    }
    return 0;
}

static void generate_default_config(const char *path, int is_server) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "[general]\n");
    fprintf(f, "mode = \"%s\"\n", is_server ? "server" : "client");
    fprintf(f, "key = \"change_me\"\n\n");
    fprintf(f, "[network]\n");
    if (is_server) {
        fprintf(f, "listen = \"0.0.0.0:4096\"\n");
        fprintf(f, "remote = \"127.0.0.1:7777\"\n");
    } else {
        fprintf(f, "listen = \"0.0.0.0:3333\"\n");
        fprintf(f, "remote = \"YOUR_SERVER_IP:4096\"\n");
    }
    fprintf(f, "raw_mode = \"faketcp\"\n");
    fprintf(f, "cipher = \"aes256gcm\"        # aes128cbc, aes128cfb, xor, none, aes256gcm, chacha20poly1305, auto\n");
    fprintf(f, "auth = \"none\"               # auto-none for AEAD ciphers (aes256gcm/chacha20poly1305)\n\n");
    fprintf(f, "[vpn]\n");
    fprintf(f, "subnet = \"10.22.22.0\"\n");
    fprintf(f, "tun_dev = \"tun0\"\n");
    fprintf(f, "tun_mtu = 1380\n\n");
    fprintf(f, "[fec]\n");
    fprintf(f, "mode = 0\n");
    fprintf(f, "fec = \"20:10\"\n");
    fprintf(f, "timeout = 8\n");
    fprintf(f, "mtu = 1250\n");
    fprintf(f, "adaptive = false             # auto-adjust FEC ratio based on loss\n");
    fprintf(f, "small_packet_threshold = 0   # bytes; 0=disabled, 256=recommended for SSH/gaming\n");
    fprintf(f, "small_packet_redundancy = 2  # base redundancy for small packets\n\n");
    fprintf(f, "[advanced]\n");
    fprintf(f, "seq_mode = 3\n");
    fprintf(f, "auto_iptables = true\n");
    fprintf(f, "keep_reconnect = true\n");
    fprintf(f, "log_level = 4\n");
    fprintf(f, "enable_pacing = false        # BBR-lite traffic shaping\n");
    fprintf(f, "max_bandwidth = 0            # bytes/s hard cap, 0=unlimited\n");
    fclose(f);
}

#endif
