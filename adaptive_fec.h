#ifndef FECRAW_ADAPTIVE_FEC_H_
#define FECRAW_ADAPTIVE_FEC_H_

/*
 * Adaptive FEC ratio controller -- ported from NRUP fec_adaptive.go
 *
 * Maintains a sliding window of recent send/loss events and periodically
 * recalculates the optimal data:parity ratio.  Includes smoothing to prevent
 * oscillation: changes are limited to ±1 per evaluation, with a minimum
 * hold time between adjustments and hysteresis for decreases.
 */

#include <cstring>
#include <cstdio>
#include <time.h>
#include <pthread.h>

struct adaptive_fec_t {
    int data_shards;
    int parity_shards;
    int min_parity;
    int max_parity;

    int window_sent;
    int window_lost;

    double rtt_ms;

    int    consecutive_low;  // consecutive low-loss evaluations for decrease
    double last_adjust_time;

    pthread_mutex_t mu;

    static double now_sec() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
    }

    void init(int data, int parity) {
        data_shards  = data;
        parity_shards = parity;
        min_parity   = 2;
        max_parity   = data;
        window_sent  = 0;
        window_lost  = 0;
        rtt_ms = 0;
        consecutive_low = 0;
        last_adjust_time = 0;
        pthread_mutex_init(&mu, NULL);
    }

    void destroy() {
        pthread_mutex_destroy(&mu);
    }

    void record_sent(int n) {
        pthread_mutex_lock(&mu);
        window_sent += n;
        pthread_mutex_unlock(&mu);
    }

    void record_loss(int n) {
        pthread_mutex_lock(&mu);
        window_lost += n;
        pthread_mutex_unlock(&mu);
    }

    void set_rtt(double ms) {
        rtt_ms = ms;
    }

    /* Returns true if parity changed. Caller should re-apply fec params. */
    bool adjust(int &out_data, int &out_parity) {
        pthread_mutex_lock(&mu);

        out_data   = data_shards;
        out_parity = parity_shards;

        if (window_sent < 50) {
            pthread_mutex_unlock(&mu);
            return false;
        }

        double now = now_sec();
        if (last_adjust_time > 0 && (now - last_adjust_time) < 3.0) {
            pthread_mutex_unlock(&mu);
            return false;
        }

        double loss_rate = (double)window_lost / (double)window_sent;

        double rtt_factor = 1.0;
        if (rtt_ms > 100.0) rtt_factor = 1.3;
        if (rtt_ms > 300.0) rtt_factor = 1.6;

        int target;
        if (loss_rate < 0.02)
            target = min_parity;
        else if (loss_rate < 0.05)
            target = (int)(3.0 * rtt_factor);
        else if (loss_rate < 0.10)
            target = (int)(5.0 * rtt_factor);
        else if (loss_rate < 0.20)
            target = (int)(8.0 * rtt_factor);
        else
            target = max_parity;

        if (target < min_parity) target = min_parity;
        if (target > max_parity) target = max_parity;

        int old_parity = parity_shards;

        if (target > parity_shards) {
            parity_shards += 1;
            consecutive_low = 0;
        } else if (target < parity_shards) {
            consecutive_low++;
            if (consecutive_low >= 3) {
                parity_shards -= 1;
                consecutive_low = 0;
            }
        } else {
            if (loss_rate < 0.02)
                consecutive_low++;
            else
                consecutive_low = 0;
        }

        if (parity_shards < min_parity) parity_shards = min_parity;
        if (parity_shards > max_parity) parity_shards = max_parity;

        window_sent = 0;
        window_lost = 0;
        last_adjust_time = now;

        out_data   = data_shards;
        out_parity = parity_shards;
        pthread_mutex_unlock(&mu);

        return parity_shards != old_parity;
    }
};

#endif /* FECRAW_ADAPTIVE_FEC_H_ */
