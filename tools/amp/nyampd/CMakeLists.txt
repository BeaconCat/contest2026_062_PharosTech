cmake_minimum_required(VERSION 3.16)
project(nyampd C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(../protocol protocol)

add_library(nyampd_core STATIC nyampd_core.cpp)
target_include_directories(nyampd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nyampd_core PUBLIC nyamp_protocol)
target_compile_options(nyampd_core PRIVATE -Wall -Wextra -Werror)

add_executable(nyampd nyampd_main.cpp)
target_link_libraries(nyampd PRIVATE nyampd_core)
target_compile_options(nyampd PRIVATE -Wall -Wextra -Werror)

set(NYAMP_RKNN_ROOT "" CACHE PATH "External Linux AArch64 RKNN runtime SDK")
if(NYAMP_RKNN_ROOT)
  target_sources(nyampd PRIVATE nyampd_npu.cpp)
  target_include_directories(nyampd PRIVATE "${NYAMP_RKNN_ROOT}/include")
  target_link_libraries(nyampd PRIVATE "${NYAMP_RKNN_ROOT}/lib/librknnrt.so")
  target_compile_definitions(nyampd PRIVATE NYAMP_RKNN_ENABLED=1)
  set_target_properties(nyampd PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "$ORIGIN/../lib")
endif()

add_executable(nyampd_core_test nyampd_core_test.cpp)
target_link_libraries(nyampd_core_test PRIVATE nyampd_core)
target_compile_options(nyampd_core_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME nyampd_core_test COMMAND nyampd_core_test)
