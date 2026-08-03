#include "core/stream_policy.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

/* A timeout is not a fault by itself: DXGI only publishes when the desktop
 * bitmap or hardware pointer changes. Only a call that demonstrably blocked
 * far beyond its requested timeout arms this watchdog. */
#define GRD_CAPTURE_WATCHDOG_SESSION_STALL_US 1000000ULL
#define GRD_CAPTURE_WATCHDOG_DEVICE_STALL_US 2500000ULL
#define GRD_CAPTURE_WATCHDOG_DEVICE_RETRY_US 5000000ULL
#define GRD_CAPTURE_WATCHDOG_FAST_REFAIL_US 5000000ULL

#define GRD_FPS_PRESSURE_SCORE_LIMIT 12U
#define GRD_FPS_CHANGE_MIN_INTERVAL_US 500000ULL
#define GRD_FPS_PRESSURE_DECAY_INTERVAL_US 250000ULL
#define GRD_FPS_PIPELINE_CONFIRM_US 250000ULL
#define GRD_FPS_RESTORE_CLEAN_US 5000000ULL
#define GRD_FPS_RESTORE_STEP_INTERVAL_US 2000000ULL
#define GRD_FPS_DOWN_STEP 4U
#define GRD_FPS_SEVERE_DOWN_STEP 8U
#define GRD_FPS_UP_STEP 4U
#define GRD_FPS_PIPELINE_HEADROOM_PERCENT 82U
#define GRD_RATE_CHANGE_MIN_KBPS 1000U
#define GRD_RATE_CHANGE_MIN_PERCENT 5U
#define GRD_RATE_DOWN_MIN_INTERVAL_US 2000000ULL
#define GRD_RATE_UP_MIN_INTERVAL_US 15000000ULL

void grd_capture_recovery_reset(grd_capture_recovery_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
        state->phase = GRD_CAPTURE_PHASE_STABLE;
    }
}

grd_capture_recovery_event grd_capture_recovery_on_frame(
    grd_capture_recovery_state *state,
    uint64_t previous_frame_micros,
    uint64_t frame_micros,
    uint64_t target_interval_micros,
    bool planned_interval
)
{
    if (state == NULL) {
        return GRD_CAPTURE_EVENT_NONE;
    }
    if (planned_interval) {
        if (previous_frame_micros != 0U &&
            frame_micros > previous_frame_micros &&
            frame_micros - previous_frame_micros >=
                GRD_CAPTURE_SHORT_GAP_US) {
            ++state->planned_gap_count;
        }
        state->stable_since_micros = 0U;
        return GRD_CAPTURE_EVENT_PLANNED_GAP;
    }
    if (previous_frame_micros == 0U ||
        frame_micros <= previous_frame_micros) {
        return GRD_CAPTURE_EVENT_NONE;
    }

    const uint64_t gap = frame_micros - previous_frame_micros;
    if (gap >= GRD_CAPTURE_SHORT_GAP_US) {
        ++state->raw_gap_count;
        state->stable_since_micros = 0U;
        if (state->phase == GRD_CAPTURE_PHASE_STABLE) {
            state->phase = GRD_CAPTURE_PHASE_REQUESTED;
            ++state->episode_count;
            return GRD_CAPTURE_EVENT_REQUEST;
        }
        return GRD_CAPTURE_EVENT_NONE;
    }

    if (state->phase != GRD_CAPTURE_PHASE_RECOVERING) {
        return GRD_CAPTURE_EVENT_NONE;
    }
    uint64_t stable_limit = target_interval_micros * 2U + 2000U;
    if (stable_limit < target_interval_micros) {
        stable_limit = UINT64_MAX;
    }
    if (gap > stable_limit) {
        state->stable_since_micros = 0U;
        return GRD_CAPTURE_EVENT_NONE;
    }
    if (state->stable_since_micros == 0U) {
        state->stable_since_micros = frame_micros;
        return GRD_CAPTURE_EVENT_NONE;
    }
    if (frame_micros - state->stable_since_micros >=
        GRD_CAPTURE_STABLE_REARM_US) {
        state->phase = GRD_CAPTURE_PHASE_STABLE;
        state->stable_since_micros = 0U;
        return GRD_CAPTURE_EVENT_REARMED;
    }
    return GRD_CAPTURE_EVENT_NONE;
}

