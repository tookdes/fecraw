#include "window_rlnc.h"

#include <algorithm>
#include <climits>
#include <cstring>

namespace {

static const unsigned char kMagic0 = 0xF2;
static const unsigned char kMagic1 = 'R';
static const unsigned char kMagic2 = 'L';
static const unsigned char kVersion = 1;
static const unsigned char kKindSource = 0;
static const unsigned char kKindRepair = 1;
static const int kSourceHeader = 12;
static const int kRepairHeader = 16;
static const int kVectorMeta = 10; // meaningful_len + packet_id + frag_index + frag_count
static const int kMaxRepairWindow = 256;
static const int kDecoderWidth = 512;
static const int kMaxAssemblies = 1024;
static const int kTrimAssembliesTo = 512;

static void put_u16(char *p, uint16_t v) {
    p[0] = (char)(v >> 8);
    p[1] = (char)v;
}

static uint16_t get_u16(const char *p) {
    return (uint16_t)(((uint16_t)(unsigned char)p[0] << 8) |
                      (uint16_t)(unsigned char)p[1]);
}

static void put_u32(char *p, uint32_t v) {
    p[0] = (char)(v >> 24);
    p[1] = (char)(v >> 16);
    p[2] = (char)(v >> 8);
    p[3] = (char)v;
}

static uint32_t get_u32(const char *p) {
    return ((uint32_t)(unsigned char)p[0] << 24) |
           ((uint32_t)(unsigned char)p[1] << 16) |
           ((uint32_t)(unsigned char)p[2] << 8) |
           (uint32_t)(unsigned char)p[3];
}

struct gf256_tables_t {
    unsigned char exp[512];
    unsigned char log[256];

    gf256_tables_t() {
        std::memset(exp, 0, sizeof(exp));
        std::memset(log, 0, sizeof(log));
        unsigned int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = (unsigned char)x;
            log[x] = (unsigned char)i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11d;
        }
        for (int i = 255; i < 512; ++i)
            exp[i] = exp[i - 255];
    }
};

static gf256_tables_t &gf_tables() {
    static gf256_tables_t t;
    return t;
}

static unsigned char gf_mul(unsigned char a, unsigned char b) {
    if (a == 0 || b == 0) return 0;
    gf256_tables_t &t = gf_tables();
    return t.exp[(int)t.log[a] + (int)t.log[b]];
}

static unsigned char gf_inv(unsigned char a) {
    if (a == 0) return 0;
    gf256_tables_t &t = gf_tables();
    return t.exp[255 - (int)t.log[a]];
}

static void scale_bytes(unsigned char factor, std::vector<unsigned char> &v) {
    if (factor == 1) return;
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = gf_mul(factor, v[i]);
}

static void mul_slice_xor(unsigned char factor,
                          const std::vector<unsigned char> &in,
                          std::vector<unsigned char> &out) {
    if (factor == 0) return;
    size_t n = std::min(in.size(), out.size());
    if (factor == 1) {
        for (size_t i = 0; i < n; ++i) out[i] ^= in[i];
        return;
    }
    for (size_t i = 0; i < n; ++i)
        out[i] ^= gf_mul(factor, in[i]);
}

static void add_scaled(unsigned char factor,
                       const std::vector<unsigned char> &in,
                       std::vector<unsigned char> &out) {
    if (out.size() < in.size()) out.resize(in.size(), 0);
    mul_slice_xor(factor, in, out);
}

// Pinned to Queqiao protocol-1's coefficient generator. Coefficients are
// regenerated at the receiver rather than sent on the wire.
static unsigned char window_coefficient(uint32_t rid, int index) {
    uint32_t x = rid * 2654435761u + (uint32_t)index * 2246822519u;
    x ^= x >> 15;
    x *= 2654435761u;
    x ^= x >> 13;
    x *= 2246822519u;
    x ^= x >> 16;
    return (unsigned char)(x % 255u) + 1;
}

