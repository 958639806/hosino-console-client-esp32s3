# Helix MP3 dependency: license boundary

The locked dependency is `chmorgan/esp-libhelix-mp3` **1.0.3**, registry component
hash `cbb76089dc2c5749f7b470e2e70aedc44c9da519e04eb9a67d4c7ec275229e53`.

- [Registry entry](https://components.espressif.com/components/chmorgan/esp-libhelix-mp3/versions/1.0.3)
- [ESP-IDF wrapper source](https://github.com/chmorgan/esp-libhelix-mp3)
- [Nested decoder source](https://github.com/chmorgan/libhelix-mp3)

The wrapper's `LICENSE` is Apache-2.0. Its nested `libhelix-mp3/LICENSE.txt`
states that the decoder is under the RPSL unless an applicable RCSL has been
obtained. The wrapper license does not replace the decoder license. The three
decoder texts and the wrapper license are copied verbatim from the pinned
registry package, with SHA-256 in `copy-index.json`. Including `RCSL.txt` does
not claim that this project has an RCSL grant.

The observed firmware configuration enables `CONFIG_AUDIO_PLAYER_ENABLE_MP3`;
its linker map includes `mp3dec.c.obj` referenced by `audio_player.cpp.obj`
through `MP3InitDecoder`. Thus this is a linked dependency of that firmware,
not merely an unused package download.

RPSL section2.1(e) contains source-availability and object-code notice
requirements for executable distribution. Sections4.1/4.2 address combined
works and compatible source licenses. This source-only publication does not
declare that a combined firmware binary containing all separately licensed
dependencies has passed that compatibility review. No prebuilt firmware or
decoder library is distributed in this publication. Any later binary release
must resolve the applicable combination and object-code notice requirements;
copying these texts alone is not described as sufficient binary clearance.

The component manager retrieves this dependency at build time. Its decoder
implementation is not copied into the repository by this license snapshot.
