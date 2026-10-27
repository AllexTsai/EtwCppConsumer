// 測試輔助工具：印出目前 console stdin 的模式
#include <windows.h>
#include <cstdio>

int main(int argc, char** argv)
{
    const char* label = argc > 1 ? argv[1] : "";
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in == INVALID_HANDLE_VALUE || in == nullptr || !GetConsoleMode(in, &mode)) {
        std::printf("[getmode %s] GetConsoleMode failed, error=%lu\n", label, GetLastError());
        return 1;
    }
    std::printf("[getmode %s] stdin mode=0x%04lX QuickEdit=%s ExtendedFlags=%s\n", label, mode,
                (mode & ENABLE_QUICK_EDIT_MODE) ? "ON" : "OFF", (mode & ENABLE_EXTENDED_FLAGS) ? "ON" : "OFF");
    return 0;
}
