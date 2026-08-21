#include "window_rlnc.h"

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
    tx.init(64, 700, 1, 1); // force a 2000-byte TUN packet into fragments
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

int main() {
    test_single_symbol_recovery();
    test_fragment_reassembly_with_loss();
    std::printf("window-rlnc: source recovery + fragmented packet recovery OK\n");
    return 0;
}
