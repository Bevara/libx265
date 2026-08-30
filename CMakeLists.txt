cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBX265_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/enc_x265.c
)

SET(LIBX265_LIB
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libx265.a
)

SET(LIBX265_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libx265
        "${LIBX265_SRC}"
        "${LIBX265_LIB}"
        []
        ""
        "${LIBX265_INC}"
        ""
        "1")
