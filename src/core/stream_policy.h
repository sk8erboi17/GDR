#ifndef GRD_STREAM_POLICY_H
#define GRD_STREAM_POLICY_H

#include "grd/common.h"

#include <stdbool.h>
#include <stdint.h>

#define GRD_CAPTURE_SHORT_GAP_US 100000ULL
#define GRD_CAPTURE_STABLE_REARM_US 150000ULL
/* Preserve a bounded amount of unused wire time for recovery IDRs. Ordinary
 * P-frames remain strictly paced. */
#define GRD_PACER_KEYFRAME_CREDIT_US 12000ULL

typedef enum grd_capture_recovery_phase {
    GRD_CAPTURE_PHASE_STABLE = 0,
    GRD_CAPTURE_PHASE_REQUESTED = 1,
    GRD_CAPTURE_PHASE_RECOVERING = 2
} grd_capture_recovery_phase;

typedef enum grd_capture_recovery_event {
    GRD_CAPTURE_EVENT_NONE = 0,
    GRD_CAPTURE_EVENT_REQUEST = 1,
    GRD_CAPTURE_EVENT_REARMED = 2,
    GRD_CAPTURE_EVENT_PLANNED_GAP = 3
} grd_capture_recovery_event;

typedef struct grd_capture_recovery_state {
    grd_capture_recovery_phase phase;
    uint64_t stable_since_micros;
    uint64_t raw_gap_count;
    uint64_t episode_count;
    uint64_t planned_gap_count;
} grd_capture_recovery_state;

/* Desktop Duplication legitimately returns WAIT_TIMEOUT when neither the
 * desktop bitmap nor the hardware pointer changed. The watchdog is armed
 * only by positive driver-stall evidence from the platform layer; ordinary
 * no-frame periods must never rebuild the D3D stack. */
typedef enum grd_capture_watchdog_stage {
    GRD_CAPTURE_WATCHDOG_STABLE = 0,
    GRD_CAPTURE_WATCHDOG_SESSION_RESET = 1,
    GRD_CAPTURE_WATCHDOG_DEVICE_RESET = 2
} grd_capture_watchdog_stage;

typedef enum grd_capture_watchdog_action {
    GRD_CAPTURE_WATCHDOG_NONE = 0,
    GRD_CAPTURE_WATCHDOG_RESET_SESSION = 1,
    GRD_CAPTURE_WATCHDOG_RESET_DEVICE = 2
} grd_capture_watchdog_action;

typedef struct grd_capture_watchdog_state {
    grd_capture_watchdog_stage stage;
    uint64_t last_action_micros;
    uint64_t last_session_reset_micros;
} grd_capture_watchdog_state;

void grd_capture_recovery_reset(grd_capture_recovery_state *state);
grd_capture_recovery_event grd_capture_recovery_on_frame(
    grd_capture_recovery_state *state,
    uint64_t previous_frame_micros,
    uint64_t frame_micros,
    uint64_t target_interval_micros,
    bool planned_interval
);
bool grd_capture_recovery_force(grd_capture_recovery_state *state);
void grd_capture_recovery_mark_started(grd_capture_recovery_state *state);
bool grd_capture_recovery_unsettled(
    const grd_capture_recovery_state *state
);
/* Time without a DXGI frame is normal when the desktop did not change. A
 * source discontinuity needs both a long interval and evidence that the
 * producer advanced (AccumulatedFrames) or blocked inside the driver. */
bool grd_capture_gap_is_discontinuity(
    uint64_t previous_frame_micros,
    uint64_t frame_micros,
    uint32_t accumulated_frames,
    bool driver_stalled
);

void grd_capture_watchdog_reset(grd_capture_watchdog_state *state);
grd_capture_watchdog_action grd_capture_watchdog_on_timeout(
    grd_capture_watchdog_state *state,
    uint64_t last_frame_micros,
    uint64_t now_micros,
    bool driver_stall_observed
);
void grd_capture_watchdog_on_frame(
    grd_capture_watchdog_state *state,
    uint64_t frame_micros
);
void grd_capture_watchdog_mark_session_reset(
    grd_capture_watchdog_state *state,
    uint64_t now_micros
);
void grd_capture_watchdog_mark_device_reset(
    grd_capture_watchdog_state *state,
    uint64_t now_micros
);
void grd_capture_watchdog_rearm(grd_capture_watchdog_state *state);

uint64_t grd_stream_pacer_schedule_start(
    uint64_t scheduled_micros,
    uint64_t now_micros,
    bool keyframe
);

/* A periodic/safety IDR is part of the valid encoded order and must remain
 * behind older P-frames. Only the first IDR repairing a broken reference
 * chain may preempt the queue; another repair already queued protects the
 * following P-chain. */