static void fill_magic(std::vector<char> &frame, unsigned char kind) {
    frame[0] = (char)kMagic0;
    frame[1] = (char)kMagic1;
    frame[2] = (char)kMagic2;
    frame[3] = (char)kVersion;
    frame[4] = (char)kind;
}

static bool magic_ok(const char *data, int len) {
    return len >= 5 &&
           (unsigned char)data[0] == kMagic0 &&
           (unsigned char)data[1] == kMagic1 &&
           (unsigned char)data[2] == kMagic2 &&
           (unsigned char)data[3] == kVersion;
}

} // namespace

window_rlnc_sender_t::window_rlnc_sender_t()
    : capacity_(64), mtu_(1250), data_shards_(20), parity_shards_(10),
      next_esi_(0), next_rid_(1), next_packet_id_(1), held_(0),
      repair_credit_(0), burst_symbols_(0), burst_repairs_(0) {}

void window_rlnc_sender_t::init(int window_size, int mtu,
                                int data_shards, int parity_shards) {
    capacity_ = std::max(4, std::min(kMaxRepairWindow, window_size));
    mtu_ = std::max(256, mtu);
    next_esi_ = 0;
    next_rid_ = 1;
    next_packet_id_ = 1;
    held_ = 0;
    repair_credit_ = 0;
    burst_symbols_ = 0;
    burst_repairs_ = 0;
    ring_.assign((size_t)capacity_, source_slot_t());
    set_rate(data_shards, parity_shards);
}

void window_rlnc_sender_t::set_rate(int data_shards, int parity_shards) {
    data_shards_ = std::max(1, data_shards);
    parity_shards_ = std::max(0, parity_shards);
}

void window_rlnc_sender_t::reset_window() {
    for (size_t i = 0; i < ring_.size(); ++i) {
        ring_[i].valid = false;
        ring_[i].vector.clear();
    }
    held_ = 0;
    repair_credit_ = 0;
    burst_symbols_ = 0;
    burst_repairs_ = 0;
}

bool window_rlnc_sender_t::is_frame(const char *data, int len) {
    if (!magic_ok(data, len)) return false;
    unsigned char kind = (unsigned char)data[4];
    return kind == kKindSource || kind == kKindRepair;
}

bool window_rlnc_sender_t::is_source_frame(const char *data, int len) {
    return magic_ok(data, len) && (unsigned char)data[4] == kKindSource;
}

bool window_rlnc_sender_t::is_repair_frame(const char *data, int len) {
    return magic_ok(data, len) && (unsigned char)data[4] == kKindRepair;
}

uint32_t window_rlnc_sender_t::add_source(const std::vector<unsigned char> &vector) {
    uint32_t esi = next_esi_++;
    source_slot_t &slot = ring_[esi % (uint32_t)capacity_];
    slot.esi = esi;
    slot.valid = true;
    slot.vector = vector;
    if (held_ < capacity_) ++held_;
    return esi;
}

bool window_rlnc_sender_t::build_repair(std::vector<char> &frame, int count_limit) {
    if (held_ <= 0 || ring_.empty()) return false;
    int count = std::min(held_, capacity_);
    if (count_limit > 0) count = std::min(count, count_limit);
    if (count <= 0) return false;
    uint32_t first = next_esi_ - (uint32_t)count;

    size_t vector_len = 0;
    for (int i = 0; i < count; ++i) {
        uint32_t esi = first + (uint32_t)i;
        const source_slot_t &s = ring_[esi % (uint32_t)capacity_];
        if (!s.valid || s.esi != esi) return false;
        vector_len = std::max(vector_len, s.vector.size());
    }
    if (vector_len == 0 || kRepairHeader + (int)vector_len > mtu_) return false;

    uint32_t rid = next_rid_++;
    std::vector<unsigned char> repair(vector_len, 0);
    for (int i = 0; i < count; ++i) {
        uint32_t esi = first + (uint32_t)i;
        const source_slot_t &s = ring_[esi % (uint32_t)capacity_];
        add_scaled(window_coefficient(rid, i), s.vector, repair);
    }

    frame.assign(kRepairHeader + repair.size(), 0);
    fill_magic(frame, kKindRepair);
    frame[5] = 0;
    put_u16(&frame[6], (uint16_t)count);
    put_u32(&frame[8], rid);
    put_u32(&frame[12], first);
    std::memcpy(&frame[kRepairHeader], &repair[0], repair.size());
    return true;
}

