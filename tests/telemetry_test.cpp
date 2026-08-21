#include "telemetry.h"
#include "adaptive_fec.h"
#include "pacing.h"

#include <cassert>
#include <cstdio>
#include <cstring>

static bool drop_packet(unsigned seq) {
    // Deterministic pseudo-random ~20% erasure, not a periodic burst pattern.
    unsigned x = seq * 2654435761u;
    x ^= x >> 13;
    x *= 2246822519u;
    return (x % 100) < 20;
}

static void deliver_ack(telemetry_link_t &rx, telemetry_link_t &tx) {
    char ack[64];
    int ack_len = rx.build_ack(ack, sizeof(ack), true);
    if (ack_len <= 0) return;
    bool control = false;
    telemetry_feedback_t feedback;
    int len = ack_len;
    assert(tx.consume(ack, len, control, feedback) == 0);
    assert(control);
    assert(len == 0);
}

int main() {
    telemetry_link_t tx;
    telemetry_link_t rx;
    tx.init(true);
    rx.init(true);

    const char payload[] = "fecraw-v2";
    for (unsigned i = 1; i <= 400; ++i) {
        char frame[256];
        uint64_t seq = 0;
        int frame_len = tx.prepare_data(payload, sizeof(payload), frame, sizeof(frame), seq);
        assert(frame_len > 0);
        assert(seq == i);
        tx.commit_sent(seq, frame_len);

        if (!drop_packet(i)) {
            int len = frame_len;
            bool control = false;
            telemetry_feedback_t feedback;
            assert(rx.consume(frame, len, control, feedback) == 0);
            assert(!control);
            assert(len == (int)sizeof(payload));
            assert(std::memcmp(frame, payload, sizeof(payload)) == 0);
            deliver_ack(rx, tx);
        }
    }

    // Flush the sender's reorder tolerance so earlier gaps become decided.
    for (unsigned i = 401; i <= 464; ++i) {
        char frame[256];
        uint64_t seq = 0;
        int frame_len = tx.prepare_data(payload, sizeof(payload), frame, sizeof(frame), seq);
        assert(frame_len > 0);
        tx.commit_sent(seq, frame_len);

        int len = frame_len;
        bool control = false;
        telemetry_feedback_t feedback;
        assert(rx.consume(frame, len, control, feedback) == 0);
        deliver_ack(rx, tx);
    }

    loss_snapshot_t s = tx.snapshot();
    assert(s.decided > 400);
    assert(s.loss > 0.10 && s.loss < 0.30);
    assert(s.burst_factor >= 1.0);

    adaptive_fec_t planner;
    planner.init(20, 4);
    loss_snapshot_t synthetic;
    synthetic.floor_trusted = true;
    synthetic.floor = 0.20;
    synthetic.loss = 0.20;
    synthetic.memoryless = true;
    synthetic.decided = 1000;
    int recommended = planner.recommend(synthetic, 0.20);
    assert(recommended > 4);
    assert(recommended <= planner.max_parity);

    // Regression for the real-link zero-throughput bug: the pacer must be
    // completely fail-open until real feedback exists. In particular, a hard
    // max-bandwidth setting must not seed a libev timer queue before ACK/RTT
    // feedback can itself get through.
    pacing_t pacer;
    pacer.init(1000000);
    uint64_t cold_first = pacer.reserve_delay_us(1000);
    uint64_t cold_second = pacer.reserve_delay_us(1000);
    assert(cold_first == 0);
    assert(cold_second == 0);
    assert(!pacer.has_feedback());
    pacer.cancel_reserved(2000);

    loss_snapshot_t ploss;
    ploss.floor_trusted = true;
    ploss.floor = 0.10;
    ploss.loss = 0.10;
    ploss.memoryless = true;
    ploss.decided = 1000;
    pacer.on_feedback(10000, 10000, 0.150, 1000000.0, ploss);
    assert(pacer.has_feedback());

    // Once feedback bootstraps the epoch, pacing becomes active and remains
    // non-blocking: at the 1 MB/s cap the second 1000-byte reservation should
    // be roughly 1ms behind the first.
    uint64_t paced_first = pacer.reserve_delay_us(1000);
    uint64_t paced_second = pacer.reserve_delay_us(1000);
    assert(paced_first < 10000);
    assert(paced_second > 100);
    assert(paced_second < 10000);
    pacer.cancel_reserved(2000);

    std::printf("telemetry: decided=%llu loss=%.3f burst=%.2f floor=%.3f trusted=%d; "
                "RS 20:%d; cold=%lluus/%lluus paced=%lluus/%lluus\n",
                (unsigned long long)s.decided, s.loss, s.burst_factor, s.floor,
                s.floor_trusted ? 1 : 0, recommended,
                (unsigned long long)cold_first, (unsigned long long)cold_second,
                (unsigned long long)paced_first, (unsigned long long)paced_second);
    return 0;
}
