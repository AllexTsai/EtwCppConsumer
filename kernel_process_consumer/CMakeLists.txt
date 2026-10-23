cmake_minimum_required(VERSION 3.20)
project(KernelProcessConsumer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 正式程式與測試共用的程式碼
add_library(etw_consumer_core STATIC
    src/etw_raii.cpp
    src/event_callback.cpp
    src/event_parser.cpp
    src/event_slot.cpp
)
target_include_directories(etw_consumer_core PUBLIC src)
target_compile_definitions(etw_consumer_core PUBLIC _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(etw_consumer_core PUBLIC -Wall -Wextra)
target_link_libraries(etw_consumer_core PUBLIC advapi32 tdh)

add_executable(kernel_process_consumer main.cpp)
target_link_libraries(kernel_process_consumer PRIVATE etw_consumer_core)

# 測試（CTest）
enable_testing()

add_executable(test_queue_basic tests/test_queue_basic.cpp)
target_link_libraries(test_queue_basic PRIVATE etw_consumer_core)
add_test(NAME queue_basic COMMAND test_queue_basic)

add_executable(test_queue_full tests/test_queue_full.cpp)
target_link_libraries(test_queue_full PRIVATE etw_consumer_core)
add_test(NAME queue_full COMMAND test_queue_full)

add_executable(test_queue_stress tests/test_queue_stress.cpp)
target_link_libraries(test_queue_stress PRIVATE etw_consumer_core)
add_test(NAME queue_stress COMMAND test_queue_stress)
set_tests_properties(queue_stress PROPERTIES TIMEOUT 300)

# 需要管理員權限與真實 ETW；權限不足時回傳 77 視為 SKIP
add_executable(test_parse_copy tests/test_parse_copy.cpp)
target_link_libraries(test_parse_copy PRIVATE etw_consumer_core)
add_test(NAME parse_copy COMMAND test_parse_copy)
set_tests_properties(parse_copy PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
