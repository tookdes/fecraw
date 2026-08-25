#ifndef FECRAW_PACING_H_
#define FECRAW_PACING_H_

/*
 * Erasure-aware BBR-lite pacer.
 *
 * Liveness rule: pacing is fail-open. A sender remains uncapped long enough to
 * observe one RTT of genuine delivery samples before it closes bootstrap. The
 * bandwidth estimate is a time-windowed max rather than a handful of ACK
 * samples, so pacing its own output down cannot immediately redefine path
 * capacity downward. STARTUP and PROBE_BW advance on RTT-scale rounds, not on
 * individual ACK callbacks.
 */

#include "telemetry.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <time.h>

enum bbr_state_t {
    BBR_STARTUP = 0,
    BBR_DRAIN,
    BBR_PROBE_BW,
    BBR_PROBE_RTT
};

static const double  BBR_STARTUP_GAIN = 2.0;
static const double  BBR_DRAIN_GAIN = 0.70;
static const int64_t BBR_INIT_CWND = 64 * 1024;
static const int64_t BBR_MIN_CWND = 16 * 1024;
static const int64_t BBR_PROBE_RTT_CWND = 4 * 1500;
static const double  BBR_PROBE_BW_GAINS[] = {1.20, 0.85, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
static const int     BBR_BW_BUCKETS = 32;
static const double  BBR_BW_BUCKET_S = 0.25; // eight-second max filter

struct pacing_t {
    pthread_mutex_t mu;

    int64_t max_wire_bw;
    double min_rtt_s;
    double smoothed_rtt_s;
    volatile int64_t bytes_in_flight;
    int64_t cwnd;
    int64_t pacing_rate;
    int64_t max_bandwidth;     // bytes/s hard cap; 0 means unlimited
    double next_send_s;

    bbr_state_t state;
    int cycle_idx;
    int full_bw_rounds;
    int64_t full_bw;
    int64_t startup_round_bw;
    double startup_round_started_s;
    double state_started_s;
    double probe_cycle_started_s;
    double last_probe_rtt;
    double congestion_scale;
    bool enabled;

    bool feedback_ready;
    int feedback_samples;
    double bootstrap_started_s;
    double last_feedback_s;

    int64_t bw_buckets[BBR_BW_BUCKETS];
    int bw_bucket_idx;
    double bw_bucket_started_s;

    static double now_s() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    }

    void init(int64_t max_bw_limit) {
        pthread_mutex_init(&mu, NULL);
        max_wire_bw = 0;
        min_rtt_s = 1e9;
        smoothed_rtt_s = 0;
        bytes_in_flight = 0;
        cwnd = BBR_INIT_CWND;
        pacing_rate = 0;
        max_bandwidth = max_bw_limit;
        next_send_s = now_s();
        state = BBR_STARTUP;
        cycle_idx = 0;
        full_bw_rounds = 0;
        full_bw = 0;
        startup_round_bw = 0;
        startup_round_started_s = 0;
        state_started_s = next_send_s;
        probe_cycle_started_s = next_send_s;
        last_probe_rtt = next_send_s;
        congestion_scale = 1.0;
        enabled = true;
        feedback_ready = false;
        feedback_samples = 0;
        bootstrap_started_s = 0;
        last_feedback_s = 0;
        std::memset(bw_buckets, 0, sizeof(bw_buckets));
        bw_bucket_idx = 0;
        bw_bucket_started_s = 0;
    }

    void destroy() { pthread_mutex_destroy(&mu); }

    bool has_feedback() const { return feedback_ready; }

    uint64_t reserve_delay_us(int size) {
        if (!enabled || size <= 0) return 0;
        pthread_mutex_lock(&mu);

        double now = now_s();
        maybe_fail_open(now);
        bytes_in_flight += size;

        if (!feedback_ready || pacing_rate <= 0) {
            pthread_mutex_unlock(&mu);
            return 0;
        }

        double queue_guard = 0.100;
        if (smoothed_rtt_s > 0)
            queue_guard = std::max(0.050, std::min(0.500, smoothed_rtt_s * 2.0));
        if (next_send_s < now || next_send_s - now > queue_guard)
            next_send_s = now;

        double send_at = next_send_s;
        next_send_s += (double)size / (double)pacing_rate;
        double delay = send_at - now;
        pthread_mutex_unlock(&mu);

        if (delay <= 0) return 0;
        double us = delay * 1e6;
        if (us >= (double)UINT64_MAX) return UINT64_MAX;
        return (uint64_t)us;
    }

    void cancel_reserved(int size) {
        if (!enabled || size <= 0) return;
        pthread_mutex_lock(&mu);
        bytes_in_flight -= size;
        if (bytes_in_flight < 0) bytes_in_flight = 0;
        pthread_mutex_unlock(&mu);
    }

    void on_feedback(uint64_t acked_bytes, uint64_t decided_bytes,
                     double rtt_s, double delivered_rate,
                     bool delivery_sampled, const loss_snapshot_t &loss) {
        if (!enabled) return;
        pthread_mutex_lock(&mu);

        double now = now_s();
        last_feedback_s = now;

        if (decided_bytes > 0) {
            bytes_in_flight -= (int64_t)decided_bytes;
            if (bytes_in_flight < 0) bytes_in_flight = 0;
        }

        if (rtt_s > 0) {
            if (rtt_s < min_rtt_s) min_rtt_s = rtt_s;
            if (smoothed_rtt_s <= 0) smoothed_rtt_s = rtt_s;
            else smoothed_rtt_s = smoothed_rtt_s * 0.875 + rtt_s * 0.125;
        }

        rotate_bw_buckets(now);

        if (delivery_sampled && delivered_rate > 0 && acked_bytes > 0) {
            double arrival = 1.0;
            if (loss.floor_trusted) {
                arrival = 1.0 - loss.floor;
                if (arrival < 0.15) arrival = 0.15;
            }
            int64_t wire_sample = (int64_t)(delivered_rate / arrival);
            if (wire_sample < 1) wire_sample = 1;
            record_bandwidth_sample(wire_sample, now);

            if (bootstrap_started_s <= 0) bootstrap_started_s = now;
            ++feedback_samples;

            // Two samples fixed the original process-start dilution bug, but
            // the real Ali/GCP test showed that two ACK intervals are still too
            // little evidence: an unlucky low pair closes the pacer around its
            // own output. Stay fail-open for one measured RTT and at least four
            // genuine slopes so the initial max can see the unconstrained path.
            if (!feedback_ready && feedback_samples >= 4 &&
                smoothed_rtt_s > 0 && max_wire_bw > 0 &&
                now - bootstrap_started_s >= bootstrap_wait_s()) {
                feedback_ready = true;
                next_send_s = now;
                bytes_in_flight = 0;
                enter_state(BBR_STARTUP, now);
                full_bw = 0;
                full_bw_rounds = 0;
                startup_round_bw = wire_sample;
                startup_round_started_s = now;
            } else if (feedback_ready && state == BBR_STARTUP) {
                advance_startup(wire_sample, now);
            }

            // burst_factor describes correlation of erasures, not congestion.
            // Stage 3 incorrectly used burst_factor>1.6 as a congestion signal;
            // the real E2 rate/wire_bw ~= 0.425 is exactly 0.85 probe gain times
            // the resulting 0.5 scale. Only excess loss above a trusted floor
            // is allowed to reduce the sending rate here.
            if (loss.floor_trusted && loss.congestive > 0.02) {
                congestion_scale *= 0.90;
                if (congestion_scale < 0.50) congestion_scale = 0.50;
            } else {
                congestion_scale *= 1.05;
                if (congestion_scale > 1.0) congestion_scale = 1.0;
            }
        }

        advance_periodic_state(now);
        update_limits();
        pthread_mutex_unlock(&mu);
    }

    const char *state_name() const {
        static const char *names[] = {"startup", "drain", "probe_bw", "probe_rtt"};
        return names[state];
    }

private:
    double control_round_s() const {
        double rtt = min_rtt_s < 1e8 ? min_rtt_s : smoothed_rtt_s;
        if (rtt <= 0) rtt = 0.10;
        return std::max(0.050, std::min(0.500, rtt));
    }

    double bootstrap_wait_s() const {
        double rtt = smoothed_rtt_s > 0 ? smoothed_rtt_s : 0.10;
        return std::max(0.050, std::min(0.500, rtt));
    }

    void recompute_max_bw() {
        max_wire_bw = 0;
        for (int i = 0; i < BBR_BW_BUCKETS; ++i)
            if (bw_buckets[i] > max_wire_bw) max_wire_bw = bw_buckets[i];
    }

    void rotate_bw_buckets(double now) {
        if (bw_bucket_started_s <= 0) {
            bw_bucket_started_s = now;
            return;
        }
        if (now <= bw_bucket_started_s) return;
        int steps = (int)((now - bw_bucket_started_s) / BBR_BW_BUCKET_S);
        if (steps <= 0) return;
        if (steps >= BBR_BW_BUCKETS) {
            std::memset(bw_buckets, 0, sizeof(bw_buckets));
            bw_bucket_idx = 0;
            bw_bucket_started_s = now;
        } else {
            for (int i = 0; i < steps; ++i) {
                bw_bucket_idx = (bw_bucket_idx + 1) % BBR_BW_BUCKETS;
                bw_buckets[bw_bucket_idx] = 0;
            }
            bw_bucket_started_s += (double)steps * BBR_BW_BUCKET_S;
        }
        recompute_max_bw();
    }

    void record_bandwidth_sample(int64_t wire_sample, double now) {
        rotate_bw_buckets(now);
        if (wire_sample > bw_buckets[bw_bucket_idx])
            bw_buckets[bw_bucket_idx] = wire_sample;
        if (wire_sample > max_wire_bw) max_wire_bw = wire_sample;
    }

    void enter_state(bbr_state_t next, double now) {
        state = next;
        state_started_s = now;
        if (next == BBR_PROBE_BW) {
            cycle_idx = 0;
            probe_cycle_started_s = now;
        }
    }

    void advance_startup(int64_t wire_sample, double now) {
        if (startup_round_started_s <= 0) {
            startup_round_started_s = now;
            startup_round_bw = wire_sample;
            return;
        }
        if (wire_sample > startup_round_bw) startup_round_bw = wire_sample;
        double round = control_round_s();
        if (now - startup_round_started_s < round) return;

        if (full_bw == 0 || startup_round_bw > full_bw * 5 / 4) {
            full_bw = startup_round_bw;
            full_bw_rounds = 0;
        } else {
            ++full_bw_rounds;
        }

        bool rtt_inflated = min_rtt_s < 1e8 && smoothed_rtt_s > min_rtt_s * 1.50;
        if (full_bw_rounds >= 3 || (rtt_inflated && full_bw_rounds >= 1))
            enter_state(BBR_DRAIN, now);

        startup_round_started_s = now;
        startup_round_bw = 0;
    }

    void advance_periodic_state(double now) {
        if (!feedback_ready) return;

        double round = control_round_s();
        if (state == BBR_DRAIN) {
            if (bytes_in_flight <= bdp() || now - state_started_s >= round * 2.0)
                enter_state(BBR_PROBE_BW, now);
        } else if (state == BBR_PROBE_BW) {
            if (now - probe_cycle_started_s >= round) {
                int steps = (int)((now - probe_cycle_started_s) / round);
                if (steps < 1) steps = 1;
                cycle_idx = (cycle_idx + steps) % 8;
                probe_cycle_started_s += (double)steps * round;
            }
        }

        if (state != BBR_STARTUP && state != BBR_PROBE_RTT &&
            min_rtt_s < 1e8 && now - last_probe_rtt > 10.0) {
            enter_state(BBR_PROBE_RTT, now);
            last_probe_rtt = now;
        } else if (state == BBR_PROBE_RTT && now - state_started_s > 0.20) {
            enter_state(BBR_PROBE_BW, now);
        }
    }

    void maybe_fail_open(double now) {
        if (!feedback_ready || last_feedback_s <= 0) return;
        double stale_after = 0.75;
        if (smoothed_rtt_s > 0)
            stale_after = std::max(0.75, std::min(3.0, smoothed_rtt_s * 4.0));
        if (now - last_feedback_s <= stale_after) return;

        feedback_ready = false;
        feedback_samples = 0;
        bootstrap_started_s = 0;
        next_send_s = now;
        pacing_rate = 0;
        bytes_in_flight = 0;
        state = BBR_STARTUP;
        cycle_idx = 0;
        full_bw_rounds = 0;
        full_bw = 0;
        startup_round_bw = 0;
        startup_round_started_s = 0;
        state_started_s = now;
        probe_cycle_started_s = now;
        max_wire_bw = 0;
        bw_bucket_idx = 0;
        bw_bucket_started_s = 0;
        std::memset(bw_buckets, 0, sizeof(bw_buckets));
        congestion_scale = 1.0;
    }

    int64_t bdp() const {
        if (max_wire_bw <= 0 || min_rtt_s >= 1e8) return BBR_INIT_CWND;
        int64_t v = (int64_t)((double)max_wire_bw * min_rtt_s);
        return std::max<int64_t>(v, BBR_MIN_CWND);
    }

    void update_limits() {
        if (!feedback_ready || max_wire_bw <= 0) {
            pacing_rate = 0;
            cwnd = BBR_INIT_CWND;
            return;
        }

        double gain = 1.0;
        if (state == BBR_STARTUP) gain = BBR_STARTUP_GAIN;
        else if (state == BBR_DRAIN) gain = BBR_DRAIN_GAIN;
        else if (state == BBR_PROBE_BW) gain = BBR_PROBE_BW_GAINS[cycle_idx % 8];
        else if (state == BBR_PROBE_RTT) {
            cwnd = BBR_PROBE_RTT_CWND;
            pacing_rate = max_wire_bw / 2;
            if (max_bandwidth > 0 && pacing_rate > max_bandwidth)
                pacing_rate = max_bandwidth;
            return;
        }

        pacing_rate = (int64_t)((double)max_wire_bw * gain * congestion_scale);
        cwnd = (int64_t)((double)bdp() * std::max(gain, 1.0) * congestion_scale);
        if (cwnd < BBR_MIN_CWND) cwnd = BBR_MIN_CWND;
        if (max_bandwidth > 0 && pacing_rate > max_bandwidth)
            pacing_rate = max_bandwidth;
        if (pacing_rate < 1) pacing_rate = 1;
    }
};

extern pacing_t g_fecraw_pacing;

#endif /* FECRAW_PACING_H_ */