bool grd_capture_recovery_force(grd_capture_recovery_state *state)
{
    if (state == NULL || state->phase == GRD_CAPTURE_PHASE_REQUESTED) {
        return false;
    }
    state->phase = GRD_CAPTURE_PHASE_REQUESTED;
    state->stable_since_micros = 0U;
    ++state->episode_count;
    return true;
}

void grd_capture_recovery_mark_started(grd_capture_recovery_state *state)
{
    if (state != NULL && state->phase == GRD_CAPTURE_PHASE_REQUESTED) {
        state->phase = GRD_CAPTURE_PHASE_RECOVERING;
        state->stable_since_micros = 0U;
    }
}

bool grd_capture_recovery_unsettled(
    const grd_capture_recovery_state *state
)
{
    return state != NULL && state->phase != GRD_CAPTURE_PHASE_STABLE;
}

bool grd_capture_gap_is_discontinuity(
    uint64_t previous_frame_micros,
    uint64_t frame_micros,
    uint32_t accumulated_frames,
    bool driver_stalled
)
{
    if (previous_frame_micros == 0U ||
        frame_micros <= previous_frame_micros ||
        frame_micros - previous_frame_micros < GRD_CAPTURE_SHORT_GAP_US) {
        return false;
    }
    return driver_stalled || accumulated_frames > 1U;
}

void grd_capture_watchdog_reset(grd_capture_watchdog_state *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
        state->stage = GRD_CAPTURE_WATCHDOG_STABLE;
    }
}

grd_capture_watchdog_action grd_capture_watchdog_on_timeout(
    grd_capture_watchdog_state *state,
    uint64_t last_frame_micros,
    uint64_t now_micros,
    bool driver_stall_observed
)
{
    if (state == NULL || last_frame_micros == 0U ||
        now_micros <= last_frame_micros) {
        return GRD_CAPTURE_WATCHDOG_NONE;
    }

    const uint64_t stalled_for = now_micros - last_frame_micros;

    if (state->stage == GRD_CAPTURE_WATCHDOG_STABLE) {
        if (!driver_stall_observed ||
            stalled_for < GRD_CAPTURE_WATCHDOG_SESSION_STALL_US) {
            return GRD_CAPTURE_WATCHDOG_NONE;
        }

        /* A lightweight recreation that yielded a frame and then failed
         * again under fresh input did not actually cure the capture path.
         * Escalate that quick relapse instead of looping forever through
         * one-frame duplication sessions. The timestamp is cleared after a
         * full second of stable cadence by grd_capture_watchdog_rearm(). */
        const bool recent_session_refailure =
            state->last_session_reset_micros != 0U &&
            now_micros >= state->last_session_reset_micros &&
            now_micros - state->last_session_reset_micros <=
                GRD_CAPTURE_WATCHDOG_FAST_REFAIL_US;
        if (recent_session_refailure) {
            state->stage = GRD_CAPTURE_WATCHDOG_DEVICE_RESET;
            state->last_action_micros = now_micros;
            return GRD_CAPTURE_WATCHDOG_RESET_DEVICE;
        }
        grd_capture_watchdog_mark_session_reset(state, now_micros);
        return GRD_CAPTURE_WATCHDOG_RESET_SESSION;
    }

    /* No frame at all followed the session recreation. The initial input is
     * sufficient evidence to escalate even if the user has since released
     * the key; this prevents a genuinely dead duplication object from being
     * left indefinitely at the lightweight stage. */
    if (state->stage == GRD_CAPTURE_WATCHDOG_SESSION_RESET) {
        if (!driver_stall_observed ||
            stalled_for < GRD_CAPTURE_WATCHDOG_DEVICE_STALL_US) {
            return GRD_CAPTURE_WATCHDOG_NONE;
        }
        state->stage = GRD_CAPTURE_WATCHDOG_DEVICE_RESET;
        state->last_action_micros = now_micros;
        return GRD_CAPTURE_WATCHDOG_RESET_DEVICE;
    }

    if (driver_stall_observed &&
        stalled_for >= GRD_CAPTURE_WATCHDOG_DEVICE_STALL_US &&
        now_micros >= state->last_action_micros &&
        now_micros - state->last_action_micros >=
            GRD_CAPTURE_WATCHDOG_DEVICE_RETRY_US) {
        state->last_action_micros = now_micros;
        return GRD_CAPTURE_WATCHDOG_RESET_DEVICE;
    }
    return GRD_CAPTURE_WATCHDOG_NONE;
}

