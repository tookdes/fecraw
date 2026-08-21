#include "telemetry.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <time.h>

telemetry_link_t g_fecraw_telemetry;

namespace {
static const unsigned char kMagic0 = 0xF3;
static const unsigned char kMagic1 = 0xEC;
static const unsigned char kVersion = 2;
static const unsigned char kTypeData = 1;
static const unsigned char kTypeAck = 2;
static const double kDecay = 0.75;
static const int kRoundSamples = 512;
static const int kMinMemorylessTransitions = 100;
static const double kAckInterval = 0.010;
static const int kAckEveryPackets = 8;
}

telemetry_link_t::telemetry_link_t() { init(false); }

void telemetry_link_t::init(bool enabled) {
    enabled_ = enabled;
    tx_next_ = 1;
    next_decide_ = 1;
    for (int i = 0; i < kSentRing; ++i) sent_[i] = sent_slot_t();

    rx_started_ = false;
    rx_largest_ = 0;
    rx_mask_lo_ = rx_mask_hi_ = 0;
    rx_since_ack_ = 0;
    ack_pending_ = false;
    last_ack_at_ = 0;

    samples_ = losses_ = from_arrival_ = loss_after_arrival_ = 0;
    from_loss_ = arrival_after_loss_ = 0;
    have_prev_ = prev_lost_ = false;
    round_samples_ = round_losses_ = 0;
    std::memset(rounds_, 0, sizeof(rounds_));
    rounds_count_ = round_at_ = 0;
    decided_ = reordered_ = 0;
    floor_trusted_ = false;
    established_floor_ = 0;

    min_rtt_s_ = 1e9;
    srtt_s_ = 0;
    delivery_rate_ = 0;
    rate_epoch_ = now_s();
    rate_acked_bytes_ = 0;
}

double telemetry_link_t::now_s() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void telemetry_link_t::put_u64(char *p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = (char)(v & 0xff);
        v >>= 8;
    }
}

uint64_t telemetry_link_t::get_u64(const char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | (uint64_t)(unsigned char)p[i];
    return v;
}

bool telemetry_link_t::ack_bit(uint64_t lo, uint64_t hi, unsigned off) {
    if (off >= 128) return false;
    if (off < 64) return (lo & (uint64_t(1) << off)) != 0;
    return (hi & (uint64_t(1) << (off - 64))) != 0;
}

int telemetry_link_t::prepare_data(const char *input, int input_len,
                                   char *output, int output_cap, uint64_t &seq) {
    if (!enabled_) return -1;
    if (input_len < 0 || output_cap < input_len + kDataHeader) return -1;
    seq = tx_next_++;
    output[0] = (char)kMagic0;
    output[1] = (char)kMagic1;
    output[2] = (char)kVersion;
    output[3] = (char)kTypeData;
    put_u64(output + 4, seq);
    std::memcpy(output + kDataHeader, input, input_len);
    return input_len + kDataHeader;
}

void telemetry_link_t::commit_sent(uint64_t seq, int bytes) {
    if (!enabled_ || seq == 0) return;
    sent_slot_t &s = sent_[seq % kSentRing];
    s.seq = seq;
    s.sent_at = now_s();
    s.bytes = bytes;
    s.valid = true;
    s.acked = false;
}

void telemetry_link_t::shift_ack_window(uint64_t delta) {
    if (delta >= 128) {
        rx_mask_lo_ = rx_mask_hi_ = 0;
        return;
    }
    if (delta >= 64) {
        rx_mask_hi_ = rx_mask_lo_ << (delta - 64);
        rx_mask_lo_ = 0;
        return;
    }
    if (delta > 0) {
        rx_mask_hi_ = (rx_mask_hi_ << delta) | (rx_mask_lo_ >> (64 - delta));
        rx_mask_lo_ <<= delta;
    }
}

void telemetry_link_t::note_received(uint64_t seq) {
    if (!rx_started_) {
        rx_started_ = true;
        rx_largest_ = seq;
        rx_mask_lo_ = 1;
    } else if (seq > rx_largest_) {
        uint64_t delta = seq - rx_largest_;
        shift_ack_window(delta);
        rx_largest_ = seq;
        rx_mask_lo_ |= 1;
    } else {
        uint64_t off64 = rx_largest_ - seq;
        if (off64 < 64)
            rx_mask_lo_ |= uint64_t(1) << off64;
        else if (off64 < 128)
            rx_mask_hi_ |= uint64_t(1) << (off64 - 64);
    }
    ++rx_since_ack_;
    ack_pending_ = true;
}

