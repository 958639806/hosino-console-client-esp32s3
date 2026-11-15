# Project-local FatFs override

Source: ESP-IDF **v5.5.5**, commit
`b774170ff46c393eeb5e495ea37936038d3f4f4f`,
[components/fatfs](https://github.com/espressif/esp-idf/tree/b774170ff46c393eeb5e495ea37936038d3f4f4f/components/fatfs).
The embedded FatFs is R0.15 with patch 2. This copy overrides the SDK component
only for the Apps project; the installed ESP-IDF is unchanged.

## Changes

- `src/ffconf.h`: enable `FF_FS_EXFAT=1`, retaining FAT12/16/32 support.
- Give `FF_USE_LABEL` an explicit zero fallback when its Kconfig boolean is
  disabled. The exFAT directory reader uses this macro in a C expression;
  ESP-IDF otherwise leaves the disabled `CONFIG_FATFS_USE_LABEL` undefined.
- Keep `FF_LBA64=0`; no GPT or 64-bit block-addressing extension is enabled.
- Preserve upstream Kconfig, CMake, VFS and public interfaces. Existing LFN,
  UTF-8/code-page, locking, allocation and cache settings remain controlled by
  the project sdkconfig. Current Apps configuration uses heap LFN buffers,
  maximum 255 characters, UTF-8 names and code page 936.

Only `src/ffconf.h` differs from retained upstream implementation files. The
component includes all build inputs and Python image tools; upstream test apps,
host tests, generator tests, examples and caches are omitted. The original
`project_include.cmake` retains its SDK tool paths; those tools generate FAT
images, not exFAT images. No automatic card formatting or SD-clock change is
introduced here.

## Integration and limits

Enabling exFAT changes `FSIZE_t` to 64 bits and changes FatFs structure layouts,
including `FIL`, `FATFS` and `FILINFO`. Rebuild **all** FatFs consumers against
this component; do not mix precompiled consumers or headers from the default
32-bit configuration. `LBA_t` remains 32 bits.

The ESP32-S3 toolchain's POSIX `off_t` and `long` remain 32 bits. Existing VFS
seek/stat/pread/pwrite paths therefore do not provide complete large-file
semantics, and the link-copy path also uses 32-bit `size_t`. This change does
**not** promise complete POSIX or media-app support for single files over
2 GiB. exFAT volume support is distinct from large-file API support. Use
ordinary-size media files until those paths are separately designed and tested.

With a 255-character LFN limit, exFAT needs 608 additional bytes for its
directory working buffer, alongside the existing 512-byte LFN buffer. Other
allocation and per-open-file costs also change with the structure layouts.

Static checks use the fixed ESP32-S3 compiler and current Apps configuration.
Real-card mounting, read/write behavior, format recognition and sustained
40 MHz operation require separate hardware validation; none is asserted by
this source import.

## Licenses

Original file notices are retained. `LICENSE-IDF` is the SDK's Apache-2.0
license; `LICENSE-FATFS` reproduces the ChaN FatFs copyright, redistribution
condition and disclaimer from `src/ff.c`. `sbom.yml` retains upstream metadata.
