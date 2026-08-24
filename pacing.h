#ifndef FECRAW_PACING_H_
#define FECRAW_PACING_H_

/*
 * Erasure-aware BBR-lite pacer.
 *
 * Liveness rule: pacing is fail-open. Packets bypass the delay queue until
 * telemetry has produced two real delivery-rate samples. A cached/stale rate
 * carried on an ACK is not a sample and cannot advance STARTUP. If feedback
 * later goes stale, the bandwidth epoch is discarded and traffic returns to
 * direct send until the model is rebuilt.
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
    int plateau_samples;
    int64_t last_growth_bw;
    double last_probe_rtt;
    double congestion_scale;
    bool enabled;

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

        // Only a newly measured ACK/send slope is a BBR bandwidth sample.
        // Reusing the cached delivery_rate on every ACK used to turn one low
        // observation into three apparent no-growth rounds and leave STARTUP.
        if (delivery_sampled && delivered_rate > 0 && acked_bytes > 0) {
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
            // Two independent slope samples cost only one extra ACK interval
            // on a bulk flow and prevent one anomalous first point from closing
            // the fail-open bootstrap around a false low rate.
            if (!feedback_ready && feedback_samples >= 2 &&
                smoothed_rtt_s > 0 && max_wire_bw > 0) {
                feedback_ready = true;
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

            // Loss state changes much more slowly than ACK callbacks. Apply it
            // at the same cadence as real delivery samples so one snapshot is
            // not multiplied into many congestion reductions.
            if (loss.congestive > 0.02 || loss.burst_factor > 1.6) {
                congestion_scale *= 0.85;
                if (congestion_scale < 0.50) congestion_scale = 0.50;
            } else {
                congestion_scale *= 1.02;
                if (congestion_scale > 1.0) congestion_scale = 1.0;
            }
        }

        if (feedback_ready && state != BBR_STARTUP && min_rtt_s < 1e8 &&
            now - last_probe_rtt > 10.0) {
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
        max_wire_bw = 0;
        bw_sample_idx = 0;
        std::memset(bw_samples, 0, sizeof(bw_samples));
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