void grd_capture_watchdog_on_frame(
    grd_capture_watchdog_state *state,
    uint64_t frame_micros
)
{
    if (state != NULL && frame_micros != 0U &&
        state->stage != GRD_CAPTURE_WATCHDOG_STABLE) {
        /* Any real post-reset frame proves that the requested recreation
         * completed. Preserve last_session_reset_micros until stable cadence
         * is confirmed so a quick one-frame relapse escalates correctly. */
        state->stage = GRD_CAPTURE_WATCHDOG_STABLE;
    }
}

void grd_capture_watchdog_mark_session_reset(
    grd_capture_watchdog_state *state,
    uint64_t now_micros
)
{
    if (state != NULL) {
        state->stage = GRD_CAPTURE_WATCHDOG_SESSION_RESET;
        state->last_action_micros = now_micros;
        state->last_session_reset_micros = now_micros;
    }
}

void grd_capture_watchdog_mark_device_reset(
    grd_capture_watchdog_state *state,
    uint64_t now_micros
)
{
    if (state != NULL) {
        state->stage = GRD_CAPTURE_WATCHDOG_DEVICE_RESET;
        state->last_action_micros = now_micros;
    }
}

void grd_capture_watchdog_rearm(grd_capture_watchdog_state *state)
{
    grd_capture_watchdog_reset(state);
}

uint64_t grd_stream_pacer_schedule_start(
    uint64_t scheduled_micros,
    uint64_t now_micros,
    bool keyframe
)
{
    if (scheduled_micros == 0U) {
        return now_micros;
    }
    if (scheduled_micros >= now_micros) {
        return scheduled_micros;
    }
    if (!keyframe) {
        return now_micros;
    }
    const uint64_t earliest = now_micros > GRD_PACER_KEYFRAME_CREDIT_US
                                  ? now_micros -
                                        GRD_PACER_KEYFRAME_CREDIT_US
                                  : 0U;
    return scheduled_micros < earliest ? earliest : scheduled_micros;
}

bool grd_stream_keyframe_preempts_queue(
    bool keyframe,
    bool video_discontinuity,
    bool recovery_keyframe_queued
)
{
    return keyframe && video_discontinuity &&
           !recovery_keyframe_queued;
}

bool grd_stream_drop_requests_recovery(
    bool keyframe,
    bool video_discontinuity,
    bool recovery_keyframe_queued
)
{
    if (!video_discontinuity) {
        return true;
    }
    return keyframe && recovery_keyframe_queued;
}

bool grd_stream_abr_cut_allowed(
    uint64_t now_micros,
    uint64_t hold_until_micros,
    bool host_local_pressure,
    bool recovery_unsettled
)
{
    return now_micros >= hold_until_micros &&
           (host_local_pressure || !recovery_unsettled);
}

uint32_t grd_stream_initiating_drop_percent(
    const grd_stream_drop_counts *counts
)
{
    if (counts == NULL) {
        return 0U;
    }
    const uint64_t initiating = counts->admission + counts->queue +
                                counts->deadline + counts->send;
    const uint64_t total = counts->sent + initiating;
    if (total == 0U) {
        return 0U;
    }
    const uint64_t rounded = initiating * 100ULL + total - 1ULL;
    const uint64_t percent = rounded / total;
    return percent > 100U ? 100U : (uint32_t)percent;
}

