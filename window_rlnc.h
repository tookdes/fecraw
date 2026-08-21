#ifndef FECRAW_WINDOW_RLNC_H_
#define FECRAW_WINDOW_RLNC_H_

/*
 * Sliding-window random linear erasure code for fecraw.
 *
 * The design follows Queqiao's window code rather than UDPspeeder's sealed RS
 * blocks: source symbols are transmitted immediately and repair symbols are
 * random linear combinations of the newest source window over GF(256). A
 * repair emitted now can therefore recover an older erasure without waiting
 * for a block boundary.
 *
 * This implementation is packet-oriented for the TUN data plane. Large IP
 * packets are fragmented into source symbols; each symbol carries enough
 * metadata to reassemble the original packet after recovery.
 */

#include <cstdint>
#include <map>
#include <vector>

class window_rlnc_sender_t {
public:
    window_rlnc_sender_t();

    void init(int window_size, int mtu, int data_shards, int parity_shards);
    void set_rate(int data_shards, int parity_shards);
    void reset_window();

    // Encode one TUN packet. frames contains source frames plus zero or more
    // repair frames, each ready for the legacy fecraw header + my_send().
    int encode_packet(const char *packet, int len,
                      std::vector<std::vector<char> > &frames);

    int window_size() const { return capacity_; }
    int data_shards() const { return data_shards_; }
    int parity_shards() const { return parity_shards_; }

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
    std::vector<source_slot_t> ring_;

    uint32_t add_source(const std::vector<unsigned char> &vector);
    bool build_repair(std::vector<char> &frame);
};

class window_rlnc_receiver_t {
public:
    window_rlnc_receiver_t();

    void init();
    void reset();

    // Consume one RLNC source/repair frame. Any complete original TUN packets
    // produced by this arrival are appended to packets.
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
