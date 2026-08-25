#ifndef FECRAW_ADAPTIVE_FEC_H_
#define FECRAW_ADAPTIVE_FEC_H_

/*
 * Erasure-aware redundancy planner.
 *
 * RS and sliding-window RLNC intentionally use different rate questions:
 * - recommend() sizes a sealed RS block.
 * - recommend_window_rate() sizes continuous repairs/source for RLNC using
 *   Queqiao's empirically calibrated window-chaining model.
 * - recommend_tail_repairs() sizes the last short burst as a real block so
 *   the final source symbols do not depend on future traffic for protection.
 */

#include "telemetry.h"

#include <algorithm>
#include <cmath>
#include <pthread.h>
#include <time.h>

struct adaptive_fec_t {
    // Compatibility pair consumed by the existing RLNC sender call site. It is
    // initially the configured x:y pair; once WindowRate is known it becomes a
    // fixed-point representation (1,000,000 : rate*1,000,000). RS state lives
    // separately below so the two codecs cannot overwrite one another.
    int data_shards;
    int parity_shards;

    int rs_data_shards;
    int rs_parity_shards;
    int base_parity;
    int max_parity;
    double window_rate;
    double last_adjust_time;
    double last_window_adjust_time;
    pthread_mutex_t mu;

    static double now_sec() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
    }

    void init(int data, int parity) {
        rs_data_shards = std::max(data, 1);
        rs_parity_shards = std::max(parity, 0);
        base_parity = rs_parity_shards;
        max_parity = std::min(rs_data_shards * 7, 254 - rs_data_shards);
        if (max_parity < 0) max_parity = 0;
        if (rs_parity_shards > max_parity) rs_parity_shards = max_parity;

        data_shards = rs_data_shards;
        parity_shards = rs_parity_shards;
        window_rate = (double)parity_shards / (double)data_shards;
        last_adjust_time = 0;
        last_window_adjust_time = 0;
        pthread_mutex_init(&mu, NULL);
    }

    void destroy() { pthread_mutex_destroy(&mu); }

    static double target_residual_for_rtt(double rtt_s) {
        double target = 0.01;
        if (rtt_s > 0.25) target = 0.003;
        else if (rtt_s > 0.10) target = 0.006;
        else if (rtt_s > 0 && rtt_s < 0.04) target = 0.02;
        return target;
    }

    static double binomial_tail_below(int n, double q, int k) {
        if (k <= 0) return 0;
        if (k > n) return 1;
        if (q <= 0) return 1;
        if (q >= 1) return 0;

        double total = 0;
        double log_n1 = lgamma((double)n + 1.0);
        double log_q = log(q);
        double log_p = log1p(-q);
        for (int i = 0; i < k; ++i) {
            double log_i1 = lgamma((double)i + 1.0);
            double log_ni1 = lgamma((double)(n - i) + 1.0);
            total += exp(log_n1 - log_i1 - log_ni1 + i * log_q + (n - i) * log_p);
        }
        return total > 1.0 ? 1.0 : total;
    }

    static double burst_residual(int data, int total, double loss,
                                 double burst_factor) {
        if (data <= 0 || total < data) return 1.0;
        if (!(loss >= 0.0) || !(loss < 1.0)) return 1.0;
        if (burst_factor < 1.0 || !std::isfinite(burst_factor))
            burst_factor = 1.0;

        int trials = (int)std::floor((double)total / burst_factor + 0.5);
        int need = (int)std::ceil((double)data / burst_factor);
        if (trials < 1) trials = 1;
        if (need > trials) return 1.0;
        return binomial_tail_below(trials, 1.0 - loss, need);
    }

    int recommend(const loss_snapshot_t &s, double rtt_s) const {
        if (!s.floor_trusted) return base_parity;
        double loss = s.floor;
        if (loss < 0.005) return 0;
        if (loss >= 0.85) return max_parity;

        double target_residual = target_residual_for_rtt(rtt_s);
        double arrival = 1.0 - loss;
        for (int parity = 0; parity <= max_parity; ++parity) {
            int n = rs_data_shards + parity;
            double residual = binomial_tail_below(n, arrival, rs_data_shards);
            if (residual <= target_residual) return parity;
        }
        return max_parity;
    }

    double recommend_window_rate(int capacity, const loss_snapshot_t &s,
                                 double rtt_s) const {
        if (capacity < 1) return 0;
        if (!s.floor_trusted)
            return (double)base_parity / (double)rs_data_shards;

        double loss = s.floor;
        if (loss < 0.005) return 0;
        double arrival = 1.0 - loss;
        if (arrival <= 0) return 8.0;

        const double window_chaining = 2.5;
        const double max_window_rate = 8.0;
        const double target_residual = target_residual_for_rtt(rtt_s);
        int effective = (int)((double)capacity * window_chaining);
        if (effective < 1) effective = 1;

        int lo = effective;
        int hi = (int)((double)effective / arrival * max_window_rate);
        if (hi < lo) hi = lo;
        if (binomial_tail_below(hi, arrival, effective) > target_residual)
            return max_window_rate;

        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (binomial_tail_below(mid, arrival, effective) <= target_residual)
                hi = mid;
            else
                lo = mid + 1;
        }
        return (double)(lo - effective) / (double)effective;
    }

    // Queqiao's protectBurst asks the block-code question for the actual tail
    // length, not the steady-state WindowRate question. Return the TOTAL number
    // of repairs the short burst should have. -1 means the loss model is not
    // trusted enough to protect a tail yet; callers should leave it pending.
    static int recommend_tail_repairs(int data_symbols,
                                      const loss_snapshot_t &s,
                                      double rtt_s) {
        if (data_symbols < 1 || data_symbols > 256) return -1;
        if (!s.floor_trusted || s.decided < 100) return -1;

        double loss = s.floor;
        if (loss <= 0) loss = s.loss;
        if (loss < 0.005) return 0;
        if (!(loss < 1.0) || !std::isfinite(loss)) return -1;

        double burst = s.burst_factor;
        if (burst < 1.0 || !std::isfinite(burst)) burst = 1.0;
        double target = target_residual_for_rtt(rtt_s);
        for (int total = data_symbols; total <= 256; ++total) {
            if (burst_residual(data_symbols, total, loss, burst) <= target)
                return total - data_symbols;
        }
        return -1;
    }

    bool adjust(const loss_snapshot_t &s, double rtt_s,
                int &out_data, int &out_parity) {
        pthread_mutex_lock(&mu);
        out_data = rs_data_shards;
        out_parity = rs_parity_shards;

        if (!s.floor_trusted || s.decided < 100) {
            pthread_mutex_unlock(&mu);
            return false;
        }

        double now = now_sec();
        if (last_adjust_time > 0 && now - last_adjust_time < 1.0) {
            pthread_mutex_unlock(&mu);
            return false;
        }

        int target = recommend(s, rtt_s);
        int old = rs_parity_shards;
        if (target > rs_parity_shards) {
            int step = std::max(1, (target - rs_parity_shards + 1) / 2);
            rs_parity_shards = std::min(target, rs_parity_shards + step);
        } else if (target < rs_parity_shards) {
            rs_parity_shards -= 1;
        }

        if (rs_parity_shards < 0) rs_parity_shards = 0;
        if (rs_parity_shards > max_parity) rs_parity_shards = max_parity;
        last_adjust_time = now;
        out_parity = rs_parity_shards;
        pthread_mutex_unlock(&mu);
        return old != rs_parity_shards;
    }

    bool adjust_window_rate(int capacity, const loss_snapshot_t &s,
                            double rtt_s, double &out_rate) {
        pthread_mutex_lock(&mu);
        out_rate = window_rate;
        if (!s.floor_trusted || s.decided < 100) {
            pthread_mutex_unlock(&mu);
            return false;
        }

        double now = now_sec();
        if (last_window_adjust_time > 0 && now - last_window_adjust_time < 1.0) {
            pthread_mutex_unlock(&mu);
            return false;
        }

        double target = recommend_window_rate(capacity, s, rtt_s);
        bool changed = std::fabs(target - window_rate) > 1e-9;
        window_rate = target;
        last_window_adjust_time = now;

        const int scale = 1000000;
        data_shards = scale;
        parity_shards = (int)(window_rate * scale + 0.5);
        if (parity_shards < 0) parity_shards = 0;
        if (parity_shards > 8 * scale) parity_shards = 8 * scale;
        out_rate = window_rate;
        pthread_mutex_unlock(&mu);
        return changed;
    }

    double get_window_rate() {
        pthread_mutex_lock(&mu);
        double out = window_rate;
        pthread_mutex_unlock(&mu);
        return out;
    }
};

#endif /* FECRAW_ADAPTIVE_FEC_H_ */
