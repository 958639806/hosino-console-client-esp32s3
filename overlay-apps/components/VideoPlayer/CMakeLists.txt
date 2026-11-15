set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)
file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)

idf_component_register(
    SRCS ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES
        hosino_media_player
        brookesia_core
        bsp_extra
        storage_service
        hosino_ui_fonts
        hosino_chat_fonts
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES COMPILE_FLAGS "-Wno-missing-field-initializers"
)
