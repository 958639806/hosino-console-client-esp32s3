# Session-scoped media diagnostics host tests

Run `python tests/media_profile/run_tests.py`. The runner uses the existing
portable TinyCC from the FatFs test directory, or `--compiler PATH`; it never
downloads or installs a compiler. Files under `.build` are temporary outputs.

The test compiles **byte-identical copies of the real `media_profile.c` and
public headers**, then calls their public APIs. It checks exact pathname
binding, copied path lifetime, same-path reopen and stale-generation rejection,
read/seek isolation, EOF/errors, strict 20/40/80 ms threshold boundaries,
64-bit totals, interval take/clear, and disabled-session suppression. The real
decoder wrapper is exercised across reset/stop while its injected decoder call
is in flight, including error and elapsed-time clamps. Pipeline/audio counters
also run through the real implementation.

The v11a clock hook is checked for last-sample copying (the caller immediately
overwrites its input), sample count, full 64-bit PTS/anchor fields, take/clear,
NULL input and disabled-session behavior.

Only platform dependencies are shimmed: a deterministic clock and decoder,
`strlcpy`, and checked single-thread critical-section boundaries. This does not
test multicore scheduling, SD/FatFs latency, audible output, decoder correctness
or the complete player lifecycle. Source SHA-256 and actual execution results
are saved in `validation.json`.
