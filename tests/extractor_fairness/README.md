# Demux audio fairness regression

Run `python tests/extractor_fairness/run_tests.py` from the workspace. It uses the existing portable TCC compiler and the frozen v20 staged-source ZIP; neither tools nor dependencies are downloaded.

The host harness executes the production extractor process and the production policy/ownership helpers. Its fake extractor, frame queues and clock make progress and ownership observable without accessing a card, network or device.

- Frozen v20: an empty compressed-audio queue and two queued video packets cause repeated sleeps before any next read, leaving a following audio packet unreachable until video consumption changes the queue.
- Candidate: local MJPEG A/V discards surplus non-EOS video packets before enqueue, releases each producer reference exactly once, and continues to a following audio packet. Queued packets retain ownership until consumer cleanup. A failed release returns a fatal job error and is not automatically retried.
- EOS, audio, seek, stop, network/unknown schemes, inter-frame codecs, pause/preparation and a closed buffering gate are excluded from the discard policy.
- A bounded-pool scenario demonstrates why read-before-drop still needs enough room to obtain the next candidate packet; a 100KB virtual pool can remain full while 256KiB headroom allows progress. This is not an allocator or AVI parser test.

An empty compressed-audio queue does not establish that decoded PCM or I²S is empty. This conservative policy deliberately favors audio over surplus independent video packets; it may discard useful video while some PCM remains buffered. Actual sound continuity, video smoothness and scheduling must be measured on the board.
