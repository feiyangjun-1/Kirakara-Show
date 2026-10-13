# FFmpeg is always imported as DLLs; never link static GPL/nonfree libraries.
set(KIRAKARA_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.deps/ffmpeg-sdk"
    CACHE PATH "LGPL FFmpeg shared SDK (include/lib/bin)")
if(NOT EXISTS "${KIRAKARA_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "FFmpeg SDK missing. Run tools/prepare_ffmpeg.ps1 or set KIRAKARA_FFMPEG_ROOT to an LGPL shared SDK.")
endif()
if(NOT EXISTS "${KIRAKARA_FFMPEG_ROOT}/bin/ffmpeg.exe")
    message(FATAL_ERROR "FFmpeg SDK must include ffmpeg.exe for configuration/license validation.")
endif()
execute_process(COMMAND "${KIRAKARA_FFMPEG_ROOT}/bin/ffmpeg.exe" -version
    OUTPUT_VARIABLE _ffmpeg_version ERROR_VARIABLE _ffmpeg_error RESULT_VARIABLE _ffmpeg_status)
if(NOT _ffmpeg_status EQUAL 0 OR _ffmpeg_version MATCHES "--enable-(gpl|nonfree)( |\n)")
    message(FATAL_ERROR "FFmpeg must be a working LGPL shared build without GPL/nonfree components: ${_ffmpeg_error}")
endif()
if(NOT _ffmpeg_version MATCHES "--enable-shared")
    message(FATAL_ERROR "FFmpeg must be built with --enable-shared.")
endif()
if(NOT _ffmpeg_version MATCHES "--enable-libopenh264")
    message(FATAL_ERROR "FFmpeg must include LGPL-compatible libopenh264 for software H.264 fallback.")
endif()
foreach(_component avcodec avformat avutil swscale swresample)
    file(GLOB _dll "${KIRAKARA_FFMPEG_ROOT}/bin/${_component}-*.dll")
    list(LENGTH _dll _dll_count)
    if(NOT _dll_count EQUAL 1)
        message(FATAL_ERROR "Expected one FFmpeg ${_component} DLL, found ${_dll_count}.")
    endif()
    if(MSVC)
        set(_implib "${KIRAKARA_FFMPEG_ROOT}/lib/${_component}.lib")
    else()
        set(_implib "${KIRAKARA_FFMPEG_ROOT}/lib/lib${_component}.dll.a")
    endif()
    if(NOT EXISTS "${_implib}")
        message(FATAL_ERROR "FFmpeg import library missing: ${_implib}")
    endif()
    add_library(FFmpeg::${_component} SHARED IMPORTED GLOBAL)
    set_target_properties(FFmpeg::${_component} PROPERTIES
        IMPORTED_LOCATION "${_dll}" IMPORTED_IMPLIB "${_implib}"
        INTERFACE_INCLUDE_DIRECTORIES "${KIRAKARA_FFMPEG_ROOT}/include")
    list(APPEND KIRAKARA_FFMPEG_DLLS "${_dll}")
endforeach()

add_library(kirakara_ffmpeg_decode STATIC apps/show_host/media/ffmpeg_decode.cpp
    apps/show_host/cast/ffmpeg_h264_session.cpp)
target_compile_features(kirakara_ffmpeg_decode PUBLIC cxx_std_20)
target_compile_definitions(kirakara_ffmpeg_decode PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(kirakara_ffmpeg_decode PUBLIC FFmpeg::avcodec FFmpeg::avformat
    FFmpeg::avutil FFmpeg::swscale FFmpeg::swresample d3d11 dxgi)

get_property(_ffmpeg_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(_ffmpeg_runtime_root "${CMAKE_BINARY_DIR}")
set(_ffmpeg_runtime_bin "${CMAKE_BINARY_DIR}/bin")
if(_ffmpeg_multi)
    string(APPEND _ffmpeg_runtime_root "/$<CONFIG>")
    string(APPEND _ffmpeg_runtime_bin "/$<CONFIG>")
endif()
# Deploy once before linking consumers. Parallel post-build copies from many
# tests into the same notices directory can race on Windows.
add_custom_target(kirakara_ffmpeg_runtime
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_ffmpeg_runtime_root}" "${_ffmpeg_runtime_bin}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KIRAKARA_FFMPEG_DLLS} "${_ffmpeg_runtime_root}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KIRAKARA_FFMPEG_DLLS} "${_ffmpeg_runtime_bin}"
    VERBATIM)
if(EXISTS "${KIRAKARA_FFMPEG_ROOT}/third_party/ffmpeg")
    add_custom_command(TARGET kirakara_ffmpeg_runtime POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${KIRAKARA_FFMPEG_ROOT}/third_party/ffmpeg"
            "${_ffmpeg_runtime_root}/third_party/ffmpeg"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${KIRAKARA_FFMPEG_ROOT}/third_party/ffmpeg"
            "${_ffmpeg_runtime_bin}/third_party/ffmpeg" VERBATIM)
endif()

function(kirakara_link_ffmpeg target)
    get_property(_attached TARGET ${target} PROPERTY KIRAKARA_FFMPEG_ATTACHED)
    if(_attached)
        return()
    endif()
    set_property(TARGET ${target} PROPERTY KIRAKARA_FFMPEG_ATTACHED TRUE)
    target_link_libraries(${target} PRIVATE kirakara_ffmpeg_decode)
    add_dependencies(${target} kirakara_ffmpeg_runtime)
endfunction()
