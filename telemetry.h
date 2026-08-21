#ifndef FECRAW_TELEMETRY_H_
#define FECRAW_TELEMETRY_H_

#include <cstdint>

struct loss_snapshot_t {
    double samples;
    double loss;
    double loss_after_arrival;
    double arrival_after_loss;
    double mean_burst;
    double burst_factor;
    bool memoryless;
    bool floor_trusted;
    double floor;
    double recent;
    double congestive;
    uint64_t decided;
    uint64_t reordered;

    loss_snapshot_t()
        : samples(0), loss(0), loss_after_arrival(0), arrival_after_loss(0),
          mean_burst(0), burst_factor(1), memoryless(false),
          floor_trusted(false), floor(0), recent(0), congestive(0),
          decided(0), reordered(0) {}
};

struct telemetry_feedback_t {
    bool updated;
    uint64_t acked_bytes;
    uint64_t decided_bytes;
    double rtt_s;
    double delivery_rate;
    loss_snapshot_t loss;

    telemetry_feedback_t()
        : updated(false), acked_bytes(0), decided_bytes(0),
          rtt_s(0), delivery_rate(0) {}
};

class telemetry_link_t {
public:
    telemetry_link_t();

    void init(bool enabled);
    bool enabled() const { return enabled_; }

    // Build a protocol-v2 DATA frame. commit_sent() must be called after the
    // local send is accepted, or when a paced packet is successfully queued.
    // send_after_s shifts the RTT epoch to the reserved wire time.
    int prepare_data(const char *input, int input_len,
                     char *output, int output_cap, uint64_t &seq);
    void commit_sent(uint64_t seq, int bytes, double send_after_s = 0);

    // Consume a de-cooked protocol-v2 frame in place. DATA frames are stripped
    // to their original fecraw payload. ACK frames set is_control=true and are
    // consumed completely. feedback is populated when ACK evidence decides
    // one or more locally-sent packet outcomes.
    int consume(char *data, int &len, bool &is_control,
                telemetry_feedback_t &feedback);

    // Build a feedback ACK frame. Returns 0 when no ACK is due.
    int build_ack(char *output, int output_cap, bool force = false);

    loss_snapshot_t snapshot() const;
    double min_rtt_s() const { return min_rtt_s_; }
    double smoothed_rtt_s() const { return srtt_s_; }
    double delivery_rate() const { return delivery_rate_; }

    static const int kDataHeader = 12; // magic/version/type + uint64 sequence
    static const int kAckFrame = 28;   // magic/version/type + largest + 128-bit map

private:
    enum { kSentRing = 8192, kAckBits = 128, kReorderTolerance = 32 };

    struct sent_slot_t {
        uint64_t seq;
        double sent_at;
        int bytes;
        bool valid;
        bool acked;
        sent_slot_t() : seq(0), sent_at(0), bytes(0), valid(false), acked(false) {}
    };

    bool enabled_;
    uint64_t tx_next_;
    uint64_t next_decide_;
    sent_slot_t sent_[kSentRing];

    bool rx_started_;
    uint64_t rx_largest_;
    uint64_t rx_mask_lo_;
    uint64_t rx_mask_hi_;
    int rx_since_ack_;
    bool ack_pending_;
    double last_ack_at_;

    // Markov loss estimator, adapted from Queqiao's lossmodel. The floor is a
    // lower envelope; only a statistically memoryless regime may establish it.
    double samples_;
    double losses_;
    double from_arrival_;
    double loss_after_arrival_;
    double from_loss_;
    double arrival_after_loss_;
    bool have_prev_;
    bool prev_lost_;
    int round_samples_;
    int round_losses_;
    double rounds_[8];
    int rounds_count_;
    int round_at_;
    uint64_t decided_;
    uint64_t reordered_;
    bool floor_trusted_;
    double established_floor_;

    double min_rtt_s_;
    double srtt_s_;
    double delivery_rate_;
    double rate_epoch_;
    uint64_t rate_acked_bytes_;

    static double now_s();
    static void put_u64(char *p, uint64_t v);
    static uint64_t get_u64(const char *p);
    static bool ack_bit(uint64_t lo, uint64_t hi, unsigned off);

    void note_received(uint64_t seq);
    void shift_ack_window(uint64_t delta);
    void record_outcome(bool arrived);
    void close_round();
    loss_snapshot_t raw_snapshot() const;
    void refresh_floor_trust();
    void process_ack(uint64_t largest, uint64_t mask_lo, uint64_t mask_hi,
                     telemetry_feedback_t &feedback);
};

extern telemetry_link_t g_fecraw_telemetry;

#endif // FECRAW_TELEMETRY_H_
