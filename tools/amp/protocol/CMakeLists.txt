cmake_minimum_required(VERSION 3.16)
project(nyamp_protocol C)

add_library(nyamp_protocol STATIC nyamp_protocol.c)
target_include_directories(nyamp_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(nyamp_protocol PUBLIC c_std_11)
target_compile_options(nyamp_protocol PRIVATE -Wall -Wextra -Werror)

add_executable(nyamp_protocol_test nyamp_protocol_test.c)
target_link_libraries(nyamp_protocol_test PRIVATE nyamp_protocol)
target_compile_options(nyamp_protocol_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME nyamp_protocol_test COMMAND nyamp_protocol_test)