int window_rlnc_sender_t::protect_burst(
        int desired_total_repairs,
        std::vector<std::vector<char> > &frames) {
    frames.clear();
    if (burst_symbols_ <= 0) return 0;
    if (desired_total_repairs < 0) return -1;

    while (burst_repairs_ < desired_total_repairs) {
        std::vector<char> repair;
        if (!build_repair(repair, burst_symbols_)) return -1;
        frames.push_back(repair);
        ++burst_repairs_;
    }

    int added = (int)frames.size();
    burst_symbols_ = 0;
    burst_repairs_ = 0;
    repair_credit_ = 0;
    return added;
}

int window_rlnc_sender_t::encode_packet(const char *packet, int len,
                                        std::vector<std::vector<char> > &frames) {
    frames.clear();
    if (!packet || len <= 0 || ring_.empty()) return -1;

    // Repairs have the larger header, so size source vectors against it. This
    // guarantees every repair stays within the configured FEC MTU as well.
    int max_vector = mtu_ - kRepairHeader;
    int frag_payload = max_vector - kVectorMeta;
    if (frag_payload <= 0) return -1;

    int frag_count_i = (len + frag_payload - 1) / frag_payload;
    if (frag_count_i <= 0 || frag_count_i > 65535) return -1;
    uint16_t frag_count = (uint16_t)frag_count_i;
    uint32_t packet_id = next_packet_id_++;

    int offset = 0;
    for (uint16_t frag = 0; frag < frag_count; ++frag) {
        int payload_len = std::min(frag_payload, len - offset);
        int meaningful = kVectorMeta + payload_len;
        if (meaningful > 65535) return -1;

        std::vector<unsigned char> vector((size_t)meaningful, 0);
        vector[0] = (unsigned char)((uint16_t)meaningful >> 8);
        vector[1] = (unsigned char)meaningful;
        vector[2] = (unsigned char)(packet_id >> 24);
        vector[3] = (unsigned char)(packet_id >> 16);
        vector[4] = (unsigned char)(packet_id >> 8);
        vector[5] = (unsigned char)packet_id;
        vector[6] = (unsigned char)(frag >> 8);
        vector[7] = (unsigned char)frag;
        vector[8] = (unsigned char)(frag_count >> 8);
        vector[9] = (unsigned char)frag_count;
        std::memcpy(&vector[kVectorMeta], packet + offset, (size_t)payload_len);
        offset += payload_len;

        uint32_t esi = add_source(vector);
        std::vector<char> source((size_t)kSourceHeader + vector.size(), 0);
        fill_magic(source, kKindSource);
        source[5] = source[6] = source[7] = 0;
        put_u32(&source[8], esi);
        std::memcpy(&source[kSourceHeader], &vector[0], vector.size());
        frames.push_back(source);

        ++burst_symbols_;
        repair_credit_ += (double)parity_shards_ / (double)data_shards_;
        while (repair_credit_ + 1e-12 >= 1.0) {
            std::vector<char> repair;
            if (!build_repair(repair)) break;
            frames.push_back(repair);
            repair_credit_ -= 1.0;
            ++burst_repairs_;
        }

        // Once a complete RLNC window has flowed at the steady-state rate it
        // needs no special tail treatment. Only the suffix after this point is
        // vulnerable to producer drain, so begin a fresh burst accounting era.
        if (burst_symbols_ >= capacity_) {
            burst_symbols_ = 0;
            burst_repairs_ = 0;
        }
    }
    return 0;
}

class window_rlnc_receiver_t::decoder_t {
public:
    decoder_t() : mask_(kDecoderWidth - 1), lo_(0), high_(0), origin_(0),
                  started_(false), recovered_(0), discarded_(0) {
        slots_.resize(kDecoderWidth);
    }

