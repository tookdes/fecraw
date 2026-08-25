#include "telemetry.h"
#include "adaptive_fec.h"
#include "pacing.h"

#include <cassert>
#include <cstdio>
#include <cstring>

static bool drop_packet(unsigned seq) {
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
    synthetic.burst_factor = 1.0;
    synthetic.decided = 1000;
    int recommended = planner.recommend(synthetic, 0.20);
    assert(recommended > 4);
    assert(recommended <= planner.max_parity);

    // Stage-4 pacing bootstrap regression. The sender must remain fail-open for
    // one RTT worth of genuine samples, retain a short high sample in a
    // time-based max filter, and STARTUP must apply >=2x gain rather than
    // immediately defining capacity as its own paced output.
    pacing_t pacer;
    pacer.init(0);
    uint64_t cold_first = pacer.reserve_delay_us(1000);
    uint64_t cold_second = pacer.reserve_delay_us(1000);
    assert(cold_first == 0);
    assert(cold_second == 0);
    assert(!pacer.has_feedback());
    pacer.cancel_reserved(2000);

    loss_snapshot_t ploss;
    ploss.floor_trusted = true;
    ploss.floor = 0.0;
    ploss.loss = 0.0;
    ploss.memoryless = true;
    ploss.burst_factor = 1.0;
    ploss.congestive = 0.0;
    ploss.decided = 1000;

    pacer.on_feedback(10000, 10000, 0.200, 100000.0, false, ploss);
    assert(!pacer.has_feedback());
    pacer.on_feedback(10000, 10000, 0.200, 100000.0, true, ploss);
    assert(!pacer.has_feedback());

    // Avoid a wall-clock sleep in the unit test: keep production semantics but
    // make the already-started bootstrap epoch one RTT old.
    pacer.bootstrap_started_s = pacing_t::now_s() - 0.25;
    pacer.on_feedback(10000, 10000, 0.200, 120000.0, true, ploss);
    pacer.on_feedback(10000, 10000, 0.200, 2500000.0, true, ploss);
    pacer.on_feedback(10000, 10000, 0.200, 150000.0, true, ploss);
    assert(pacer.has_feedback());
    assert(pacer.state == BBR_STARTUP);
    assert(pacer.max_wire_bw >= 2500000);
    assert(pacer.pacing_rate >= 5000000);

    int64_t peak = pacer.max_wire_bw;
    for (int i = 0; i < 16; ++i)
        pacer.on_feedback(10000, 10000, 0.200, 80000.0 + i, true, ploss);
    assert(pacer.max_wire_bw == peak);

    // Correlated erasure is not congestion. Stage 3 used burst_factor>1.6 to
    // halve congestion_scale even with no excess loss, producing the observed
    // probe_bw rate/wire_bw ~= 0.425. Keep scale at one in this case.
    loss_snapshot_t burst_only = ploss;
    burst_only.burst_factor = 3.0;
    burst_only.congestive = 0.0;
    pacer.congestion_scale = 1.0;
    pacer.on_feedback(10000, 10000, 0.200, 100000.0, true, burst_only);
    assert(pacer.congestion_scale > 0.99);

    uint64_t paced_first = pacer.reserve_delay_us(1000);
    uint64_t paced_second = pacer.reserve_delay_us(1000);
    assert(paced_first < 10000);
    assert(paced_second > 10);
    assert(paced_second < 10000);
    pacer.cancel_reserved(2000);

    std::printf("telemetry: decided=%llu loss=%.3f burst=%.2f floor=%.3f trusted=%d; "
                "RS 20:%d; cold=%lluus/%lluus startup_bw=%.3fMbps rate=%.3fMbps paced=%lluus/%lluus\n",
                (unsigned long long)s.decided, s.loss, s.burst_factor, s.floor,
                s.floor_trusted ? 1 : 0, recommended,
                (unsigned long long)cold_first, (unsigned long long)cold_second,
                (double)pacer.max_wire_bw * 8.0 / 1000000.0,
                (double)pacer.pacing_rate * 8.0 / 1000000.0,
                (unsigned long long)paced_first, (unsigned long long)paced_second);
    return 0;
}