bool grd_stream_rate_change_due(
    uint32_t active_kbps,
    uint32_t desired_kbps,
    uint64_t now_micros,
    uint64_t last_change_micros,
    bool protection_changed,
    bool urgent_decrease
)
{
    if (active_kbps == 0U || desired_kbps == 0U ||
        active_kbps == desired_kbps) {
        return protection_changed && active_kbps != 0U;
    }
    if (protection_changed) {
        return true;
    }
    const uint32_t delta = active_kbps > desired_kbps
                               ? active_kbps - desired_kbps
                               : desired_kbps - active_kbps;
    const uint32_t relative = (uint32_t)(
        ((uint64_t)active_kbps * GRD_RATE_CHANGE_MIN_PERCENT + 99U) /
        100U
    );
    const uint32_t threshold = relative > GRD_RATE_CHANGE_MIN_KBPS
                                   ? relative
                                   : GRD_RATE_CHANGE_MIN_KBPS;
    if (delta < threshold) {
        return false;
    }
    if (desired_kbps < active_kbps && urgent_decrease) {
        return true;
    }
    if (now_micros < last_change_micros) {
        return false;
    }
    return now_micros - last_change_micros >=
        (desired_kbps < active_kbps
             ? GRD_RATE_DOWN_MIN_INTERVAL_US
             : GRD_RATE_UP_MIN_INTERVAL_US);
}

bool grd_stream_arrival_gap_is_late(
    uint64_t arrival_delta_micros,
    uint64_t source_delta_micros,
    uint32_t configured_fps
)
{
    uint64_t expected = source_delta_micros;
    if (expected == 0U) {
        const uint32_t safe_fps = configured_fps >= 1U &&
                                          configured_fps <= 1000U
                                      ? configured_fps
                                      : 60U;
        expected = 1000000ULL / safe_fps;
    }

    /* Allow half a source interval of arrival variation. Keep a 4 ms floor
     * for high-rate streams and preserve the historical 25 ms minimum so a
     * single scheduler wake-up at 120 fps is not treated as packet loss. */
    uint64_t tolerance = expected / 2U;
    if (tolerance < 4000U) {
        tolerance = 4000U;
    }
    uint64_t threshold = expected > UINT64_MAX - tolerance
                             ? UINT64_MAX
                             : expected + tolerance;
    if (threshold < 25000U) {
        threshold = 25000U;
    }
    return arrival_delta_micros > threshold;
}

void grd_stream_fps_pressure_reset(
    grd_stream_fps_pressure_state *state,
    uint32_t target_fps,
    uint64_t now_micros
)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
        state->target_fps = target_fps;
        state->effective_fps = target_fps;
        state->last_change_micros = now_micros;
        state->last_score_decay_micros = now_micros;
    }
}

static uint32_t fps_pressure_floor(uint32_t target_fps)
{
    if (target_fps >= 100U) {
        return 60U;
    }
    if (target_fps >= 60U) {
        return 45U;
    }
    return 30U;
}

static uint32_t fps_step_down(
    uint32_t current,
    uint32_t desired,
    uint32_t floor,
    uint32_t maximum_step
)
{
    if (current <= floor) {
        return floor;
    }
    if (desired >= current || current - desired < 4U) {
        desired = current > 4U ? current - 4U : floor;
    }
    const uint32_t step_floor = current > maximum_step
                                    ? current - maximum_step
                                    : floor;
    if (desired < step_floor) {
        desired = step_floor;
    }
    if (desired < floor) {
        desired = floor;
    }
    /* Even rates keep the rational capture/pacer interval predictable while
     * still allowing much finer control than a hard 120 -> 90 transition. */
    desired &= ~1U;
    return desired < floor ? floor : desired;
}

