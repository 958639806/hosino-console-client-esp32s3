#include <assert.h>
#include <stdio.h>
#include "hosino_mjpeg_clock.h"

static void kept(hosino_mjpeg_clock_decision_t d)
{
    assert(!d.drop_audio && !d.drop_wall);
}

int main(void)
{
    hosino_mjpeg_clock_t c = {0};
    /* No PCM, and a genuine first PTS zero, must not invent a clock. */
    kept(hosino_mjpeg_clock_decide(&c, 9000, 10000000, 43));
    assert(hosino_mjpeg_clock_audio_commit(&c, c.generation, 0, 0) == HOSINO_MJPEG_ANCHOR_FRESH);
    kept(hosino_mjpeg_clock_decide(&c, 0, 0, 43));
    kept(hosino_mjpeg_clock_decide(&c, 43, 55000, 43));

    /* Audio itself advances too slowly. A last-audio-PTS-only policy would
     * keep this frame; the original monotonic anchor must detect its lateness. */
    assert(hosino_mjpeg_clock_audio_commit(&c, c.generation, 20, 100000) == 0);
    assert(hosino_mjpeg_clock_audio_commit(&c, c.generation, 40, 200000) == 0);
    hosino_mjpeg_clock_decision_t d = hosino_mjpeg_clock_decide(&c, 86, 250000, 43);
    assert(d.drop_wall && !d.drop_audio && d.late_ms == 164);

    /* Pause/seek/reset invalidates an in-flight acquired block. No output
     * from the old generation may anchor a resumed or newly sought timeline. */
    uint32_t old = c.generation;
    hosino_mjpeg_clock_reset(&c);
    assert(hosino_mjpeg_clock_audio_commit(&c, old, 60, 5000000) == 0);
    assert(!c.audio_valid);
    kept(hosino_mjpeg_clock_decide(&c, 12000, 5000000, 43));
    assert(hosino_mjpeg_clock_audio_commit(&c, c.generation, 12000, 5010000) == HOSINO_MJPEG_ANCHOR_FRESH);
    kept(hosino_mjpeg_clock_decide(&c, 12000, 5010000, 43));
    kept(hosino_mjpeg_clock_decide(&c, 12043, 5053000, 43));

    /* A real accepted-audio clock can independently outrun a stale video. */
    hosino_mjpeg_clock_reset(&c);
    hosino_mjpeg_clock_audio_commit(&c, c.generation, 1000, 0);
    kept(hosino_mjpeg_clock_decide(&c, 1000, 0, 33));
    hosino_mjpeg_clock_audio_commit(&c, c.generation, 1200, 50000);
    d = hosino_mjpeg_clock_decide(&c, 1033, 60000, 33);
    assert(d.drop_audio && !d.drop_wall && d.late_ms == 167);

    /* Threshold is strict and never below 60ms; a frame exactly on it stays. */
    hosino_mjpeg_clock_reset(&c);
    hosino_mjpeg_clock_audio_commit(&c, c.generation, 0, 0);
    kept(hosino_mjpeg_clock_decide(&c, 0, 0, 16));
    kept(hosino_mjpeg_clock_decide(&c, 16, 76000, 16));
    d = hosino_mjpeg_clock_decide(&c, 32, 93000, 16);
    assert(d.drop_wall && d.late_ms == 61);

    /* Stalls, large catch-up, timestamp regressions and missing/repeated
     * timestamps retain a frame rather than scanning an entire movie. */
    d = hosino_mjpeg_clock_decide(&c, 48, 1100000, 16);
    kept(d); assert(d.reanchor_reason == HOSINO_MJPEG_ANCHOR_STALL && !c.audio_valid);
    hosino_mjpeg_clock_audio_commit(&c, c.generation, 50, 1200000);
    kept(hosino_mjpeg_clock_decide(&c, 64, 1200000, 16));
    d = hosino_mjpeg_clock_decide(&c, 32, 1201000, 16);
    kept(d); assert(d.reanchor_reason == HOSINO_MJPEG_ANCHOR_BACKWARD && !c.audio_valid);
    hosino_mjpeg_clock_audio_commit(&c, c.generation, 0, 1300000);
    kept(hosino_mjpeg_clock_decide(&c, 0, 1300000, 16));
    d = hosino_mjpeg_clock_decide(&c, 0, 1400000, 16);
    kept(d); assert(d.reanchor_reason == HOSINO_MJPEG_ANCHOR_BACKWARD);
    hosino_mjpeg_clock_audio_commit(&c, c.generation, 0, 1500000);
    kept(hosino_mjpeg_clock_decide(&c, 0, 1500000, 16));
    hosino_mjpeg_clock_audio_commit(&c, c.generation, 20, 2400000);
    d = hosino_mjpeg_clock_decide(&c, 16, 2600000, 16);
    kept(d); assert(d.reanchor_reason == HOSINO_MJPEG_ANCHOR_TOO_LATE);
    hosino_mjpeg_clock_audio_commit(&c, c.generation, 100, 2700000);
    kept(hosino_mjpeg_clock_decide(&c, 100, 2700000, 33));
    d = hosino_mjpeg_clock_decide(&c, 5000, 2701000, 33);
    kept(d); assert(d.reanchor_reason == HOSINO_MJPEG_ANCHOR_FORWARD);

    /* A/V constraints are the exact predicates used by the production ports
     * and sync code: H264, network, audio-only, speed, paused/seek/EOS-stop. */
    assert(hosino_mjpeg_clock_source_eligible(true, true, true, true));
    assert(!hosino_mjpeg_clock_source_eligible(false, true, true, true));
    assert(!hosino_mjpeg_clock_source_eligible(true, false, true, true));
    assert(!hosino_mjpeg_clock_source_eligible(true, true, false, true));
    assert(!hosino_mjpeg_clock_source_eligible(true, true, true, false));
    assert(hosino_mjpeg_clock_can_run(true, 1.0f, false, false, false));
    assert(!hosino_mjpeg_clock_can_run(false, 1.0f, false, false, false));
    assert(!hosino_mjpeg_clock_can_run(true, 0.5f, false, false, false));
    assert(!hosino_mjpeg_clock_can_run(true, 2.0f, false, false, false));
    assert(!hosino_mjpeg_clock_can_run(true, 1.0f, true, false, false));
    assert(!hosino_mjpeg_clock_can_run(true, 1.0f, false, true, false));
    assert(!hosino_mjpeg_clock_can_run(true, 1.0f, false, false, true));
    puts("PASS: real MJPEG clock helper, lifecycle epochs, discontinuities, lag clocks and source guards");
    return 0;
}
