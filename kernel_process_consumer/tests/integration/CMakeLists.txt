# 整合測試用的輔助工具，由 run_test.ps1 / run_test_console.ps1 呼叫。
# 不加入 ctest：整合測試需要管理員權限與互動式 console，用腳本手動執行。
# 輸出到 build 根目錄（與 kernel_process_consumer.exe 同一層），腳本從同一處取得所有 exe。

add_executable(sendctrlc sendctrlc.cpp)
add_executable(getmode getmode.cpp)

foreach(tool sendctrlc getmode)
    target_compile_options(${tool} PRIVATE -Wall -Wextra)
    set_target_properties(${tool} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()