uint32_t grd_stream_fps_pressure_update(
    grd_stream_fps_pressure_state *state,
    uint32_t target_fps,
    uint64_t now_micros,
    uint64_t pipeline_micros,
    uint32_t initiating_drop_percent,
    uint64_t initiating_drop_generation,
    uint32_t capture_backlog_frames
)
{
    if (state == NULL || target_fps < 30U) {
        return target_fps;
    }
    if (state->target_fps != target_fps || state->effective_fps == 0U) {
        grd_stream_fps_pressure_reset(state, target_fps, now_micros);
    }
    const uint32_t current = state->effective_fps;
    const uint64_t interval = current != 0U
                                  ? 1000000ULL / current
                                  : 33333ULL;
    if (pipeline_micros != 0U) {
        /* A single driver hiccup is real, but feeding its complete duration
         * into the EWMA would make the controller react to an already-finished
         * event for seconds. Cap an individual sample while the separate
         * backlog signal accounts for how many frames were actually missed. */
        const uint64_t sample_limit = interval * 2ULL;
        const uint64_t bounded_sample = pipeline_micros < sample_limit
                                            ? pipeline_micros
                                            : sample_limit;
        const uint64_t sample = bounded_sample << 8U;
        state->pipeline_ewma_x256 = state->pipeline_ewma_x256 == 0U
            ? sample
            : (state->pipeline_ewma_x256 * 7U + sample) / 8U;
    }
    const bool pipeline_overloaded = pipeline_micros != 0U &&
        pipeline_micros * 100ULL > interval * 90ULL;
    if (pipeline_overloaded) {
        if (state->pipeline_overload_since_micros == 0U) {
            state->pipeline_overload_since_micros = now_micros;
        }
    } else {
        state->pipeline_overload_since_micros = 0U;
    }
    const bool fresh_drop_sample = initiating_drop_percent != 0U &&
        initiating_drop_generation != 0U &&
        initiating_drop_generation != state->last_drop_sample_generation;
    if (fresh_drop_sample) {
        state->last_drop_sample_generation = initiating_drop_generation;
    }
    const bool severe = capture_backlog_frames >= 4U ||
        initiating_drop_percent >= 15U ||
        (pipeline_micros != 0U &&
         pipeline_micros * 100ULL > interval * 140ULL);
    const bool pressured = capture_backlog_frames != 0U ||
                           fresh_drop_sample || pipeline_overloaded;
    if (pressured) {
        uint32_t increment = pipeline_overloaded ? 1U : 0U;
        if (capture_backlog_frames != 0U) {
            const uint32_t backlog_increment =
                capture_backlog_frames >= 4U
                    ? 8U
                    : capture_backlog_frames * 3U;
            increment += backlog_increment;
        }
        if (fresh_drop_sample) {
            increment += initiating_drop_percent >= 15U
                             ? GRD_FPS_PRESSURE_SCORE_LIMIT
                             : initiating_drop_percent >= 5U ? 8U : 4U;
        }
        state->pressure_score =
            state->pressure_score > UINT32_MAX - increment
                ? UINT32_MAX
                : state->pressure_score + increment;
        state->last_score_decay_micros = now_micros;
        state->clean_since_micros = 0U;
    } else {
        if (state->pressure_score != 0U &&
            now_micros >= state->last_score_decay_micros &&
            now_micros - state->last_score_decay_micros >=
                GRD_FPS_PRESSURE_DECAY_INTERVAL_US) {
            --state->pressure_score;
            state->last_score_decay_micros = now_micros;
        }
        const bool ample_headroom = pipeline_micros == 0U ||
            pipeline_micros * 100ULL <= interval * 80ULL;
        if (ample_headroom && initiating_drop_percent == 0U &&
            state->clean_since_micros == 0U) {
            state->clean_since_micros = now_micros;
        } else if (!ample_headroom || initiating_drop_percent != 0U) {
            state->clean_since_micros = 0U;
        }
    }

    const bool change_allowed = now_micros >= state->last_change_micros &&
        now_micros - state->last_change_micros >=
            GRD_FPS_CHANGE_MIN_INTERVAL_US;
    const bool sustained_pipeline =
        state->pipeline_overload_since_micros != 0U &&
        now_micros >= state->pipeline_overload_since_micros &&
        now_micros - state->pipeline_overload_since_micros >=
            GRD_FPS_PIPELINE_CONFIRM_US;
    /* A local drop sample already summarizes a full rolling second. Capture
     * backlog, instead, needs either a repeated event (score >= limit) or
     * sustained pipeline pressure; one completed map-load hiccup must not
     * lower the session after the work has already recovered. */
    const bool repeated_capture_backlog =
        capture_backlog_frames != 0U &&
        state->pressure_score >= GRD_FPS_PRESSURE_SCORE_LIMIT;
    const bool actionable_pressure = sustained_pipeline || fresh_drop_sample ||
                                     repeated_capture_backlog;
    if (change_allowed &&
        actionable_pressure &&
        state->pressure_score >= GRD_FPS_PRESSURE_SCORE_LIMIT) {
        uint32_t sustainable = current;
        if (state->pipeline_ewma_x256 != 0U) {
            const uint64_t average_micros =
                (state->pipeline_ewma_x256 + 255U) >> 8U;
            if (average_micros != 0U) {
                sustainable = (uint32_t)(
                    (1000000ULL * GRD_FPS_PIPELINE_HEADROOM_PERCENT) /
                    (average_micros * 100ULL)
                );
            }
        }
        if (sustainable > target_fps) {
            sustainable = target_fps;
        }
        const uint32_t floor = fps_pressure_floor(target_fps);
        state->effective_fps = fps_step_down(
            current,
            sustainable,
            floor,
            severe ? GRD_FPS_SEVERE_DOWN_STEP : GRD_FPS_DOWN_STEP
        );
        state->last_change_pressure_score = state->pressure_score;
        state->last_change_reasons =
            (sustained_pipeline ? GRD_FPS_CHANGE_REASON_PIPELINE : 0U) |
            (fresh_drop_sample ? GRD_FPS_CHANGE_REASON_DROP : 0U) |
            (repeated_capture_backlog ? GRD_FPS_CHANGE_REASON_BACKLOG : 0U);
        state->pressure_score = 0U;
        state->last_change_micros = now_micros;
        state->last_score_decay_micros = now_micros;
        state->clean_since_micros = 0U;
        state->pipeline_overload_since_micros = pipeline_overloaded
                                                    ? now_micros
                                                    : 0U;
        return state->effective_fps;
    }

    if (state->effective_fps < target_fps &&
        state->clean_since_micros != 0U &&
        now_micros >= state->clean_since_micros &&
        now_micros - state->clean_since_micros >=
            GRD_FPS_RESTORE_CLEAN_US &&
        now_micros >= state->last_change_micros &&
        now_micros - state->last_change_micros >=
            GRD_FPS_RESTORE_STEP_INTERVAL_US) {
        uint32_t restored = state->effective_fps + GRD_FPS_UP_STEP;
        if (restored > target_fps) {
            restored = target_fps;
        }
        state->effective_fps = restored;
        state->last_change_pressure_score = state->pressure_score;
        state->last_change_reasons = GRD_FPS_CHANGE_REASON_RESTORE;
        state->last_change_micros = now_micros;
        state->pressure_score = 0U;
    }
    return state->effective_fps;
}

