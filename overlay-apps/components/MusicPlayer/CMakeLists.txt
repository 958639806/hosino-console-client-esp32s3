idf_component_register(
    SRCS "MusicPlayer.cpp" "gui_music/lv_demo_music.cpp" "assets/img_app_musicplayer.c"
    INCLUDE_DIRS "."
    REQUIRES brookesia_core bsp_extra storage_service hosino_ui_fonts hosino_media_player
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
