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
    // True only when this ACK produced a new delivery-rate observation. The
    // cached delivery_rate is still carried on other feedback for diagnostics,
    // but congestion control must not count it as another BBR sample.
    bool delivery_sampled;
    loss_snapshot_t loss;

    telemetry_feedback_t()
        : updated(false), acked_bytes(0), decided_bytes(0),
          rtt_s(0), delivery_rate(0), delivery_sampled(false) {}
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

    int consume(char *data, int &len, bool &is_control,
                telemetry_feedback_t &feedback);
    int build_ack(char *output, int output_cap, bool force = false);

    loss_snapshot_t snapshot() const;
    double min_rtt_s() const { return min_rtt_s_; }
    double smoothed_rtt_s() const { return srtt_s_; }
    double delivery_rate() const { return delivery_rate_; }

    static const int kDataHeader = 12;
    static const int kAckFrame = 28;

private:
    enum { kSentRing = 8192, kAckBits = 128, kReorderTolerance = 32 };

    struct sent_slot_t {
        uint64_t seq;
        double sent_at;
        int bytes;
        uint64_t sent_total;
        bool valid;
        bool acked;
        sent_slot_t()
            : seq(0), sent_at(0), bytes(0), sent_total(0),
              valid(false), acked(false) {}
    };

    bool enabled_;
    uint64_t tx_next_;
    uint64_t next_decide_;
    sent_slot_t sent_[kSentRing];
    uint64_t total_sent_bytes_;

    bool rx_started_;
    uint64_t rx_largest_;
    uint64_t rx_mask_lo_;
    uint64_t rx_mask_hi_;
    int rx_since_ack_;
    bool ack_pending_;
    double last_ack_at_;

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

    // BBR-shaped delivery sampler. It starts at the first positive ACK rather
    // than process startup, then bounds ACK arrival slope by the corresponding
    // send slope. This avoids both long-RTT first-sample dilution and ACK
    // compression. Only a completed pair of points produces delivery_sampled.
    bool rate_started_;
    uint64_t delivered_bytes_;
    double last_ack_point_time_;
    uint64_t last_ack_point_delivered_;
    double last_ack_point_sent_time_;
    uint64_t last_ack_point_sent_bytes_;

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