bool grd_stream_keyframe_preempts_queue(
    bool keyframe,
    bool video_discontinuity,
    bool recovery_keyframe_queued
);

/* Coalesce consecutive reference drops into one recovery episode. Ordinary
 * P-frame drops while an episode is open do not publish another IDR request;
 * failure of the queued repair IDR does. */
bool grd_stream_drop_requests_recovery(
    bool keyframe,
    bool video_discontinuity,
    bool recovery_keyframe_queued
);

/* Receiver-loss cuts wait until recovery settles, because the loss report
 * may include recovery traffic. Host-local initiating pressure is already
 * filtered to exclude recovery descendants and must remain actionable. */
bool grd_stream_abr_cut_allowed(
    uint64_t now_micros,
    uint64_t hold_until_micros,
    bool host_local_pressure,
    bool recovery_unsettled
);

typedef struct grd_stream_drop_counts {
    uint64_t sent;
    uint64_t admission;
    uint64_t queue;
    uint64_t deadline;
    uint64_t send;
    uint64_t discontinuity;
    uint64_t recovery_purge;
} grd_stream_drop_counts;

uint32_t grd_stream_initiating_drop_percent(
    const grd_stream_drop_counts *counts
);

/* FFmpeg's NVENC live reconfiguration resets rate control and forces an IDR.
 * Batch small ABR probes so a clean +200 kbps/s ramp cannot turn into one
 * large keyframe every second. Loss-protection changes remain immediate. */
bool grd_stream_rate_change_due(
    uint32_t active_kbps,
    uint32_t desired_kbps,
    uint64_t now_micros,
    uint64_t last_change_micros,
    bool protection_changed,
    bool urgent_decrease
);

/* Classify transport jitter independently from the producer cadence. A
 * Desktop Duplication source may legitimately publish at 30 fps (or pause)
 * while the negotiated ceiling is 120 fps; comparing arrival time with a
 * fixed 25 ms threshold turns that source behaviour into false network
 * gaps. source_delta_micros is preferred when available, while configured
 * fps provides a conservative fallback for malformed timestamps. */
bool grd_stream_arrival_gap_is_late(
    uint64_t arrival_delta_micros,
    uint64_t source_delta_micros,
    uint32_t configured_fps
);

#define GRD_FPS_CHANGE_REASON_PIPELINE (1U << 0U)
#define GRD_FPS_CHANGE_REASON_DROP (1U << 1U)
#define GRD_FPS_CHANGE_REASON_BACKLOG (1U << 2U)
#define GRD_FPS_CHANGE_REASON_RESTORE (1U << 3U)

typedef struct grd_stream_fps_pressure_state {
    uint32_t target_fps;
    uint32_t effective_fps;
    uint32_t pressure_score;
    uint64_t pipeline_ewma_x256;
    uint64_t last_change_micros;
    uint64_t clean_since_micros;
    uint64_t last_score_decay_micros;
    uint64_t last_drop_sample_generation;
    uint64_t pipeline_overload_since_micros;
    /* Snapshot retained across the controller reset so the log describes
     * the pressure that actually caused a change instead of always printing
     * zero after a downshift. */
    uint32_t last_change_pressure_score;
    uint32_t last_change_reasons;
} grd_stream_fps_pressure_state;

void grd_stream_fps_pressure_reset(
    grd_stream_fps_pressure_state *state,
    uint32_t target_fps,
    uint64_t now_micros
);
/* Keeps the encoder session open and adapts only capture/pacer cadence. The
 * requested rate remains the ceiling; sustained local pressure moves the
 * effective rate in small steps towards the measured sustainable rate. A
 * clean window restores it more slowly, avoiding 120/90 oscillation. */
uint32_t grd_stream_fps_pressure_update(
    grd_stream_fps_pressure_state *state,
    uint32_t target_fps,
    uint64_t now_micros,
    uint64_t pipeline_micros,
    uint32_t initiating_drop_percent,
    uint64_t initiating_drop_generation,
    uint32_t capture_backlog_frames
);

/* Convert the user's explicit client-upscale preference into the host
 * resolution rung. Malformed or future protocol values are clamped. */
uint32_t grd_stream_client_offload_level(int upscale_mode);
void grd_stream_ladder_max_dimensions(
    uint32_t requested_width,
    uint32_t requested_height,
    uint32_t level,
    uint32_t *width,
    uint32_t *height
);

bool grd_stream_encoder_configuration_changed(
    uint32_t active_width,
    uint32_t active_height,
    uint32_t active_fps,
    int active_codec,
    uint32_t desired_width,
    uint32_t desired_height,
    uint32_t desired_fps,
    int desired_codec
);

#endif
