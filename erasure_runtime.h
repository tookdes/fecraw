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
                                    fb.rtt_s, fb.delivery_rate,
                                    fb.delivery_sampled, fb.loss);

        // Report only genuine ACK/send-slope samples, at most once per second.
        // Stage 4 also exposes the two independent loss-model dimensions and
        // congestion scale: burst correlation is an FEC sizing input, while
        // only excess loss above the trusted erasure floor may reduce pacing.
        if (fb.delivery_sampled) {
            static double last_pacing_report = 0;
            double now = pacing_t::now_s();
            if (last_pacing_report <= 0 || now - last_pacing_report >= 1.0) {
                last_pacing_report = now;
                mylog(log_info,
                      "[%s] pacing sample delivered=%.3fMbps wire_bw=%.3fMbps rate=%.3fMbps state=%s ready=%d floor=%.3f recent=%.3f cong=%.3f burst=%.2f scale=%.2f rtt=%.1fms\n",
                      role,
                      fb.delivery_rate * 8.0 / 1000000.0,
                      (double)g_fecraw_pacing.max_wire_bw * 8.0 / 1000000.0,
                      (double)g_fecraw_pacing.pacing_rate * 8.0 / 1000000.0,
                      g_fecraw_pacing.state_name(),
                      g_fecraw_pacing.has_feedback() ? 1 : 0,
                      fb.loss.floor_trusted ? fb.loss.floor : 0.0,
                      fb.loss.recent,
                      fb.loss.congestive,
                      fb.loss.burst_factor,
                      g_fecraw_pacing.congestion_scale,
                      fb.rtt_s * 1000.0);
            }
        }
    }

    if (g_cfg.small_packet_threshold > 0) {
        double physical_loss = fb.loss.floor_trusted ? fb.loss.floor : fb.loss.loss;
        small_sender.update_loss(physical_loss);
    }

    if (g_cfg.fec_adaptive) {
        // RLNC asks a different sizing question than sealed-block RS. Keep its
        // continuous WindowRate current even when the integer RS ratio happens
        // not to change, so AUTO can switch codecs without inheriting 20:13.
        double window_rate = adaptive.get_window_rate();
        if (adaptive.adjust_window_rate(g_cfg.rlnc_window, fb.loss, fb.rtt_s,
                                        window_rate)) {
            mylog(log_info,
                  "[%s] RLNC window rate -> %.4f repair/source window=%d floor=%.3f rtt=%.1fms\n",
                  role, window_rate, g_cfg.rlnc_window, fb.loss.floor,
                  fb.rtt_s * 1000.0);
        }

        int data = 0, parity = 0;
        if (adaptive.adjust(fb.loss, fb.rtt_s, data, parity)) {
            char fec[64];
            snprintf(fec, sizeof(fec), "%d:%d", data, parity);

            // Do not rewrite fec_encode_manager's active parameters in the
            // middle of a block. UDPspeeder clones the versioned global table
            // only when its current block counter is zero.
            fec_parameter_t next;
            if (next.rs_from_str(fec) == 0) {
                int version = g_fec_par.version;
                g_fec_par.copy_fec(next);
                g_fec_par.version = version + 1;
                mylog(log_info,
                      "[%s] erasure RS scheduled -> %s floor=%.3f loss=%.3f burst=%.2f rtt=%.1fms\n",
                      role, fec, fb.loss.floor, fb.loss.loss, fb.loss.burst_factor,
                      fb.rtt_s * 1000.0);
            }
        }
    }
}

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

    char ack[buf_len];
    int ack_len = g_fecraw_telemetry.build_ack(ack, sizeof(ack));
    if (ack_len > 0)
        my_send(feedback_dest, ack, ack_len);
    return true;
}

static inline void fecraw_flush_wire_feedback(dest_t &feedback_dest) {
    if (!g_fecraw_telemetry.enabled()) return;
    char ack[buf_len];
    int ack_len = g_fecraw_telemetry.build_ack(ack, sizeof(ack), true);
    if (ack_len > 0)
        my_send(feedback_dest, ack, ack_len);
}

#endif // FECRAW_ERASURE_RUNTIME_H_
