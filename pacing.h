#ifndef FECRAW_PACING_H_
#define FECRAW_PACING_H_

/*
 * BBR-lite congestion controller / pacer -- ported from NRUP congestion.go
 *
 * Four-state machine: startup -> drain -> probe_bw -> probe_rtt
 * Estimates maxBW and minRTT, then derives:
 *   BDP       = maxBW * minRTT
 *   cwnd      = BDP * gain
 *   pacingRate = maxBW * gain
 *
 * Call wait(size) before sending -- it blocks until both cwnd and
 * pacing constraints are satisfied.
 */

#include <cstdint>
#include <cstring>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

enum bbr_state_t {
    BBR_STARTUP = 0,
    BBR_DRAIN,
    BBR_PROBE_BW,
    BBR_PROBE_RTT
};

static const double  BBR_STARTUP_GAIN   = 2.89;   // 2/ln(2)
static const double  BBR_DRAIN_GAIN     = 0.35;   // 1/startup
static const int64_t BBR_PROBE_RTT_CWND = 4 * 1500;
static const int64_t BBR_INIT_CWND      = 32768;  // 32KB
static const int64_t BBR_MIN_CWND       = 4096;   // 4KB
static const double  BBR_MIN_RTT_WINDOW_S = 10.0;
static const double  BBR_PROBE_RTT_HOLD_S = 0.2;
static const double  BBR_PROBE_BW_GAINS[] = {1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

struct pacing_t {
    pthread_mutex_t mu;

    /* estimates */
    int64_t max_bw;        // bytes/s
    double  min_rtt_s;     // seconds
    double  smoothed_rtt_s;

    /* in-flight tracking */
    volatile int64_t bytes_in_flight;

    /* delivery tracking */
    int64_t  delivered;
    double   delivered_time_s;

    /* window */
    int64_t cwnd;
    int64_t pacing_rate;   // bytes/s

    /* state machine */
    bbr_state_t state;
    int    cycle_idx;
    double probe_rtt_start_s;
    double min_rtt_expiry_s;
    bool   probe_rtt_done;
    int64_t prior_cwnd;

    /* BW samples (sliding window of 10) */
    int64_t bw_samples[10];
    int     bw_sample_idx;

    /* user limit */
    int64_t max_bandwidth;  // 0 = unlimited

    bool enabled;

    static double now_s() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
    }

    void init(int64_t max_bw_limit) {
        pthread_mutex_init(&mu, NULL);
        max_bw = 0;
        min_rtt_s = 1e9;
        smoothed_rtt_s = 0;
        bytes_in_flight = 0;
        delivered = 0;
        delivered_time_s = now_s();
        cwnd = BBR_INIT_CWND;
        pacing_rate = 0;
        state = BBR_STARTUP;
        cycle_idx = 0;
        probe_rtt_start_s = 0;
        min_rtt_expiry_s = now_s() + BBR_MIN_RTT_WINDOW_S;
        probe_rtt_done = false;
        prior_cwnd = 0;
        memset(bw_samples, 0, sizeof(bw_samples));
        bw_sample_idx = 0;
        max_bandwidth = max_bw_limit;
        enabled = true;
    }

    void destroy() {
        pthread_mutex_destroy(&mu);
    }

    /* Block until cwnd + pacing allow sending `size` bytes.
     * Safety cap: yield at most 500ms to avoid indefinite stall. */
    void wait(int size) {
        if (!enabled) return;

        int wait_loops = 0;
        while (bytes_in_flight > cwnd && wait_loops < 500) {
            usleep(1000);
            wait_loops++;
        }
        __sync_fetch_and_add(&bytes_in_flight, (int64_t)size);

        if (pacing_rate > 0) {
            double delay_us = (double)size / (double)pacing_rate * 1e6;
            if (delay_us > 500.0) {
                usleep((unsigned int)delay_us);
            }
        }
    }

    /* Call when data is acknowledged / received from remote */
    void on_ack(int64_t bytes, double rtt_s) {
        pthread_mutex_lock(&mu);

        __sync_fetch_and_add(&bytes_in_flight, -bytes);
        if (bytes_in_flight < 0) bytes_in_flight = 0;

        if (rtt_s > 0) {
            if (smoothed_rtt_s <= 0)
                smoothed_rtt_s = rtt_s;
            else
                smoothed_rtt_s = smoothed_rtt_s * 0.875 + rtt_s * 0.125;

            double t = now_s();
            if (rtt_s < min_rtt_s || t > min_rtt_expiry_s) {
                min_rtt_s = rtt_s;
                min_rtt_expiry_s = t + BBR_MIN_RTT_WINDOW_S;
            }

            double elapsed = t - delivered_time_s;
            if (elapsed > 0.001) {
                int64_t bw = (int64_t)((double)bytes / elapsed);
                bw_samples[bw_sample_idx % 10] = bw;
                bw_sample_idx++;

                max_bw = 0;
                for (int i = 0; i < 10; i++) {
                    if (bw_samples[i] > max_bw) max_bw = bw_samples[i];
                }
            }
            delivered += bytes;
            delivered_time_s = t;
        }

        update_state();
        update_cwnd();
        pthread_mutex_unlock(&mu);
    }

    void on_loss() {
        pthread_mutex_lock(&mu);
        cwnd = cwnd * 85 / 100;
        if (cwnd < BBR_MIN_CWND) cwnd = BBR_MIN_CWND;
        pthread_mutex_unlock(&mu);
    }

    const char *state_name() const {
        static const char *names[] = {"startup", "drain", "probe_bw", "probe_rtt"};
        return names[state];
    }

private:
    int64_t bdp() const {
        if (max_bw == 0 || min_rtt_s >= 1e8)
            return BBR_INIT_CWND;
        return (int64_t)((double)max_bw * min_rtt_s);
    }

    void update_state() {
        double t = now_s();
        switch (state) {
        case BBR_STARTUP:
            if (bw_sample_idx >= 3) {
                int64_t recent = bw_samples[(bw_sample_idx - 1) % 10];
                int64_t prev   = bw_samples[(bw_sample_idx - 2) % 10];
                if (prev > 0 && (double)recent / (double)prev < 1.25) {
                    state = BBR_DRAIN;
                    prior_cwnd = cwnd;
                }
            }
            break;

        case BBR_DRAIN:
            if (bytes_in_flight <= bdp()) {
                state = BBR_PROBE_BW;
                cycle_idx = 0;
            }
            break;

        case BBR_PROBE_BW:
            cycle_idx++;
            if (t > min_rtt_expiry_s) {
                state = BBR_PROBE_RTT;
                probe_rtt_start_s = t;
                prior_cwnd = cwnd;
                probe_rtt_done = false;
            }
            break;

        case BBR_PROBE_RTT:
            if (!probe_rtt_done) {
                if (bytes_in_flight <= BBR_PROBE_RTT_CWND) {
                    probe_rtt_done = true;
                    probe_rtt_start_s = t;
                }
            }
            if (probe_rtt_done && (t - probe_rtt_start_s) > BBR_PROBE_RTT_HOLD_S) {
                min_rtt_expiry_s = t + BBR_MIN_RTT_WINDOW_S;
                state = BBR_PROBE_BW;
                cycle_idx = 0;
                cwnd = prior_cwnd;
            }
            break;
        }
    }

    void update_cwnd() {
        int64_t b = bdp();
        double gain;

        switch (state) {
        case BBR_STARTUP:
            gain = BBR_STARTUP_GAIN;
            break;
        case BBR_DRAIN:
            gain = BBR_DRAIN_GAIN;
            break;
        case BBR_PROBE_BW:
            gain = BBR_PROBE_BW_GAINS[cycle_idx % 8];
            break;
        case BBR_PROBE_RTT:
            cwnd = BBR_PROBE_RTT_CWND;
            pacing_rate = (int64_t)((double)max_bw * 0.5);
            return;
        default:
            gain = 1.0;
            break;
        }

        cwnd = (int64_t)((double)b * gain);
        if (cwnd < BBR_MIN_CWND) cwnd = BBR_MIN_CWND;

        pacing_rate = (int64_t)((double)max_bw * gain);

        if (max_bandwidth > 0 && pacing_rate > max_bandwidth)
            pacing_rate = max_bandwidth;
    }
};

#endif /* FECRAW_PACING_H_ */
