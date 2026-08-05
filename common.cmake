# Copyright (C) 2025 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

option(USE_IAA "Use IAA (requires QPL)" OFF)
option(USE_IGZIP "Use IGZIP (requires isa-l)" OFF)
option(USE_QAT "Use QAT (requires QATzip)" OFF)
option(DEBUG_LOG "for logging" ON)
option(COVERAGE "for coverage" OFF)
option(ASAN "Enable AddressSanitizer" OFF)
option(UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
# TSAN may require disabling ASLR at runtime.
option(TSAN "Enable ThreadSanitizer" OFF)

if(USE_IAA)
  add_compile_definitions(USE_IAA)
endif()

if(USE_IGZIP)
  add_compile_definitions(USE_IGZIP)

  # Enforce minimum ISA-L version. v2.32.1 contains critical igzip bugfixes:
  #   - next_in/avail_in not reset after block finish (inflate boundary bug)
  #   - zlib DICTID stored/read in wrong byte order
  #   - gzip_flag mutated by isal_deflate when writing wrapper header
  # Earlier versions require defensive workarounds that have been removed.
  if(ISAL_PATH)
    # A build tree keeps headers in include/; an install prefix puts them in
    # include/isa-l/.
    if(EXISTS "${ISAL_PATH}/include/isal_api.h")
      set(_isal_api_header "${ISAL_PATH}/include/isal_api.h")
    elseif(EXISTS "${ISAL_PATH}/include/isa-l/isal_api.h")
      set(_isal_api_header "${ISAL_PATH}/include/isa-l/isal_api.h")
    endif()
  else()
    find_path(_isal_api_include isal_api.h PATH_SUFFIXES include include/isa-l isa-l)
    if(_isal_api_include)
      set(_isal_api_header "${_isal_api_include}/isal_api.h")
    endif()
    unset(_isal_api_include CACHE)
  endif()
  if(NOT _isal_api_header OR NOT EXISTS "${_isal_api_header}")
    message(FATAL_ERROR "ISA-L header (isal_api.h) not found. Set -DISAL_PATH=<path> or install ISA-L system-wide.")
  endif()
  file(STRINGS "${_isal_api_header}" _isal_major_line REGEX "^#define[ \t]+ISAL_MAJOR_VERSION[ \t]+")
  file(STRINGS "${_isal_api_header}" _isal_minor_line REGEX "^#define[ \t]+ISAL_MINOR_VERSION[ \t]+")
  file(STRINGS "${_isal_api_header}" _isal_patch_line REGEX "^#define[ \t]+ISAL_PATCH_VERSION[ \t]+")
  if(NOT _isal_major_line OR NOT _isal_minor_line OR NOT _isal_patch_line)
    message(FATAL_ERROR "Could not find ISAL_MAJOR/MINOR/PATCH_VERSION macros in ${_isal_api_header}. ISA-L header may be malformed or too old.")
  endif()
  list(GET _isal_major_line 0 _isal_major_line)
  list(GET _isal_minor_line 0 _isal_minor_line)
  list(GET _isal_patch_line 0 _isal_patch_line)
  string(REGEX REPLACE "^#define[ \t]+ISAL_MAJOR_VERSION[ \t]+([0-9]+).*" "\\1" _isal_major "${_isal_major_line}")
  string(REGEX REPLACE "^#define[ \t]+ISAL_MINOR_VERSION[ \t]+([0-9]+).*" "\\1" _isal_minor "${_isal_minor_line}")
  string(REGEX REPLACE "^#define[ \t]+ISAL_PATCH_VERSION[ \t]+([0-9]+).*" "\\1" _isal_patch "${_isal_patch_line}")
  foreach(_v _isal_major _isal_minor _isal_patch)
    if(NOT "${${_v}}" MATCHES "^[0-9]+$")
      message(FATAL_ERROR "Failed to parse ISA-L version from ${_isal_api_header}: '${_v}' = '${${_v}}'")
    endif()
  endforeach()
  set(_isal_version_found "${_isal_major}.${_isal_minor}.${_isal_patch}")
  message(STATUS "Found ISA-L version: ${_isal_version_found} (${_isal_api_header})")
  if(_isal_version_found VERSION_LESS "2.32.1")
    message(FATAL_ERROR
      "ISA-L >= 2.32.1 required, found ${_isal_version_found}.\n"
      "Update ISA-L or pass -DISAL_PATH=<path> to cmake.\n"
      "Release: https://github.com/intel/isa-l/releases/tag/v2.32.1")
  endif()
  unset(_isal_api_header)
  unset(_isal_major_line)
  unset(_isal_minor_line)
  unset(_isal_patch_line)
  unset(_isal_major)
  unset(_isal_minor)
  unset(_isal_patch)
  unset(_isal_version_found)
endif()

if(USE_QAT)
  add_compile_definitions(USE_QAT)
endif()

if(DEBUG_LOG)
  add_compile_definitions(DEBUG_LOG)
endif()

set(COMPILER_FLAGS "-Wall -Wextra -Werror \
-fvisibility=hidden \
-Wformat -Wformat-security -Werror=format-security \
-D_FORTIFY_SOURCE=2 \
-fstack-protector-strong")
# UBSAN not compatible with -flto
if(NOT UBSAN)
  set(COMPILER_FLAGS "${COMPILER_FLAGS} -flto")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(STATUS "GCC detected.")
  set(COMPILER_FLAGS "${COMPILER_FLAGS} -Wl,-z,noexecstack,-z,relro,-z,now -mindirect-branch-register")
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(STATUS "Clang detected.")
  set(COMPILER_FLAGS "${COMPILER_FLAGS}  -mretpoline")
  # For UBSAN, -flto is disabled. -fsanitize=cfi depends on it.
  if(NOT UBSAN)
    set(COMPILER_FLAGS "${COMPILER_FLAGS} -fsanitize=cfi")
  endif()
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMPILER_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMPILER_FLAGS}")

