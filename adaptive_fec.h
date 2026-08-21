#ifndef FECRAW_ADAPTIVE_FEC_H_
#define FECRAW_ADAPTIVE_FEC_H_

/*
 * Erasure-aware RS planner.
 *
 * The previous controller derived "loss" from FEC decoder input/output counts,
 * which mixed parity with actual channel loss and also adjusted the opposite
 * direction. This planner consumes protocol-v2 sender feedback instead. It
 * sizes parity only from a trusted rate-independent erasure floor and leaves
 * excess/bursty loss to congestion control.
 */

#include "telemetry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <pthread.h>
#include <time.h>

struct adaptive_fec_t {
    int data_shards;
    int parity_shards;
    int base_parity;
    int max_parity;
    double last_adjust_time;
    pthread_mutex_t mu;

    static double now_sec() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
    }

    void init(int data, int parity) {
        data_shards = std::max(data, 1);
        parity_shards = std::max(parity, 0);
        base_parity = parity_shards;
        // Match Queqiao's minimum code-rate guard (1/8) while staying below
        // the GF(256) shard ceiling used by the inherited RS implementation.
        max_parity = std::min(data_shards * 7, 254 - data_shards);
        if (max_parity < 0) max_parity = 0;
        if (parity_shards > max_parity) parity_shards = max_parity;
        last_adjust_time = 0;
        pthread_mutex_init(&mu, NULL);
    }

    void destroy() { pthread_mutex_destroy(&mu); }

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

    int recommend(const loss_snapshot_t &s, double rtt_s) const {
        if (!s.floor_trusted) return base_parity;
        double loss = s.floor;
        if (loss < 0.005) return 0;
        if (loss >= 0.85) return max_parity;

        // Long RTT makes a residual loss more expensive because the inner TCP
        // recovery costs a WAN RTT. Keep a non-zero target: parity beyond this
        // point grows geometrically and residual loss is what TCP is good at.
        double target_residual = 0.01;
        if (rtt_s > 0.25) target_residual = 0.003;
        else if (rtt_s > 0.10) target_residual = 0.006;
        else if (rtt_s > 0 && rtt_s < 0.04) target_residual = 0.02;

        double arrival = 1.0 - loss;
        for (int parity = 0; parity <= max_parity; ++parity) {
            int n = data_shards + parity;
            double residual = binomial_tail_below(n, arrival, data_shards);
            if (residual <= target_residual) return parity;
        }
        return max_parity;
    }

    bool adjust(const loss_snapshot_t &s, double rtt_s, int &out_data, int &out_parity) {
        pthread_mutex_lock(&mu);
        out_data = data_shards;
        out_parity = parity_shards;

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
        int old = parity_shards;

        // Rise quickly enough to become useful on a 40% erasure path, but do
        // not jump from a clean-path code to the final rate in one feedback.
        if (target > parity_shards) {
            int step = std::max(1, (target - parity_shards + 1) / 2);
            parity_shards = std::min(target, parity_shards + step);
        } else if (target < parity_shards) {
            parity_shards -= 1;
        }

        if (parity_shards < 0) parity_shards = 0;
        if (parity_shards > max_parity) parity_shards = max_parity;
        last_adjust_time = now;
        out_parity = parity_shards;
        pthread_mutex_unlock(&mu);
        return old != parity_shards;
    }
};

#endif /* FECRAW_ADAPTIVE_FEC_H_ */
