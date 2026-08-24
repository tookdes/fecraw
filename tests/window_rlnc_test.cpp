#include "window_rlnc.h"
#include "adaptive_fec.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static void deliver(window_rlnc_receiver_t &rx,
                    const std::vector<char> &frame,
                    std::vector<std::vector<char> > &out) {
    std::vector<std::vector<char> > packets;
    assert(rx.receive(frame.data(), (int)frame.size(), packets) == 0);
    out.insert(out.end(), packets.begin(), packets.end());
}

static void test_single_symbol_recovery() {
    window_rlnc_sender_t tx;
    window_rlnc_receiver_t rx;
    tx.init(64, 1250, 1, 1);
    rx.init();

    std::vector<std::vector<char> > received;
    for (int id = 0; id < 8; ++id) {
        char payload[64];
        int n = std::snprintf(payload, sizeof(payload), "packet-%d-rlnc", id);
        assert(n > 0);

        std::vector<std::vector<char> > frames;
        assert(tx.encode_packet(payload, n + 1, frames) == 0);
        bool dropped_source = false;
        for (size_t i = 0; i < frames.size(); ++i) {
            if (id == 3 && !dropped_source &&
                window_rlnc_sender_t::is_source_frame(frames[i].data(), (int)frames[i].size())) {
                dropped_source = true;
                continue;
            }
            deliver(rx, frames[i], received);
        }
        if (id == 3) assert(dropped_source);
    }

    assert(received.size() == 8);
    bool saw_recovered = false;
    for (size_t i = 0; i < received.size(); ++i) {
        if (std::strcmp(received[i].data(), "packet-3-rlnc") == 0)
            saw_recovered = true;
    }
    assert(saw_recovered);
    assert(rx.recovered_symbols() >= 1);
}

static void test_fragment_reassembly_with_loss() {
    window_rlnc_sender_t tx;
    window_rlnc_receiver_t rx;
    tx.init(64, 700, 1, 1);
    rx.init();

    std::vector<char> original(2000);
    for (size_t i = 0; i < original.size(); ++i)
        original[i] = (char)((i * 37 + 11) & 0xff);

    std::vector<std::vector<char> > frames;
    assert(tx.encode_packet(original.data(), (int)original.size(), frames) == 0);

    int source_index = 0;
    bool dropped = false;
    std::vector<std::vector<char> > received;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (window_rlnc_sender_t::is_source_frame(frames[i].data(), (int)frames[i].size())) {
            ++source_index;
            if (source_index == 2) {
                dropped = true;
                continue;
            }
        }
        deliver(rx, frames[i], received);
    }

    assert(dropped);
    assert(source_index >= 3);
    assert(received.size() == 1);
    assert(received[0].size() == original.size());
    assert(std::memcmp(received[0].data(), original.data(), original.size()) == 0);
    assert(rx.recovered_symbols() >= 1);
}

static bool drop_wire(unsigned seq) {
    unsigned x = seq * 2654435761u + 0x9e3779b9u;
    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (x % 2000u) < 350u;
}

static void test_window_rate_on_real_link_floor() {
    adaptive_fec_t planner = {};
    planner.data_shards = planner.rs_data_shards = 20;
    planner.parity_shards = planner.rs_parity_shards = 10;
    planner.base_parity = 10;
    planner.max_parity = 140;

    loss_snapshot_t s;
    s.floor_trusted = true;
    s.floor = 0.175;
    s.loss = 0.175;
    s.memoryless = true;
    s.burst_factor = 1.0;
    s.decided = 10000;

    int rs = planner.recommend(s, 0.20);
    double rate = planner.recommend_window_rate(64, s, 0.20);
    // The live controller can remain at 20:13 because parity decreases one at
    // a time after higher earlier estimates. This pure snapshot calculation is
    // intentionally stateless; only require that block RS needs substantially
    // more repair than the chained-window rate at the same measured floor.
    assert(rs >= 10 && rs <= 13);
    assert(rate > 0.25 && rate < 0.40);
    assert(rate < (double)rs / 20.0);

    window_rlnc_sender_t tx;
    window_rlnc_receiver_t rx;
    tx.init(64, 1250, 20, 10);
    tx.set_repair_rate(rate);
    rx.init();

    const int main_packets = 3000;
    const int flush_packets = 512;
    std::vector<unsigned char> seen((size_t)main_packets, 0);
    unsigned wire_seq = 1;

    for (int id = 0; id < main_packets + flush_packets; ++id) {
        char payload[64];
        int n = std::snprintf(payload, sizeof(payload), "flow-%d", id);
        assert(n > 0);

        std::vector<std::vector<char> > frames;
        assert(tx.encode_packet(payload, n + 1, frames) == 0);
        for (size_t i = 0; i < frames.size(); ++i, ++wire_seq) {
            if (id < main_packets && drop_wire(wire_seq))
                continue;

            std::vector<std::vector<char> > out;
            assert(rx.receive(frames[i].data(), (int)frames[i].size(), out) == 0);
            for (size_t j = 0; j < out.size(); ++j) {
                int got = -1;
                if (!out[j].empty() && std::sscanf(out[j].data(), "flow-%d", &got) == 1 &&
                    got >= 0 && got < main_packets)
                    seen[(size_t)got] = 1;
            }
        }
    }

    int missing = 0;
    for (int i = 0; i < main_packets; ++i)
        if (!seen[(size_t)i]) ++missing;
    double residual = (double)missing / (double)main_packets;

    assert(residual < 0.03);
    std::printf("window-rate: floor=0.175 RS=20:%d RLNC=%.4f residual=%.4f recovered=%llu\n",
                rs, rate, residual,
                (unsigned long long)rx.recovered_symbols());
}

int main() {
    test_single_symbol_recovery();
    test_fragment_reassembly_with_loss();
    test_window_rate_on_real_link_floor();
    std::printf("window-rlnc: source recovery + fragmented packet recovery + WindowRate OK\n");
    return 0;
}