void telemetry_link_t::record_outcome(bool arrived) {
    ++decided_;
    samples_ += 1.0;
    if (!arrived) losses_ += 1.0;
    if (have_prev_) {
        if (prev_lost_) {
            from_loss_ += 1.0;
            if (arrived) arrival_after_loss_ += 1.0;
        } else {
            from_arrival_ += 1.0;
            if (!arrived) loss_after_arrival_ += 1.0;
        }
    }
    prev_lost_ = !arrived;
    have_prev_ = true;

    ++round_samples_;
    if (!arrived) ++round_losses_;
    if (round_samples_ >= kRoundSamples) close_round();
}

void telemetry_link_t::close_round() {
    double loss = round_samples_ > 0 ? (double)round_losses_ / round_samples_ : 0;
    if (rounds_count_ < 8) {
        rounds_[rounds_count_++] = loss;
    } else {
        rounds_[round_at_] = loss;
        round_at_ = (round_at_ + 1) % 8;
    }
    round_samples_ = round_losses_ = 0;
    samples_ *= kDecay;
    losses_ *= kDecay;
    from_arrival_ *= kDecay;
    loss_after_arrival_ *= kDecay;
    from_loss_ *= kDecay;
    arrival_after_loss_ *= kDecay;
    refresh_floor_trust();
}

loss_snapshot_t telemetry_link_t::raw_snapshot() const {
    loss_snapshot_t s;
    s.samples = samples_;
    s.decided = decided_;
    s.reordered = reordered_;
    if (samples_ <= 0) return s;

    s.loss = losses_ / samples_;
    if (from_arrival_ > 0) s.loss_after_arrival = loss_after_arrival_ / from_arrival_;
    if (from_loss_ > 0) {
        s.arrival_after_loss = arrival_after_loss_ / from_loss_;
        if (s.arrival_after_loss > 0) s.mean_burst = 1.0 / s.arrival_after_loss;
    }
    if (s.mean_burst > 0) s.burst_factor = s.mean_burst * (1.0 - s.loss);
    if (s.burst_factor < 1.0 || !std::isfinite(s.burst_factor)) s.burst_factor = 1.0;

    if (from_arrival_ >= kMinMemorylessTransitions && s.loss > 0 && s.loss < 1) {
        double stderr = std::sqrt(s.loss * (1.0 - s.loss) / from_arrival_);
        if (stderr > 0)
            s.memoryless = std::fabs(s.loss_after_arrival - s.loss) / stderr < 3.0;
    }

    double partial = round_samples_ > 0 ? (double)round_losses_ / round_samples_ : 0;
    double recent = partial;
    double minimum = partial;
    if (rounds_count_ > 0) {
        minimum = rounds_[0];
        for (int i = 1; i < rounds_count_; ++i) minimum = std::min(minimum, rounds_[i]);
        int last = rounds_count_ < 8 ? rounds_count_ - 1 : (round_at_ + 7) % 8;
        recent = rounds_[last];
        if (round_samples_ >= kRoundSamples / 4) {
            recent = partial;
            minimum = std::min(minimum, partial);
        }
    }
    s.recent = recent;
    s.floor = minimum;
    if (s.recent > s.floor) s.congestive = s.recent - s.floor;
    return s;
}

void telemetry_link_t::refresh_floor_trust() {
    loss_snapshot_t s = raw_snapshot();
    if (!s.memoryless || s.samples < kMinMemorylessTransitions) return;
    double candidate = s.floor > 0 ? s.floor : s.loss;
    if (candidate <= 0 || candidate >= 0.85) return;

    if (!floor_trusted_ || candidate < established_floor_) {
        floor_trusted_ = true;
        established_floor_ = candidate;
        return;
    }

    // fecraw does not replace the outer connection on every physical path
    // change. Once the old minimum has rotated out of a full eight-round
    // window, allow a persistently memoryless higher floor to become the new
    // baseline. Bursty queue loss fails the memoryless test and cannot ratchet
    // this upward.
    if (rounds_count_ == 8 && s.burst_factor < 1.30 &&
        candidate > established_floor_ * 1.15)
        established_floor_ = candidate;
}

loss_snapshot_t telemetry_link_t::snapshot() const {
    loss_snapshot_t s = raw_snapshot();
    s.floor_trusted = floor_trusted_;
    if (floor_trusted_) {
        s.floor = established_floor_;
        s.congestive = s.recent > s.floor ? s.recent - s.floor : 0;
    }
    return s;
}