    void reset() {
        slots_.assign(kDecoderWidth, slot_t());
        rows_.clear();
        lo_ = high_ = origin_ = 0;
        started_ = false;
        recovered_ = discarded_ = 0;
    }

    bool source(uint32_t esi, const std::vector<unsigned char> &vector,
                std::vector<std::vector<unsigned char> > &recovered) {
        if (!admit(esi)) return false;
        witness(esi);
        slot_t &s = slots_[esi & mask_];
        if (s.live && s.known && s.esi == esi) return false;
        substitute(esi, vector, recovered, false);
        harvest(recovered);
        return true;
    }

    void repair(uint32_t rid, uint32_t first, int count,
                const std::vector<unsigned char> &vector,
                std::vector<std::vector<unsigned char> > &recovered) {
        if (count <= 0 || count > kMaxRepairWindow || vector.empty()) {
            ++discarded_;
            return;
        }
        uint32_t newest = first + (uint32_t)(count - 1);
        if (!admit(newest)) {
            ++discarded_;
            return;
        }
        witness(first);
        if ((int32_t)(first - lo_) < 0) {
            ++discarded_;
            return;
        }

        std::vector<unsigned char> coef(kDecoderWidth, 0);
        for (int i = 0; i < count; ++i) {
            uint32_t esi = first + (uint32_t)i;
            if ((int32_t)(esi - lo_) < 0 || (int32_t)(esi - high_) >= 0)
                continue;
            coef[esi & mask_] = window_coefficient(rid, i);
        }
        insert(coef, vector, recovered);
    }

    uint64_t recovered() const { return recovered_; }
    uint64_t discarded() const { return discarded_; }

private:
    struct slot_t {
        uint32_t esi;
        bool live;
        bool known;
        std::vector<unsigned char> vector;
        slot_t() : esi(0), live(false), known(false) {}
    };

    struct row_t {
        uint32_t pivot;
        std::vector<unsigned char> coef;
        std::vector<unsigned char> data;
    };

    std::vector<slot_t> slots_;
    const uint32_t mask_;
    uint32_t lo_;
    uint32_t high_;
    uint32_t origin_;
    bool started_;
    std::vector<row_t> rows_;
    uint64_t recovered_;
    uint64_t discarded_;

    bool admit(uint32_t esi) {
        const uint32_t width = kDecoderWidth;
        if (!started_) {
            started_ = true;
            origin_ = esi;
            high_ = esi + 1;
            lo_ = high_ - width;
            return true;
        }
        if ((int32_t)(esi - lo_) < 0) return false;
        if ((int32_t)(esi - high_) >= 0) {
            high_ = esi + 1;
            uint32_t walked_until = lo_ + width;
            while (high_ - lo_ > width && (int32_t)(lo_ - walked_until) < 0) {
                evict(lo_);
                ++lo_;
            }
            if (high_ - lo_ > width) {
                // A malicious/garbled huge ESI jump must not turn into a
                // billion-iteration eviction walk.
                slots_.assign(kDecoderWidth, slot_t());
                rows_.clear();
                lo_ = high_ - width;
                origin_ = esi;
            }
        }
        return true;
    }

    void witness(uint32_t esi) {
        if (started_ && (int32_t)(esi - origin_) < 0 &&
            (int32_t)(esi - lo_) >= 0)
            origin_ = esi;
    }

    void evict(uint32_t esi) {
        uint32_t index = esi & mask_;
        slot_t &s = slots_[index];
        if (s.live && s.esi == esi) s = slot_t();

        std::vector<row_t> kept;
        kept.reserve(rows_.size());
        for (size_t i = 0; i < rows_.size(); ++i) {
            if (rows_[i].coef[index] != 0) {
                ++discarded_;
                continue;
            }
            kept.push_back(rows_[i]);
        }
        rows_.swap(kept);
    }

