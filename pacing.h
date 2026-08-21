#ifndef FECRAW_PACING_H_
#define FECRAW_PACING_H_

/*
 * Erasure-aware BBR-lite pacer.
 *
 * The controller consumes protocol-v2 telemetry feedback for packets this
 * endpoint actually sent. The delivered-rate estimate is compensated only for
 * a trusted rate-independent erasure floor; excess loss and burst growth are
 * treated as congestion instead of being coded around.
 *
 * Liveness rule: pacing is fail-open. Before we have a real RTT + delivery-rate
 * sample, packets bypass the delay queue completely. If feedback later goes
 * stale, the pacing epoch is reset and traffic bypasses the queue again until a
 * fresh sample arrives. This prevents the libev timer queue from becoming part
 * of the feedback bootstrap dependency.
 *
 * Once feedback is live, reserve_delay_us() is deliberately non-blocking. It
 * assigns a wire send time and returns immediately; packet.cpp places the
 * already-framed packet into UDPspeeder's existing delay_manager. ACK handling
 * therefore remains runnable while the sender is rate-limited.
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

struct pacing_t {
    pthread_mutex_t mu;

    int64_t max_wire_bw;       // bytes/s, compensated for trusted erasure
    double min_rtt_s;
    double smoothed_rtt_s;
    volatile int64_t bytes_in_flight;
    int64_t cwnd;
    int64_t pacing_rate;
    int64_t max_bandwidth;     // bytes/s hard cap after feedback bootstrap
    double next_send_s;        // reserved wire time for the next data packet

    bbr_state_t state;
    int cycle_idx;
    int plateau_samples;
    int64_t last_growth_bw;
    double last_probe_rtt;
    double congestion_scale;
    bool enabled;

    // Cold-start / liveness state. No packet enters delay_manager until a real
    // feedback sample has established both RTT and delivered rate.
    bool feedback_ready;
    int feedback_samples;
    double last_feedback_s;

    int64_t bw_samples[10];
    int bw_sample_idx;

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
        // Deliberately zero during bootstrap. max_bandwidth is enforced once
        // telemetry has established a pacing epoch; bootstrap itself is direct.
        pacing_rate = 0;
        max_bandwidth = max_bw_limit;
        next_send_s = now_s();
        state = BBR_STARTUP;
        cycle_idx = 0;
        plateau_samples = 0;
        last_growth_bw = 0;
        last_probe_rtt = next_send_s;
        congestion_scale = 1.0;
        enabled = true;
        feedback_ready = false;
        feedback_samples = 0;
        last_feedback_s = 0;
        std::memset(bw_samples, 0, sizeof(bw_samples));
        bw_sample_idx = 0;
    }

    void destroy() { pthread_mutex_destroy(&mu); }

    bool has_feedback() const { return feedback_ready; }

    // Reserve this many bytes in the aggregate sender and return how long the
    // caller should defer the packet. This never sleeps or waits for cwnd.
    uint64_t reserve_delay_us(int size) {
        if (!enabled || size <= 0) return 0;
        pthread_mutex_lock(&mu);

        double now = now_s();
        maybe_fail_open(now);
        bytes_in_flight += size;

        // Critical cold-start behavior: do not enqueue before real feedback.
        if (!feedback_ready || pacing_rate <= 0) {
            pthread_mutex_unlock(&mu);
            return 0;
        }

        // If userspace/timer scheduling fell behind far enough to build a long
        // queue, discard the stale schedule rather than amplifying the stall.
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
                     const loss_snapshot_t &loss) {
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

        if (delivered_rate > 0 && acked_bytes > 0) {
            double arrival = 1.0;
            if (loss.floor_trusted) {
                arrival = 1.0 - loss.floor;
                if (arrival < 0.15) arrival = 0.15;
            }
            int64_t wire_sample = (int64_t)(delivered_rate / arrival);
            if (wire_sample < 1) wire_sample = 1;
            bw_samples[bw_sample_idx++ % 10] = wire_sample;
            max_wire_bw = 0;
            for (int i = 0; i < 10; ++i)
                if (bw_samples[i] > max_wire_bw) max_wire_bw = bw_samples[i];

            ++feedback_samples;
            if (!feedback_ready && smoothed_rtt_s > 0 && max_wire_bw > 0) {
                feedback_ready = true;
                // Start the pacing timeline at this instant. Never inherit a
                // timestamp accumulated during the feedback-free period.
                next_send_s = now;
                bytes_in_flight = 0;
            }

            if (state == BBR_STARTUP) {
                if (last_growth_bw == 0 || wire_sample > last_growth_bw * 5 / 4) {
                    last_growth_bw = wire_sample;
                    plateau_samples = 0;
                } else if (++plateau_samples >= 3) {
                    state = BBR_DRAIN;
                    plateau_samples = 0;
                }
            } else if (state == BBR_DRAIN && bytes_in_flight <= bdp()) {
                state = BBR_PROBE_BW;
                cycle_idx = 0;
            } else if (state == BBR_PROBE_BW) {
                cycle_idx = (cycle_idx + 1) % 8;
            }
        }

        // A trusted floor is not congestion. Only excess loss / burst growth
        // trims the aggregate wire rate. Recover slowly after the queue clears.
        if (loss.congestive > 0.02 || loss.burst_factor > 1.6) {
            congestion_scale *= 0.85;
            if (congestion_scale < 0.50) congestion_scale = 0.50;
        } else {
            congestion_scale *= 1.02;
            if (congestion_scale > 1.0) congestion_scale = 1.0;
        }

        if (state != BBR_STARTUP && min_rtt_s < 1e8 && now - last_probe_rtt > 10.0) {
            state = BBR_PROBE_RTT;
            last_probe_rtt = now;
        } else if (state == BBR_PROBE_RTT && now - last_probe_rtt > 0.20) {
            state = BBR_PROBE_BW;
            cycle_idx = 0;
        }

        update_limits();
        pthread_mutex_unlock(&mu);
    }

    const char *state_name() const {
        static const char *names[] = {"startup", "drain", "probe_bw", "probe_rtt"};
        return names[state];
    }

private:
    void maybe_fail_open(double now) {
        if (!feedback_ready || last_feedback_s <= 0) return;
        double stale_after = 0.75;
        if (smoothed_rtt_s > 0)
            stale_after = std::max(0.75, std::min(3.0, smoothed_rtt_s * 4.0));
        if (now - last_feedback_s <= stale_after) return;

        feedback_ready = false;
        feedback_samples = 0;
        next_send_s = now;
        pacing_rate = 0;
        bytes_in_flight = 0;
        state = BBR_STARTUP;
        cycle_idx = 0;
        plateau_samples = 0;
        last_growth_bw = 0;
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
