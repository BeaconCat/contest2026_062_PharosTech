# ##############################################################################
# app/nyabula/host/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

cmake_minimum_required(VERSION 3.20)

project(nyabula_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(nyabula_runtime STATIC
  src/json.cxx
  src/runtime.cxx)
target_include_directories(nyabula_runtime PUBLIC include)
if(MSVC)
  target_compile_options(nyabula_runtime PUBLIC /utf-8)
endif()

add_executable(nyabula_host
  src/main.cxx
  src/persistence.cxx
  src/windows_gateway.cxx)
target_include_directories(nyabula_host PRIVATE include)
target_link_libraries(nyabula_host PRIVATE nyabula_runtime ws2_32)
target_compile_definitions(nyabula_host PRIVATE
  NYABULA_HOST_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/web"
  NYABULA_APP_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")

add_executable(nyabula_runtime_tests tests/runtime_tests.cxx)
target_link_libraries(nyabula_runtime_tests PRIVATE nyabula_runtime)

enable_testing()
add_test(NAME nyabula_runtime_tests COMMAND nyabula_runtime_tests)