    std::pair<uint32_t, int> leading(const std::vector<unsigned char> &coef) const {
        uint32_t pivot = 0;
        int count = 0;
        for (uint32_t esi = lo_; (int32_t)(esi - high_) < 0; ++esi) {
            if (coef[esi & mask_] == 0) continue;
            if (count == 0) pivot = esi;
            ++count;
        }
        return std::make_pair(pivot, count);
    }

    void insert(std::vector<unsigned char> coef,
                std::vector<unsigned char> data,
                std::vector<std::vector<unsigned char> > &recovered) {
        // First remove symbols that arrived intact or were already recovered.
        for (uint32_t esi = lo_; (int32_t)(esi - high_) < 0; ++esi) {
            uint32_t index = esi & mask_;
            unsigned char c = coef[index];
            if (c == 0) continue;
            const slot_t &s = slots_[index];
            if (s.live && s.known && s.esi == esi) {
                add_scaled(c, s.vector, data);
                coef[index] = 0;
            }
        }

        // Existing rows are kept in reduced form: each pivot is unique.
        for (size_t i = 0; i < rows_.size(); ++i) {
            unsigned char c = coef[rows_[i].pivot & mask_];
            if (c == 0) continue;
            add_scaled(c, rows_[i].data, data);
            mul_slice_xor(c, rows_[i].coef, coef);
        }

        std::pair<uint32_t, int> lead = leading(coef);
        if (lead.second == 0) {
            ++discarded_;
            return;
        }

        uint32_t pivot = lead.first;
        unsigned char f = gf_inv(coef[pivot & mask_]);
        if (f == 0) {
            ++discarded_;
            return;
        }
        if (f != 1) {
            scale_bytes(f, coef);
            scale_bytes(f, data);
        }

        row_t row;
        row.pivot = pivot;
        row.coef.swap(coef);
        row.data.swap(data);

        // Clear the new pivot from all rows already held, maintaining RREF.
        for (size_t i = 0; i < rows_.size(); ++i) {
            unsigned char c = rows_[i].coef[pivot & mask_];
            if (c == 0) continue;
            add_scaled(c, row.data, rows_[i].data);
            mul_slice_xor(c, row.coef, rows_[i].coef);
        }
        rows_.push_back(row);
        harvest(recovered);
    }

    void harvest(std::vector<std::vector<unsigned char> > &recovered) {
        for (;;) {
            bool progress = false;
            for (size_t i = 0; i < rows_.size(); ++i) {
                std::pair<uint32_t, int> lead = leading(rows_[i].coef);
                if (lead.second == 0) {
                    rows_.erase(rows_.begin() + (long)i);
                    ++discarded_;
                    progress = true;
                    break;
                }
                if (lead.second == 1) {
                    uint32_t esi = lead.first;
                    std::vector<unsigned char> vector = rows_[i].data;
                    rows_.erase(rows_.begin() + (long)i);
                    substitute(esi, vector, recovered, true);
                    progress = true;
                    break;
                }
            }
            if (!progress) break;
        }
    }

    void substitute(uint32_t esi, const std::vector<unsigned char> &vector,
                    std::vector<std::vector<unsigned char> > &recovered,
                    bool recovered_symbol) {
        uint32_t index = esi & mask_;
        slot_t &s = slots_[index];
        if (s.live && s.known && s.esi == esi) return;
        s.esi = esi;
        s.live = true;
        s.known = true;
        s.vector = vector;

        if (recovered_symbol) {
            recovered.push_back(vector);
            ++recovered_;
        }

        for (size_t i = 0; i < rows_.size(); ++i) {
            unsigned char c = rows_[i].coef[index];
            if (c == 0) continue;
            add_scaled(c, vector, rows_[i].data);
            rows_[i].coef[index] = 0;
        }
    }
};

window_rlnc_receiver_t::window_rlnc_receiver_t()
    : decoder_(new decoder_t()) {}

void window_rlnc_receiver_t::init() {
    reset();
}

