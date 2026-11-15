idf_component_register(SRCS "hosino_media_player.c" "media_profile.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_player esp_audio_render esp_video_render esp_extractor esp_audio_codec esp_video_codec
                  gmf_core gmf_audio gmf_video
                  bsp_extra storage_service esp_timer heap freertos hosino_jpegdec gmf_io)

# Measure the built-in decoder without modifying vendor codec/GMF sources.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_video_dec_process"
    "-Wl,--undefined=__wrap_esp_video_dec_process"
    "-Wl,--undefined=hosino_player_diagnostics_record"
    "-Wl,--undefined=hosino_player_clock_record"
    "-Wl,--undefined=hosino_file_diagnostics_bind"
    "-Wl,--undefined=hosino_file_diagnostics_record")
