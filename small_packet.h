#ifndef FECRAW_SMALL_PACKET_H_
#define FECRAW_SMALL_PACKET_H_

/*
 * Small-packet redundancy handler -- ported from NRUP nrup.go
 *
 * For interactive traffic (SSH keystrokes, game packets, DNS, etc.)
 * waiting to fill an entire FEC block adds unacceptable latency.
 * Instead, small packets are sent immediately with N-fold redundancy.
 * The receiver deduplicates using a sliding sequence-number window.
 *
 * Wire format:  [1B marker=0xFE][4B seq BE][payload]
 */

#include <cstdint>
#include <cstring>
#include <map>
#include <pthread.h>

static const uint8_t SMALL_PKT_MARKER = 0xFE;
static const int     SMALL_PKT_HEADER = 5;  // 1 marker + 4 seq
static const int     SMALL_PKT_MAX_SEEN = 1000;
static const int     SMALL_PKT_EVICT_TO = 500;

struct small_packet_sender_t {
    uint32_t seq;
    int      threshold;     // bytes; packets below this use redundancy path
    int      base_redundancy;
    double   smoothed_loss; // EWMA smoothed loss rate
    double   alpha;         // EWMA coefficient

    void init(int thresh, int redundancy) {
        seq = 0;
        threshold = thresh;
        base_redundancy = redundancy;
        smoothed_loss = 0.0;
        alpha = 0.28;
    }

    void update_loss(double current_loss_rate) {
        smoothed_loss = (1.0 - alpha) * smoothed_loss + alpha * current_loss_rate;
    }

    int get_redundancy() const {
        if (smoothed_loss > 0.50) return 5;
        if (smoothed_loss > 0.35) return 4;
        if (smoothed_loss > 0.20) return 3;
        return base_redundancy;
    }

    /*
     * Build a small-packet frame into `out`.
     * Returns total frame length, or 0 if packet is too large for this path.
     */
    int build_frame(const char *payload, int payload_len, char *out, int out_cap) {
        if (payload_len >= threshold) return 0;
        int frame_len = SMALL_PKT_HEADER + payload_len;
        if (frame_len > out_cap) return 0;

        uint32_t s = __sync_fetch_and_add(&seq, 1);
        out[0] = (char)SMALL_PKT_MARKER;
        out[1] = (char)(s >> 24);
        out[2] = (char)(s >> 16);
        out[3] = (char)(s >> 8);
        out[4] = (char)(s);
        memcpy(out + SMALL_PKT_HEADER, payload, payload_len);
        return frame_len;
    }
};

struct small_packet_receiver_t {
    std::map<uint32_t, bool> seen;
    pthread_mutex_t mu;

    void init() {
        pthread_mutex_init(&mu, NULL);
    }

    void destroy() {
        pthread_mutex_destroy(&mu);
    }

    static bool is_small_packet(const char *data, int len) {
        return len >= SMALL_PKT_HEADER && (uint8_t)data[0] == SMALL_PKT_MARKER;
    }

    /*
     * Parse and deduplicate. Returns payload length into `out_payload`,
     * or -1 if this is a duplicate.
     */
    int receive(const char *data, int len, char *out_payload, int out_cap) {
        if (!is_small_packet(data, len)) return -1;

        uint32_t s = ((uint32_t)(uint8_t)data[1] << 24) |
                     ((uint32_t)(uint8_t)data[2] << 16) |
                     ((uint32_t)(uint8_t)data[3] << 8)  |
                     ((uint32_t)(uint8_t)data[4]);

        pthread_mutex_lock(&mu);
        if (seen.count(s)) {
            pthread_mutex_unlock(&mu);
            return -1;  // duplicate
        }
        seen[s] = true;
        if ((int)seen.size() > SMALL_PKT_MAX_SEEN) {
            auto it = seen.begin();
            while ((int)seen.size() > SMALL_PKT_EVICT_TO) {
                it = seen.erase(it);
            }
        }
        pthread_mutex_unlock(&mu);

        int payload_len = len - SMALL_PKT_HEADER;
        if (payload_len > out_cap) payload_len = out_cap;
        memcpy(out_payload, data + SMALL_PKT_HEADER, payload_len);
        return payload_len;
    }
};

#endif /* FECRAW_SMALL_PACKET_H_ */
