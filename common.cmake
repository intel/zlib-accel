# Copyright (C) 2025 Intel Corporation
# SPDX-License-Identifier: Apache-2.0

option(USE_IAA "Use IAA (requires QPL)" OFF)
option(USE_QAT "Use QAT (requires QATzip)" OFF)
option(DEBUG_LOG "for logging" ON)
option(COVERAGE "for coverage" OFF)

if(USE_IAA)
  add_compile_definitions(USE_IAA)
endif()

if(USE_QAT)
  add_compile_definitions(USE_QAT)
endif()

if(DEBUG_LOG)
  add_compile_definitions(DEBUG_LOG)
endif()

if(CMAKE_BUILD_TYPE STREQUAL Debug)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0")
endif()

if(COVERAGE)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage")
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

if(USE_QAT)
  if(DEFINED QATZIP_PATH)
    message(STATUS "Using QATZIP_PATH: ${QATZIP_PATH}")
    include_directories(${QATZIP_PATH}/include)
    link_directories(PUBLIC ${QATZIP_PATH}/src/.libs)
  endif()
  link_libraries(qatzip)
endif()

link_libraries(z)
link_libraries(dl)
