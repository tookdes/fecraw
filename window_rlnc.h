#ifndef FECRAW_WINDOW_RLNC_H_
#define FECRAW_WINDOW_RLNC_H_

/*
 * Sliding-window random linear erasure code for fecraw.
 *
 * Source symbols are transmitted immediately and repair symbols are random
 * linear combinations of the newest source window over GF(256). Large IP
 * packets are fragmented into source symbols and reassembled after recovery.
 */

#include <cstdint>
#include <map>
#include <vector>

class window_rlnc_sender_t {
public:
    window_rlnc_sender_t();

    void init(int window_size, int mtu, int data_shards, int parity_shards);
    void set_rate(int data_shards, int parity_shards);

    // Continuous sliding-window repair rate (repairs/source). The underlying
    // encoder already uses a fractional credit accumulator, so no codec or wire
    // change is needed: represent the rate with a fixed-point x:y pair and let
    // the existing credit mechanism emit repairs at that average.
    void set_repair_rate(double repairs_per_source) {
        if (repairs_per_source < 0) repairs_per_source = 0;
        if (repairs_per_source > 8.0) repairs_per_source = 8.0;
        const int scale = 1000000;
        data_shards_ = scale;
        parity_shards_ = (int)(repairs_per_source * scale + 0.5);
    }

    void reset_window();

    int encode_packet(const char *packet, int len,
                      std::vector<std::vector<char> > &frames);

    // Top the currently unfinished source burst up to desired_total_repairs.
    // Extra repairs cover exactly the trailing burst, not the whole retained
    // window. The wire format is unchanged: only the existing repair count/
    // first-ESI fields differ. On success the burst and fractional credit are
    // sealed exactly like Queqiao's protectBurst drain point.
    int protect_burst(int desired_total_repairs,
                      std::vector<std::vector<char> > &frames);

    int window_size() const { return capacity_; }
    int data_shards() const { return data_shards_; }
    int parity_shards() const { return parity_shards_; }
    double repair_rate() const {
        return data_shards_ > 0 ? (double)parity_shards_ / (double)data_shards_ : 0;
    }
    int pending_burst_symbols() const { return burst_symbols_; }
    int pending_burst_repairs() const { return burst_repairs_; }

    static bool is_frame(const char *data, int len);
    static bool is_source_frame(const char *data, int len);
    static bool is_repair_frame(const char *data, int len);

private:
    struct source_slot_t {
        uint32_t esi;
        bool valid;
        std::vector<unsigned char> vector;
        source_slot_t() : esi(0), valid(false) {}
    };

    int capacity_;
    int mtu_;
    int data_shards_;
    int parity_shards_;
    uint32_t next_esi_;
    uint32_t next_rid_;
    uint32_t next_packet_id_;
    int held_;
    double repair_credit_;
    int burst_symbols_;
    int burst_repairs_;
    std::vector<source_slot_t> ring_;

    uint32_t add_source(const std::vector<unsigned char> &vector);
    bool build_repair(std::vector<char> &frame, int count_limit = 0);
};

class window_rlnc_receiver_t {
public:
    window_rlnc_receiver_t();

    void init();
    void reset();

    int receive(const char *data, int len,
                std::vector<std::vector<char> > &packets);

    static bool is_frame(const char *data, int len) {
        return window_rlnc_sender_t::is_frame(data, len);
    }

    uint64_t recovered_symbols() const;
    uint64_t discarded_equations() const;

private:
    struct assembly_t {
        uint16_t frag_count;
        int got;
        std::vector<std::vector<char> > fragments;
        std::vector<unsigned char> have;
        assembly_t() : frag_count(0), got(0) {}
    };

    class decoder_t;
    decoder_t *decoder_;
    std::map<uint32_t, assembly_t> assemblies_;

    void accept_vector(const std::vector<unsigned char> &vector,
                       std::vector<std::vector<char> > &packets);
    void trim_assemblies();
};

#endif // FECRAW_WINDOW_RLNC_H_