void telemetry_link_t::process_ack(uint64_t largest, uint64_t mask_lo, uint64_t mask_hi,
                                   telemetry_feedback_t &feedback) {
    if (largest == 0 || next_decide_ == 0) return;
    double now = now_s();
    uint64_t acked_bytes = 0;
    uint64_t decided_bytes = 0;
    double best_rtt = 0;

    // Positive acknowledgements are sampled immediately, even if an earlier
    // gap is still inside the reorder tolerance. This keeps RTT and delivery
    // rate from inheriting artificial head-of-line delay from loss detection.
    for (unsigned off = 0; off < 128 && largest >= off; ++off) {
        if (!ack_bit(mask_lo, mask_hi, off)) continue;
        uint64_t seq = largest - off;
        if (seq < next_decide_) {
            ++reordered_;
            continue;
        }
        sent_slot_t &slot = sent_[seq % kSentRing];
        if (!slot.valid || slot.seq != seq || slot.acked) continue;
        slot.acked = true;
        acked_bytes += (uint64_t)std::max(slot.bytes, 0);
        double sample = now - slot.sent_at;
        if (sample > 0 && (best_rtt <= 0 || sample < best_rtt)) best_rtt = sample;
    }

    while (next_decide_ <= largest) {
        uint64_t distance = largest - next_decide_;
        sent_slot_t &slot = sent_[next_decide_ % kSentRing];
        bool arrived = slot.valid && slot.seq == next_decide_ && slot.acked;

        if (!arrived && distance < kReorderTolerance) break;
        if (!slot.valid || slot.seq != next_decide_) {
            ++next_decide_;
            continue;
        }

        record_outcome(arrived);
        decided_bytes += (uint64_t)std::max(slot.bytes, 0);
        slot.valid = false;
        ++next_decide_;
    }

    if (best_rtt > 0) {
        if (best_rtt < min_rtt_s_) min_rtt_s_ = best_rtt;
        if (srtt_s_ <= 0) srtt_s_ = best_rtt;
        else srtt_s_ = srtt_s_ * 0.875 + best_rtt * 0.125;
    }

    rate_acked_bytes_ += acked_bytes;
    double elapsed = now - rate_epoch_;
    if (elapsed >= 0.050) {
        double sample_rate = (double)rate_acked_bytes_ / elapsed;
        if (delivery_rate_ <= 0) delivery_rate_ = sample_rate;
        else delivery_rate_ = std::max(sample_rate, delivery_rate_ * 0.90);
        rate_acked_bytes_ = 0;
        rate_epoch_ = now;
    }

    refresh_floor_trust();
    if (acked_bytes || decided_bytes) {
        feedback.updated = true;
        feedback.acked_bytes = acked_bytes;
        feedback.decided_bytes = decided_bytes;
        feedback.rtt_s = best_rtt > 0 ? best_rtt : srtt_s_;
        feedback.delivery_rate = delivery_rate_;
        feedback.loss = snapshot();
    }
}

int telemetry_link_t::consume(char *data, int &len, bool &is_control,
                              telemetry_feedback_t &feedback) {
    is_control = false;
    feedback = telemetry_feedback_t();
    if (!enabled_) return 0;
    if (len < 4 || (unsigned char)data[0] != kMagic0 ||
        (unsigned char)data[1] != kMagic1 || (unsigned char)data[2] != kVersion)
        return -1;

    unsigned char type = (unsigned char)data[3];
    if (type == kTypeData) {
        if (len < kDataHeader) return -1;
        uint64_t seq = get_u64(data + 4);
        note_received(seq);
        len -= kDataHeader;
        std::memmove(data, data + kDataHeader, len);
        return 0;
    }
    if (type == kTypeAck) {
        if (len != kAckFrame) return -1;
        uint64_t largest = get_u64(data + 4);
        uint64_t mask_lo = get_u64(data + 12);
        uint64_t mask_hi = get_u64(data + 20);
        process_ack(largest, mask_lo, mask_hi, feedback);
        is_control = true;
        len = 0;
        return 0;
    }
    return -1;
}

int telemetry_link_t::build_ack(char *output, int output_cap, bool force) {
    if (!enabled_ || !rx_started_ || !ack_pending_ || output_cap < kAckFrame) return 0;
    double now = now_s();
    if (!force && rx_since_ack_ < kAckEveryPackets && last_ack_at_ > 0 &&
        now - last_ack_at_ < kAckInterval)
        return 0;

    output[0] = (char)kMagic0;
    output[1] = (char)kMagic1;
    output[2] = (char)kVersion;
    output[3] = (char)kTypeAck;
    put_u64(output + 4, rx_largest_);
    put_u64(output + 12, rx_mask_lo_);
    put_u64(output + 20, rx_mask_hi_);
    ack_pending_ = false;
    rx_since_ack_ = 0;
    last_ack_at_ = now;
    return kAckFrame;
}
