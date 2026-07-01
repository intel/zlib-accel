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
  if(NOT ISAL_PATH)
    message(FATAL_ERROR "ISAL_PATH must be set when USE_IGZIP=ON.")
  endif()
  set(_isal_api_header "${ISAL_PATH}/include/isal_api.h")
  if(NOT EXISTS "${_isal_api_header}")
    message(FATAL_ERROR "ISA-L header not found: ${_isal_api_header}. Check ISAL_PATH.")
  endif()
  file(STRINGS "${_isal_api_header}" _isal_major_line REGEX "^#define ISAL_MAJOR_VERSION ")
  file(STRINGS "${_isal_api_header}" _isal_minor_line REGEX "^#define ISAL_MINOR_VERSION ")
  file(STRINGS "${_isal_api_header}" _isal_patch_line REGEX "^#define ISAL_PATCH_VERSION ")
  string(REGEX REPLACE "^#define ISAL_MAJOR_VERSION ([0-9]+).*" "\\1" _isal_major "${_isal_major_line}")
  string(REGEX REPLACE "^#define ISAL_MINOR_VERSION ([0-9]+).*" "\\1" _isal_minor "${_isal_minor_line}")
  string(REGEX REPLACE "^#define ISAL_PATCH_VERSION ([0-9]+).*" "\\1" _isal_patch "${_isal_patch_line}")
  set(_isal_version_found "${_isal_major}.${_isal_minor}.${_isal_patch}")
  message(STATUS "Found ISA-L version: ${_isal_version_found} at ${ISAL_PATH}")
  if(_isal_version_found VERSION_LESS "2.32.1")
    message(FATAL_ERROR
      "ISA-L >= 2.32.1 required, found ${_isal_version_found}.\n"
      "Update ISA-L at ISAL_PATH=${ISAL_PATH} or pass -DISAL_PATH=<path> to cmake.\n"
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
    link_directories(PUBLIC ${QPL_PATH}/lib64 ${QPL_PATH}/lib)
    link_libraries(qpl)
  endif()
endif()

if(USE_IGZIP)
	if(NOT DEFINED ISAL_PATH)
	  find_package(isal REQUIRED)
	  if(isal_FOUND)
	    message(STATUS "Found ISA-L: ${isal_DIR}")
	    link_libraries(isal)
	  endif()
	  else()
    message(STATUS "Using ISAL_PATH: ${ISAL_PATH}")
    include_directories(${ISAL_PATH}/include)
    link_directories(PUBLIC ${ISAL_PATH}/.libs)
    link_libraries(isal)
  endif()
endif()

if(USE_QAT)
  if(DEFINED QATZIP_PATH)
    message(STATUS "Using QATZIP_PATH: ${QATZIP_PATH}")
    include_directories(${QATZIP_PATH}/include)
    link_directories(PUBLIC ${QATZIP_PATH}/src/.libs)
  endif()
  link_libraries(qatzip)
endif()

link_libraries(z)