uint32_t grd_stream_client_offload_level(int upscale_mode)
{
    if (upscale_mode <= (int)GRD_CLIENT_UPSCALE_NATIVE) {
        return 0U;
    }
    if (upscale_mode == (int)GRD_CLIENT_UPSCALE_BALANCED) {
        return 1U;
    }
    return 2U;
}

void grd_stream_ladder_max_dimensions(
    uint32_t requested_width,
    uint32_t requested_height,
    uint32_t level,
    uint32_t *width,
    uint32_t *height
)
{
    if (width == NULL || height == NULL) {
        return;
    }
    if (requested_width < 640U || requested_height < 360U) {
        requested_width = 1920U;
        requested_height = 1080U;
    }
    if (level > 2U) {
        level = 2U;
    }
    if (level == 0U) {
        *width = requested_width;
        *height = requested_height;
    } else if (requested_height > 1440U) {
        *width = level == 1U ? 2560U : 1920U;
        *height = level == 1U ? 1440U : 1080U;
    } else if (requested_height > 1080U) {
        *width = level == 1U ? 1920U : 1280U;
        *height = level == 1U ? 1080U : 720U;
    } else {
        *width = level == 1U ? 1600U : 1280U;
        *height = level == 1U ? 900U : 720U;
    }
    if (*width > requested_width) {
        *width = requested_width;
    }
    if (*height > requested_height) {
        *height = requested_height;
    }
}

bool grd_stream_encoder_configuration_changed(
    uint32_t active_width,
    uint32_t active_height,
    uint32_t active_fps,
    int active_codec,
    uint32_t desired_width,
    uint32_t desired_height,
    uint32_t desired_fps,
    int desired_codec
)
{
    return active_width != desired_width || active_height != desired_height ||
           active_fps != desired_fps || active_codec != desired_codec;
}
