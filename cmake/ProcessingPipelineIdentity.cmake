# Reviewed checkpoint-affecting production sources. Keep this explicit: the
# coverage audit below rejects a new src/ file until its identity is reviewed.
# Native/QML UI, tests, models, packaging and local paths are not source inputs.
set(rifelove_processing_identity_sources
  CMakeLists.txt
  cmake/ProcessingPipelineIdentity.cmake
  cmake/processing_pipeline_identity.hpp.in
  cmake/nvenc_build_config.hpp.in
  src/application/checkpoint_artifacts.cpp
  src/application/checkpoint_artifacts.hpp
  src/application/import_service.cpp
  src/application/import_service.hpp
  src/application/job_serialization.cpp
  src/application/job_serialization.hpp
  src/application/local_lease.cpp
  src/application/local_lease.hpp
  src/application/pause_gate.hpp
  src/application/platform_bitrate.hpp
  src/application/processing_service.cpp
  src/application/processing_service.hpp
  src/application/queue_report.cpp
  src/application/queue_report.hpp
  src/application/task_queue.cpp
  src/application/task_queue.hpp
  src/common/json.hpp
  src/common/logging.cpp
  src/common/logging.hpp
  src/common/output_target.hpp
  src/common/rational.cpp
  src/common/rational.hpp
  src/core/pipeline/motion_blur.cpp
  src/core/pipeline/motion_blur.hpp
  src/core/pipeline/output_validation.hpp
  src/core/pipeline/video_pipeline.cpp
  src/core/pipeline/video_pipeline.hpp
  src/database/task_store.cpp
  src/database/task_store.hpp
  src/gpu/cuda_device.cpp
  src/gpu/cuda_device.hpp
  src/inference/interpolation_backend.hpp
  src/inference/model_pack.cpp
  src/inference/model_pack.hpp
  src/inference/onnx_cuda_backend.cpp
  src/inference/onnx_cuda_backend.hpp
  src/inference/ort_runtime_lease.hpp
  src/inference/tensorrt_cache.cpp
  src/inference/tensorrt_cache.hpp
  src/interpolation/model_adapter.cpp
  src/interpolation/model_adapter.hpp
  src/interpolation/scene_analysis.cpp
  src/interpolation/scene_analysis.hpp
  src/media/decode.cpp
  src/media/ffmpeg_helpers.cpp
  src/media/ffmpeg_helpers.hpp
  src/media/media_engine.hpp
  src/media/nvenc_diagnostic.cpp
  src/media/nvenc_diagnostic.hpp
  src/media/output_helpers.cpp
  src/media/output_helpers.hpp
  src/media/probe.cpp
  src/media/remux.cpp
  src/media/rife_color.cpp
  src/media/rife_color.hpp
  src/media/segment_assembler.cpp
  src/media/segment_verifier.cpp
  src/media/timestamp_recovery.hpp
  src/media/video_writer.cpp
  src/media/video_writer.hpp
  src/restoration/dynamic_control.cpp
  src/restoration/dynamic_control.hpp
  src/restoration/compression_estimate.cpp
  src/restoration/compression_estimate.hpp
  src/restoration/detail_blend.hpp
  src/restoration/model_pack.cpp
  src/restoration/model_pack.hpp
  src/restoration/noise_estimate.cpp
  src/restoration/noise_estimate.hpp
  src/restoration/onnx_cuda_backend.hpp
  src/restoration/stdf_cuda_backend.cpp
  src/restoration/stdf_cuda_backend.hpp
  src/restoration/stdf_luma_adapter.hpp
  src/restoration/stdf_tile_policy.hpp
  src/restoration/stdf_tiled_adapter.hpp
  src/restoration/temporal_window.cpp
  src/restoration/temporal_window.hpp
  src/restoration/video_restoration_backend.cpp
  src/restoration/video_restoration_backend.hpp
  src/timeline/timeline.cpp
  src/timeline/timeline.hpp)

function(rifelove_generate_processing_identity source_root output_directory)
  # This version defines the canonical source-manifest encoding and the
  # supported pipeline contract; change it for non-source algorithm inputs.
  set(algorithm_version "rife-processing-source-contract-v1")
  set(files ${rifelove_processing_identity_sources})
  list(SORT files)
  list(LENGTH files file_count)
  list(REMOVE_DUPLICATES files)
  list(LENGTH files unique_count)
  if(NOT file_count EQUAL unique_count)
    message(FATAL_ERROR "Duplicate controlled processing identity source")
  endif()
  # GLOB is only an omission detector, never the identity membership rule.
  if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE candidates RELATIVE "${source_root}" "${source_root}/src/*")
  else()
    file(GLOB_RECURSE candidates CONFIGURE_DEPENDS RELATIVE "${source_root}" "${source_root}/src/*")
  endif()
  foreach(candidate IN LISTS candidates)
    if(NOT candidate IN_LIST files)
      message(FATAL_ERROR "Unreviewed processing identity source: ${candidate}. Update cmake/ProcessingPipelineIdentity.cmake explicitly.")
    endif()
  endforeach()
  set(canonical "algorithm=${algorithm_version}\n")
  set(entries)
  foreach(relative IN LISTS files)
    if(NOT EXISTS "${source_root}/${relative}" OR IS_DIRECTORY "${source_root}/${relative}")
      message(FATAL_ERROR "Controlled processing identity source missing: ${relative}")
    endif()
    file(SHA256 "${source_root}/${relative}" digest)
    string(APPEND canonical "${relative}=${digest}\n")
    list(APPEND entries "\"${relative}\":\"${digest}\"")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${source_root}/${relative}")
    endif()
  endforeach()
  string(SHA256 source_sha256 "${canonical}")
  string(JOIN "," source_entries ${entries})
  string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
  string(SHA256 flags_sha256 "${CMAKE_CXX_FLAGS}\n${CMAKE_CXX_FLAGS_${build_type_upper}}\n")
  if(CMAKE_CONFIGURATION_TYPES OR NOT CMAKE_CXX_COMPILER_ID OR NOT CMAKE_CXX_COMPILER_VERSION OR NOT CMAKE_BUILD_TYPE)
    message(FATAL_ERROR "Processing checkpoint identity requires an explicit compiler and single-configuration build type")
  endif()
  set(processing_identity_json "{\"schemaVersion\":1,\"algorithmVersion\":\"${algorithm_version}\",\"sourceSha256\":\"${source_sha256}\",\"sourceFiles\":{${source_entries}},\"build\":{\"compilerId\":\"${CMAKE_CXX_COMPILER_ID}\",\"compilerVersion\":\"${CMAKE_CXX_COMPILER_VERSION}\",\"buildType\":\"${CMAKE_BUILD_TYPE}\",\"cxxFlagsSha256\":\"${flags_sha256}\",\"cxxStandard\":20}}")
  file(MAKE_DIRECTORY "${output_directory}")
  configure_file("${source_root}/cmake/processing_pipeline_identity.hpp.in"
    "${output_directory}/processing_pipeline_identity.hpp" @ONLY)
  file(WRITE "${output_directory}/processing_pipeline_identity.json" "${processing_identity_json}\n")
endfunction()
