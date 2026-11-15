# Video content pinch policy checks

`python tests/video_pinch/run_tests.py` extracts the current production policy,
gesture handler, chrome handler and reset bodies and runs them with deterministic
host LVGL event/object stubs using the existing portable TCC. It covers content
and global edge boundaries, overlay/browser/background/drag exclusions, repeated
recognition, fit/fill thresholds, pause, cancel/release/press-lost, lifecycle reset,
and the native ended → released → clicked sequence, including original target
preservation when a click bubbles to the screen.

`python tests/video_pinch/check_xtensa.py` copies the current source and header to
an isolated ASCII directory and uses the existing real Xtensa compile flags with
only the two candidate gesture-related SDK options enabled in a temporary config
header. It does not stage a project, link, flash or change the active SDK config.

The gesture changes only the existing **fit/fill mode** once per pinch; it is not
continuous arbitrary image scaling. Paused video updates after playback resumes.
These checks do not prove physical multi-touch recognition, actual rendering or
the preservation of Brookesia's independently sampled system gestures. Those
remain device acceptance tests.