if(CMAKE_BUILD_TYPE STREQUAL Debug)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0")
endif()

if(COVERAGE)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage")
endif()

if(ASAN)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
endif()

if(UBSAN)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=undefined")
endif()

if(TSAN)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=thread")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

if(USE_IAA)
  if(NOT DEFINED QPL_PATH)
    find_package(Qpl REQUIRED)
    if(Qpl_FOUND)
      message(STATUS "Found QPL: ${Qpl_DIR}")
      link_libraries(Qpl::qpl)
    endif()
  else()
    message(STATUS "Using QPL_PATH: ${QPL_PATH}")
    include_directories(${QPL_PATH}/include/qpl ${QPL_PATH}/include)
    link_directories(${QPL_PATH}/lib64 ${QPL_PATH}/lib)
    link_libraries(qpl)
  endif()
endif()

if(USE_IGZIP)
  # Same predicate as the version check above: if(ISAL_PATH) rather than
  # if(DEFINED ISAL_PATH), so a defined-but-empty -DISAL_PATH= does not send the
  # two blocks down different branches.
  if(NOT ISAL_PATH)
    # find_package would need an ISALConfig.cmake, which autotools installs of
    # ISA-L (including Ubuntu's libisal-dev) do not ship, and whose exported
    # target is namespaced ISAL::isal rather than plain isal. Locate the library
    # and headers directly instead, so any system install works.
    find_library(ISAL_LIB NAMES isal)
    if(NOT ISAL_LIB)
      message(FATAL_ERROR
        "ISA-L library (libisal) not found. Install ISA-L or set -DISAL_PATH=<path>.")
    endif()
    find_path(ISAL_INCLUDE_DIR igzip_lib.h PATH_SUFFIXES include include/isa-l isa-l)
    if(NOT ISAL_INCLUDE_DIR)
      message(FATAL_ERROR
        "ISA-L headers not found. Install ISA-L or set -DISAL_PATH=<path>.")
    endif()
    message(STATUS "Found ISA-L: ${ISAL_LIB} (headers: ${ISAL_INCLUDE_DIR})")
    include_directories(${ISAL_INCLUDE_DIR})
    link_libraries(${ISAL_LIB})
  else()
    message(STATUS "Using ISAL_PATH: ${ISAL_PATH}")
    # ISAL_PATH may point at either an ISA-L build tree (headers in include/,
    # library in .libs/) or an install prefix (headers in include/isa-l/,
    # library in lib64/ or lib/). Search all of them.
    include_directories(${ISAL_PATH}/include ${ISAL_PATH}/include/isa-l)
    link_directories(${ISAL_PATH}/.libs ${ISAL_PATH}/lib64 ${ISAL_PATH}/lib)
    link_libraries(isal)
  endif()
endif()

if(USE_QAT)
  if(DEFINED QATZIP_PATH)
    message(STATUS "Using QATZIP_PATH: ${QATZIP_PATH}")
    include_directories(${QATZIP_PATH}/include)
    link_directories(${QATZIP_PATH}/src/.libs)
  endif()
  link_libraries(qatzip)
endif()

if(USE_QAT OR USE_IAA OR USE_IGZIP)
  find_package(TBB REQUIRED COMPONENTS tbb)
  link_libraries(TBB::tbb)
  add_compile_definitions(USE_TBB)
endif()

link_libraries(z)
