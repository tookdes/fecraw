#ifndef FECRAW_ERASURE_RUNTIME_H_
#define FECRAW_ERASURE_RUNTIME_H_

#include "adaptive_fec.h"
#include "fecraw_config.h"
#include "pacing.h"
#include "small_packet.h"
#include "telemetry.h"

#include "connection.h"
#include "packet.h"
#include "log.h"

extern fecraw_config_t g_cfg;

static inline void fecraw_apply_feedback(conn_info_t &conn_info,
                                         adaptive_fec_t &adaptive,
                                         small_packet_sender_t &small_sender,
                                         const telemetry_feedback_t &fb,
                                         const char *role) {
    (void)conn_info;
    if (!fb.updated) return;

    if (g_cfg.enable_pacing) {
        g_fecraw_pacing.on_feedback(fb.acked_bytes, fb.decided_bytes,
                                    fb.rtt_s, fb.delivery_rate, fb.loss);
    }

    if (g_cfg.small_packet_threshold > 0) {
        double physical_loss = fb.loss.floor_trusted ? fb.loss.floor : fb.loss.loss;
        small_sender.update_loss(physical_loss);
    }

    if (g_cfg.fec_adaptive) {
        int data = 0, parity = 0;
        if (adaptive.adjust(fb.loss, fb.rtt_s, data, parity)) {
            char fec[64];
            snprintf(fec, sizeof(fec), "%d:%d", data, parity);

            // Do not rewrite fec_encode_manager's active parameters in the
            // middle of a block. UDPspeeder already has a versioned global
            // parameter handoff: input() clones g_fec_par only when its block
            // counter is zero. Publish the new RS table there and let the next
            // block adopt it atomically.
            fec_parameter_t next;
            if (next.rs_from_str(fec) == 0) {
                int version = g_fec_par.version;
                g_fec_par.copy_fec(next);
                g_fec_par.version = version + 1;
                mylog(log_info,
                      "[%s] erasure FEC scheduled -> %s floor=%.3f loss=%.3f burst=%.2f rtt=%.1fms\n",
                      role, fec, fb.loss.floor, fb.loss.loss, fb.loss.burst_factor,
                      fb.rtt_s * 1000.0);
            }
        }
    }
}

// Called after udp2raw decryption and UDPspeeder de_cook(), before the legacy
// fecraw header/FEC decoder. Returns false when the frame was protocol-v2
// control traffic or malformed and therefore must not enter the legacy path.
static inline bool fecraw_process_wire_input(conn_info_t &conn_info,
                                              dest_t &feedback_dest,
                                              char *data, int &len,
                                              adaptive_fec_t &adaptive,
                                              small_packet_sender_t &small_sender,
                                              const char *role) {
    if (!g_fecraw_telemetry.enabled()) return true;

    bool is_control = false;
    telemetry_feedback_t fb;
    if (g_fecraw_telemetry.consume(data, len, is_control, fb) != 0) {
        mylog(log_warn, "[%s] invalid protocol-v2 telemetry frame\n", role);
        return false;
    }

    fecraw_apply_feedback(conn_info, adaptive, small_sender, fb, role);
    if (is_control) return false;

    char ack[64];
    int ack_len = g_fecraw_telemetry.build_ack(ack, sizeof(ack));
    if (ack_len > 0)
        my_send(feedback_dest, ack, ack_len);
    return true;
}

// Flush a delayed ACK when the packet-count trigger did not fire. Both event
// loops run this on a 10ms timer so a sparse interactive exchange never waits
// for the 400ms connection timer before producing RTT feedback.
static inline void fecraw_flush_wire_feedback(dest_t &feedback_dest) {
    if (!g_fecraw_telemetry.enabled()) return;
    char ack[64];
    int ack_len = g_fecraw_telemetry.build_ack(ack, sizeof(ack), true);
    if (ack_len > 0)
        my_send(feedback_dest, ack, ack_len);
}

#endif // FECRAW_ERASURE_RUNTIME_H_
