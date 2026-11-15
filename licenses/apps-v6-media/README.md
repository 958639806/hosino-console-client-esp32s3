# Apps v6 media dependency licenses

This snapshot contains the original LICENSE and idf_component.yml of each of the
15 registry media dependencies locked by firmware/apps/80mhz/dependencies.lock.
Each PROVENANCE.json records the exact version, registry component content hash,
source repository/commit where the upstream manifest provides it, and hashes of
the copied license and manifest. All component contents were validated against
the firmware lock before copying. Original registry download receipts are
included where one was captured for the exact selected version. A missing ZIP
receipt does not imply an invented ZIP hash; the registry component hash is
recorded for every item.

License texts are copied verbatim and are not replaced by the application
license. The packages include differing license terms; consult each LICENSE.
Some codec/extractor implementations ship as precompiled libraries. This
snapshot does not claim that all implementation source is available.

The additional local GMF IO 1.1.0 compatibility override retains its own LICENSE
and SOURCE.md in overlay-apps/components/gmf_io_compat/gmf_io. Its original
registry receipt is separately included at
upstream/esp-player-registry-v1.1.1/gmf_io/REGISTRY_SOURCE.json.
