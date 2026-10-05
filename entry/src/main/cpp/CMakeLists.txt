cmake_minimum_required(VERSION 3.5.0)
project(OpenSportCamera)

set(NATIVERENDER_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include_directories(${NATIVERENDER_ROOT_PATH})

add_library(burn SHARED
    napi_init.cpp
    mini_json.cpp
    overlay_layout.cpp
    blitter.cpp
    codec_common.cpp
    burn_engine.cpp
    record_stream.cpp
    rtsp_server.cpp
    onvif_server.cpp
)

find_library(
    # Sets the name of the path variable.
    log-lib
    # Specifies the name of the library that the script locates.
    hilog_ndk.z.so
)

# OHOS 工具链的 sysroot 不在 find_library 默认搜索路径内，
# 直接以 soname 链接（链接期由 -L sysroot 解析）。
# API 24 起 native_media_avcodec 拆分为 vdec/venc/codecbase。
target_link_libraries(burn PUBLIC
    libace_napi.z.so
    libhilog_ndk.z.so
    libnative_media_vdec.so
    libnative_media_venc.so
    libnative_media_codecbase.so
    libnative_media_core.so
    libnative_media_avsource.so
    libnative_media_avdemuxer.so
    libnative_media_avmuxer.so
    libEGL.so
    libGLESv3.so
    libnative_image.so
    libnative_window.so
    libace_ndk.z.so
)

target_include_directories(burn PUBLIC ${NATIVERENDER_ROOT_PATH})
