# Find an explicitly supplied local FFmpeg SDK. This module never downloads files.
find_path(FFmpeg_INCLUDE_DIR libavformat/avformat.h HINTS "${RIFELOVE_FFMPEG_ROOT}/include")
foreach(component avformat avcodec avutil swscale)
  find_library(FFmpeg_${component}_LIBRARY NAMES ${component} HINTS "${RIFELOVE_FFMPEG_ROOT}/lib")
endforeach()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg REQUIRED_VARS FFmpeg_INCLUDE_DIR FFmpeg_avformat_LIBRARY FFmpeg_avcodec_LIBRARY FFmpeg_avutil_LIBRARY FFmpeg_swscale_LIBRARY)
if(FFmpeg_FOUND)
  foreach(component avformat avcodec avutil swscale)
    if(NOT TARGET FFmpeg::${component})
      add_library(FFmpeg::${component} UNKNOWN IMPORTED)
      set_target_properties(FFmpeg::${component} PROPERTIES
        IMPORTED_LOCATION "${FFmpeg_${component}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_INCLUDE_DIR}")
    endif()
  endforeach()
endif()
