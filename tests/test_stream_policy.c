#include "test.h"

#include "core/stream_policy.h"

void test_stream_policy(void)
{
    grd_capture_recovery_state capture;
    grd_capture_recovery_reset(&capture);

    GRD_ASSERT(
        grd_capture_recovery_on_frame(
            &capture, 1000000U, 1150000U, 8333U, true
        ) == GRD_CAPTURE_EVENT_PLANNED_GAP
    );
    GRD_ASSERT(capture.phase == GRD_CAPTURE_PHASE_STABLE);
    GRD_ASSERT(capture.episode_count == 0U);
    GRD_ASSERT(capture.planned_gap_count == 1U);

    GRD_ASSERT(
        grd_capture_recovery_on_frame(
            &capture, 0U, 1160000U, 8333U, true
        ) == GRD_CAPTURE_EVENT_PLANNED_GAP
    );
    GRD_ASSERT(capture.planned_gap_count == 1U);

    GRD_ASSERT(
        grd_capture_recovery_on_frame(
            &capture, 2000000U, 2150000U, 8333U, false
        ) == GRD_CAPTURE_EVENT_REQUEST
    );
    grd_capture_recovery_mark_started(&capture);
    GRD_ASSERT(
        grd_capture_recovery_on_frame(
            &capture, 2150000U, 2300000U, 8333U, false
        ) == GRD_CAPTURE_EVENT_NONE
    );
    GRD_ASSERT(capture.raw_gap_count == 2U);
    GRD_ASSERT(capture.episode_count == 1U);

    uint64_t frame_at = 2300000U;
    bool rearmed = false;
    for (unsigned index = 0U; index < 140U; ++index) {
        const uint64_t previous = frame_at;
        frame_at += 8333U;
        if (grd_capture_recovery_on_frame(
                &capture, previous, frame_at, 8333U, false
            ) == GRD_CAPTURE_EVENT_REARMED) {
            rearmed = true;
        }
    }
    GRD_ASSERT(rearmed);
    GRD_ASSERT(capture.phase == GRD_CAPTURE_PHASE_STABLE);
    GRD_ASSERT(
        grd_capture_recovery_on_frame(
            &capture, frame_at, frame_at + 120000U, 8333U, false
        ) == GRD_CAPTURE_EVENT_REQUEST
    );
    GRD_ASSERT(capture.episode_count == 2U);

    grd_capture_recovery_mark_started(&capture);
    GRD_ASSERT(grd_capture_recovery_force(&capture));
    GRD_ASSERT(capture.phase == GRD_CAPTURE_PHASE_REQUESTED);
    GRD_ASSERT(capture.episode_count == 3U);

    GRD_ASSERT(!grd_capture_gap_is_discontinuity(
        1000000U, 1150000U, 1U, false
    ));
    GRD_ASSERT(grd_capture_gap_is_discontinuity(
        1000000U, 1150000U, 2U, false
    ));
    GRD_ASSERT(grd_capture_gap_is_discontinuity(
        1000000U, 1150000U, 1U, true
    ));
    GRD_ASSERT(!grd_capture_gap_is_discontinuity(
        1000000U, 1020000U, 4U, true
    ));

    grd_capture_watchdog_state watchdog;
    grd_capture_watchdog_reset(&watchdog);
    /* A static desktop is not a failure, even after several seconds. */
    GRD_ASSERT(
        grd_capture_watchdog_on_timeout(
            &watchdog, 1000000U, 5000000U, false
        ) == GRD_CAPTURE_WATCHDOG_NONE
    );
    /* A measured driver overrun turns a one-second silence into a soft
     * session reset, never an immediate D3D device rebuild. */
    GRD_ASSERT(
        grd_capture_watchdog_on_timeout(
            &watchdog, 5000000U, 6000000U, true
        ) == GRD_CAPTURE_WATCHDOG_RESET_SESSION
    );
    GRD_ASSERT(
        watchdog.stage == GRD_CAPTURE_WATCHDOG_SESSION_RESET
    );
    GRD_ASSERT(
        grd_capture_watchdog_on_timeout(
            &watchdog, 5000000U, 7000000U, false
        ) == GRD_CAPTURE_WATCHDOG_NONE
    );
    GRD_ASSERT(
        grd_capture_watchdog_on_timeout(
            &watchdog, 5000000U, 7500000U, true
        ) == GRD_CAPTURE_WATCHDOG_RESET_DEVICE
    );
    GRD_ASSERT(
        grd_capture_watchdog_on_timeout(
            &watchdog, 5000000U, 10000000U, true
        ) == GRD_CAPTURE_WATCHDOG_NONE
    );
    GRD_ASSERT(
        grd_capture_watchdog_on_timeout(
            &watchdog, 5000000U, 12500000U, true
        ) == GRD_CAPTURE_WATCHDOG_RESET_DEVICE
    );
    grd_capture_watchdog_rearm(&watchdog);
    GRD_ASSERT(watchdog.stage == GRD_CAPTURE_WATCHDOG_STABLE);
    GRD_ASSERT(watchdog.last_session_reset_micros == 0U);

    /* A real frame completes the soft recovery. A static image afterwards
     * must stay untouched, but a fresh-input relapse inside five seconds
     * proves the session-only reset was insufficient and escalates. */
    GRD_ASSERT(
        grd_capture_watchdog_on_timeout(
            &watchdog, 15000000U, 16000000U, true
        ) == GRD_CAPTURE_WATCHDOG_RESET_SESSION
    );
    grd_capture_watchdog_on_frame(&watchdog, 16100000U);
    GRD_ASSERT(watchdog.stage == GRD_CAPTURE_WATCHDOG_STABLE);
    GRD_ASSERT(
        grd_capture_watchdog_on_timeout(
            &watchdog, 16100000U, 18000000U, false
        ) == GRD_CAPTURE_WATCHDOG_NONE
    );
    GRD_ASSERT(
        grd_capture_watchdog_on_timeout(
            &watchdog, 16100000U, 18100000U, true
        ) == GRD_CAPTURE_WATCHDOG_RESET_DEVICE
    );

    grd_capture_watchdog_mark_device_reset(&watchdog, 14000000U);
    GRD_ASSERT(watchdog.stage == GRD_CAPTURE_WATCHDOG_DEVICE_RESET);
    GRD_ASSERT(watchdog.last_action_micros == 14000000U);

    const grd_stream_drop_counts descendants_only = {
        .sent = 120U,
        .discontinuity = 44U,
        .recovery_purge = 412U
    };
    GRD_ASSERT(
        grd_stream_initiating_drop_percent(&descendants_only) == 0U
    );
    const grd_stream_drop_counts initiating = {
        .sent = 98U,
        .admission = 1U,
        .deadline = 1U,
        .discontinuity = 100U,
        .recovery_purge = 500U
    };
    GRD_ASSERT(grd_stream_initiating_drop_percent(&initiating) == 2U);

    GRD_ASSERT(grd_stream_pacer_schedule_start(0U, 50000U, false) == 50000U);
    GRD_ASSERT(
        grd_stream_pacer_schedule_start(60000U, 50000U, false) == 60000U
    );
    GRD_ASSERT(
        grd_stream_pacer_schedule_start(45000U, 50000U, false) == 50000U
    );
    GRD_ASSERT(
        grd_stream_pacer_schedule_start(45000U, 50000U, true) == 45000U
    );
    GRD_ASSERT(
        grd_stream_pacer_schedule_start(10000U, 50000U, true) ==
        50000U - GRD_PACER_KEYFRAME_CREDIT_US
    );

    GRD_ASSERT(!grd_stream_keyframe_preempts_queue(
        false, false, false
    ));
    GRD_ASSERT(!grd_stream_keyframe_preempts_queue(
        false, true, false
    ));
    GRD_ASSERT(!grd_stream_keyframe_preempts_queue(
        true, false, false
    ));
    GRD_ASSERT(grd_stream_keyframe_preempts_queue(
        true, true, false
    ));
    GRD_ASSERT(!grd_stream_keyframe_preempts_queue(
        true, true, true
    ));

    GRD_ASSERT(grd_stream_drop_requests_recovery(
        false, false, false
    ));
    GRD_ASSERT(!grd_stream_drop_requests_recovery(
        false, true, false
    ));
    GRD_ASSERT(!grd_stream_drop_requests_recovery(
        false, true, true
    ));
    GRD_ASSERT(!grd_stream_drop_requests_recovery(
        true, true, false
    ));
    GRD_ASSERT(grd_stream_drop_requests_recovery(
        true, true, true
    ));

    GRD_ASSERT(!grd_stream_abr_cut_allowed(
        900U, 1000U, true, true
    ));
    GRD_ASSERT(grd_stream_abr_cut_allowed(
        1000U, 1000U, true, true
    ));
    GRD_ASSERT(!grd_stream_abr_cut_allowed(
        1000U, 1000U, false, true
    ));
    GRD_ASSERT(grd_stream_abr_cut_allowed(
        1000U, 1000U, false, false
    ));

    GRD_ASSERT(!grd_stream_rate_change_due(
        20000U, 20200U, 20000000U, 0U, false, false
    ));
    GRD_ASSERT(!grd_stream_rate_change_due(
        20000U, 22000U, 14999999U, 0U, false, false
    ));
    GRD_ASSERT(grd_stream_rate_change_due(
        20000U, 22000U, 15000000U, 0U, false, false
    ));
    GRD_ASSERT(grd_stream_rate_change_due(
        20000U, 18000U, 1000000U, 900000U, false, true
    ));
    GRD_ASSERT(!grd_stream_rate_change_due(
        20000U, 18000U, 1000000U, 0U, false, false
    ));
    GRD_ASSERT(grd_stream_rate_change_due(
        20000U, 20000U, 1000U, 1000U, true, false
    ));

    /* A 30 fps source arriving every 33 ms is not late merely because the
     * session ceiling is 120 fps. Jitter beyond the source cadence is. */
    GRD_ASSERT(!grd_stream_arrival_gap_is_late(
        33334U, 33333U, 120U
    ));
    GRD_ASSERT(grd_stream_arrival_gap_is_late(
        60000U, 33333U, 120U
    ));
    GRD_ASSERT(!grd_stream_arrival_gap_is_late(
        70000U, 70000U, 120U
    ));
    GRD_ASSERT(!grd_stream_arrival_gap_is_late(
        25000U, 0U, 120U
    ));
    GRD_ASSERT(grd_stream_arrival_gap_is_late(
        25001U, 0U, 120U
    ));
    GRD_ASSERT(!grd_stream_arrival_gap_is_late(
        33334U, 0U, 30U
    ));

    grd_stream_fps_pressure_state fps_pressure;
    grd_stream_fps_pressure_reset(&fps_pressure, 120U, 1000000U);
    uint64_t pressure_at = 1000000U;
    for (unsigned frame = 0U; frame < 80U; ++frame) {
        pressure_at += 8333U;
        (void)grd_stream_fps_pressure_update(
            &fps_pressure, 120U, pressure_at, 10000U, 0U, 0U, 0U
        );
        if (frame == 19U) {
            /* A short scene-load burst is over before adaptation could help;
             * keep 120 instead of reacting to an already-finished spike. */
            GRD_ASSERT(fps_pressure.effective_fps == 120U);
        }
    }
    GRD_ASSERT(fps_pressure.effective_fps == 116U);
    GRD_ASSERT(
        (fps_pressure.last_change_reasons &
         GRD_FPS_CHANGE_REASON_PIPELINE) != 0U
    );
    GRD_ASSERT(fps_pressure.last_change_pressure_score >= 12U);

    const uint32_t reduced_fps = fps_pressure.effective_fps;
    for (unsigned sample = 0U; sample < 80U; ++sample) {
        pressure_at += 100000U;
        (void)grd_stream_fps_pressure_update(
            &fps_pressure, 120U, pressure_at, 4000U, 0U, 0U, 0U
        );
    }
    GRD_ASSERT(fps_pressure.effective_fps > reduced_fps);
    GRD_ASSERT(fps_pressure.effective_fps <= 120U);

    /* The host exposes a one-second rolling drop percentage. Polling that
     * same atomic value on every frame must not count it 120 times, while a
     * second persistently lossy window still causes one small correction. */
    grd_stream_fps_pressure_reset(&fps_pressure, 120U, 0U);
    GRD_ASSERT(
        grd_stream_fps_pressure_update(
            &fps_pressure, 120U, 600000U, 4000U, 10U, 2U, 0U
        ) == 120U
    );
    for (uint64_t at = 608333U; at < 1600000U; at += 8333U) {
        GRD_ASSERT(
            grd_stream_fps_pressure_update(
                &fps_pressure, 120U, at, 4000U, 10U, 2U, 0U
            ) == 120U
        );
    }
    GRD_ASSERT(
        grd_stream_fps_pressure_update(
            &fps_pressure, 120U, 1600000U, 4000U, 10U, 4U, 0U
        ) == 116U
    );
    GRD_ASSERT(
        (fps_pressure.last_change_reasons &
         GRD_FPS_CHANGE_REASON_DROP) != 0U
    );

    grd_stream_fps_pressure_reset(&fps_pressure, 120U, 0U);
    GRD_ASSERT(
        grd_stream_fps_pressure_update(
            &fps_pressure, 120U, 600000U, 4000U, 0U, 0U, 1U
        ) == 120U
    );
    GRD_ASSERT(
        grd_stream_fps_pressure_update(
            &fps_pressure, 120U, 700000U, 4000U, 0U, 0U, 4U
        ) == 120U
    );
    GRD_ASSERT(
        grd_stream_fps_pressure_update(
            &fps_pressure, 120U, 800000U, 4000U, 0U, 0U, 4U
        ) == 116U
    );
    GRD_ASSERT(
        (fps_pressure.last_change_reasons &
         GRD_FPS_CHANGE_REASON_BACKLOG) != 0U
    );
    GRD_ASSERT(
        grd_stream_fps_pressure_update(
            &fps_pressure, 60U, 900000U, 4000U, 0U, 0U, 0U
        ) == 60U
    );

    GRD_ASSERT(grd_stream_client_offload_level(-1) == 0U);
    GRD_ASSERT(
        grd_stream_client_offload_level(GRD_CLIENT_UPSCALE_NATIVE) == 0U
    );
    GRD_ASSERT(
        grd_stream_client_offload_level(GRD_CLIENT_UPSCALE_BALANCED) == 1U
    );
    GRD_ASSERT(
        grd_stream_client_offload_level(GRD_CLIENT_UPSCALE_PERFORMANCE) == 2U
    );
    GRD_ASSERT(grd_stream_client_offload_level(99) == 2U);
    uint32_t ladder_width = 0U;
    uint32_t ladder_height = 0U;
    grd_stream_ladder_max_dimensions(
        1920U, 1080U, 0U, &ladder_width, &ladder_height
    );
    GRD_ASSERT(ladder_width == 1920U && ladder_height == 1080U);
    grd_stream_ladder_max_dimensions(
        1920U, 1080U, 1U, &ladder_width, &ladder_height
    );
    GRD_ASSERT(ladder_width == 1600U && ladder_height == 900U);
    grd_stream_ladder_max_dimensions(
        1920U, 1080U, 2U, &ladder_width, &ladder_height
    );
    GRD_ASSERT(ladder_width == 1280U && ladder_height == 720U);
    grd_stream_ladder_max_dimensions(
        2560U, 1440U, 1U, &ladder_width, &ladder_height
    );
    GRD_ASSERT(ladder_width == 1920U && ladder_height == 1080U);
    grd_stream_ladder_max_dimensions(
        3840U, 2160U, 1U, &ladder_width, &ladder_height
    );
    GRD_ASSERT(ladder_width == 2560U && ladder_height == 1440U);
    grd_stream_ladder_max_dimensions(
        3840U, 2160U, 2U, &ladder_width, &ladder_height
    );
    GRD_ASSERT(ladder_width == 1920U && ladder_height == 1080U);
    GRD_ASSERT(!grd_stream_encoder_configuration_changed(
        1920U, 1080U, 120U, 1, 1920U, 1080U, 120U, 1
    ));
    GRD_ASSERT(grd_stream_encoder_configuration_changed(
        1920U, 1080U, 120U, 1, 1600U, 900U, 120U, 1
    ));
    GRD_ASSERT(grd_stream_encoder_configuration_changed(
        1920U, 1080U, 120U, 1, 1920U, 1080U, 60U, 1
    ));
    GRD_ASSERT(grd_stream_encoder_configuration_changed(
        1920U, 1080U, 120U, 1, 1920U, 1080U, 120U, 2
    ));
}
