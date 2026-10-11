find_package(CUDAToolkit REQUIRED)
find_package(Threads REQUIRED)

if(WIN32)
  # Windows consumes prebuilt shared archives laid out as include/, lib/ and bin/:
  # NINFER_FFMPEG_ROOT is an FFmpeg win64 *-shared build, NINFER_CURL_ROOT the curl.se win64
  # archive. The imported target names match the pkg-config ones used on Linux.
  set(ninfer_runtime_dlls "")

  function(ninfer_import_prebuilt target root)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "LIBS;DLLS")
    if(NOT IS_DIRECTORY "${root}/include")
      message(FATAL_ERROR "${target}: missing ${root}/include")
    endif()
    add_library(${target} INTERFACE IMPORTED GLOBAL)
    target_include_directories(${target} INTERFACE "${root}/include")
    foreach(lib IN LISTS arg_LIBS)
      # MSVC import libraries (.lib) or MinGW ones (.dll.a), which MSVC links as well.
      find_library(ninfer_${lib}_import NAMES ${lib}.lib ${lib}.dll.a lib${lib}.dll.a
        PATHS "${root}/lib" NO_DEFAULT_PATH REQUIRED)
      target_link_libraries(${target} INTERFACE "${ninfer_${lib}_import}")
    endforeach()
    foreach(pattern IN LISTS arg_DLLS)
      file(GLOB matched "${root}/bin/${pattern}")
      list(LENGTH matched count)
      if(NOT count EQUAL 1)
        message(FATAL_ERROR "${target}: expected one ${root}/bin/${pattern}, found ${count}")
      endif()
      list(APPEND ninfer_runtime_dlls ${matched})
    endforeach()
    set(ninfer_runtime_dlls ${ninfer_runtime_dlls} PARENT_SCOPE)
  endfunction()

  ninfer_import_prebuilt(PkgConfig::FFMPEG "${NINFER_FFMPEG_ROOT}"
    LIBS avformat avcodec avutil swscale
    DLLS avformat-*.dll avcodec-*.dll avutil-*.dll swscale-*.dll swresample-*.dll)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale)
endif()

# Repository-pinned header dependencies. No configure-time downloads.
add_library(ninfer::json INTERFACE IMPORTED GLOBAL)
target_include_directories(ninfer::json INTERFACE
  ${PROJECT_SOURCE_DIR}/third_party)

# Source base for the custom-template frontend; consumers will link it explicitly.
add_subdirectory(third_party/llama-jinja EXCLUDE_FROM_ALL)

# CPU grammar source base; consumers will link it explicitly.
add_subdirectory(third_party/xgrammar EXCLUDE_FROM_ALL)

if(NINFER_BUILD_PRODUCT_SUPPORT)
  # Media acquisition uses CURLOPT_PROTOCOLS_STR and CURLOPT_REDIR_PROTOCOLS_STR,
  # introduced in libcurl 7.85 (not merely the version of the maintainer environment).
  if(WIN32)
    ninfer_import_prebuilt(PkgConfig::LIBCURL "${NINFER_CURL_ROOT}"
      LIBS libcurl DLLS libcurl*.dll)
  else()
    pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl>=7.85)
  endif()
  add_library(ninfer::httplib INTERFACE IMPORTED GLOBAL)
  target_include_directories(ninfer::httplib INTERFACE
    ${PROJECT_SOURCE_DIR}/third_party/cpp-httplib)
  add_subdirectory(third_party/spdlog)
endif()

# Windows executables load the prebuilt dependency DLLs from their own directory.
function(ninfer_copy_runtime_dlls target)
  if(WIN32 AND ninfer_runtime_dlls)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ninfer_runtime_dlls}
        $<TARGET_FILE_DIR:${target}>)
  endif()
endfunction()