void window_rlnc_receiver_t::reset() {
    if (!decoder_) decoder_ = new decoder_t();
    decoder_->reset();
    assemblies_.clear();
}

uint64_t window_rlnc_receiver_t::recovered_symbols() const {
    return decoder_ ? decoder_->recovered() : 0;
}

uint64_t window_rlnc_receiver_t::discarded_equations() const {
    return decoder_ ? decoder_->discarded() : 0;
}

int window_rlnc_receiver_t::receive(const char *data, int len,
                                    std::vector<std::vector<char> > &packets) {
    packets.clear();
    if (!is_frame(data, len) || !decoder_) return -1;

    std::vector<std::vector<unsigned char> > recovered;
    unsigned char kind = (unsigned char)data[4];

    if (kind == kKindSource) {
        if (len <= kSourceHeader) return -1;
        uint32_t esi = get_u32(data + 8);
        std::vector<unsigned char> vector((size_t)(len - kSourceHeader));
        std::memcpy(&vector[0], data + kSourceHeader, vector.size());
        bool fresh = decoder_->source(esi, vector, recovered);
        if (fresh) accept_vector(vector, packets);
    } else if (kind == kKindRepair) {
        if (len <= kRepairHeader) return -1;
        int count = (int)get_u16(data + 6);
        uint32_t rid = get_u32(data + 8);
        uint32_t first = get_u32(data + 12);
        std::vector<unsigned char> vector((size_t)(len - kRepairHeader));
        std::memcpy(&vector[0], data + kRepairHeader, vector.size());
        decoder_->repair(rid, first, count, vector, recovered);
    } else {
        return -1;
    }

    for (size_t i = 0; i < recovered.size(); ++i)
        accept_vector(recovered[i], packets);
    trim_assemblies();
    return 0;
}

void window_rlnc_receiver_t::accept_vector(
        const std::vector<unsigned char> &vector,
        std::vector<std::vector<char> > &packets) {
    if (vector.size() < kVectorMeta) return;
    uint16_t meaningful = (uint16_t)(((uint16_t)vector[0] << 8) | vector[1]);
    if (meaningful < kVectorMeta || meaningful > vector.size()) return;

    uint32_t packet_id = ((uint32_t)vector[2] << 24) |
                         ((uint32_t)vector[3] << 16) |
                         ((uint32_t)vector[4] << 8) |
                         (uint32_t)vector[5];
    uint16_t frag = (uint16_t)(((uint16_t)vector[6] << 8) | vector[7]);
    uint16_t frag_count = (uint16_t)(((uint16_t)vector[8] << 8) | vector[9]);
    if (frag_count == 0 || frag >= frag_count || frag_count > 1024) return;

    std::vector<char> payload((size_t)(meaningful - kVectorMeta));
    if (!payload.empty())
        std::memcpy(&payload[0], &vector[kVectorMeta], payload.size());

    if (frag_count == 1) {
        packets.push_back(payload);
        return;
    }

    assembly_t &a = assemblies_[packet_id];
    if (a.frag_count != frag_count) {
        a = assembly_t();
        a.frag_count = frag_count;
        a.fragments.resize(frag_count);
        a.have.assign(frag_count, 0);
    }
    if (!a.have[frag]) {
        a.fragments[frag].swap(payload);
        a.have[frag] = 1;
        ++a.got;
    }
    if (a.got != a.frag_count) return;

    size_t total = 0;
    for (uint16_t i = 0; i < a.frag_count; ++i)
        total += a.fragments[i].size();
    std::vector<char> packet;
    packet.reserve(total);
    for (uint16_t i = 0; i < a.frag_count; ++i)
        packet.insert(packet.end(), a.fragments[i].begin(), a.fragments[i].end());
    packets.push_back(packet);
    assemblies_.erase(packet_id);
}

void window_rlnc_receiver_t::trim_assemblies() {
    if ((int)assemblies_.size() <= kMaxAssemblies) return;
    while ((int)assemblies_.size() > kTrimAssembliesTo)
        assemblies_.erase(assemblies_.begin());
}
