// 測試輔助工具：對指定 PID 所在的 console 送出 CTRL_C_EVENT
#include <windows.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::printf("usage: sendctrlc <pid>\n");
        return 1;
    }
    DWORD pid = std::strtoul(argv[1], nullptr, 10);
    FreeConsole();
    if (!AttachConsole(pid)) {
        std::printf("AttachConsole(%lu) failed, error=%lu\n", pid, GetLastError());
        return 2;
    }
    // 自己忽略 Ctrl+C，避免把自己也停掉
    if (!SetConsoleCtrlHandler(nullptr, TRUE)) {
        std::printf("SetConsoleCtrlHandler failed, error=%lu\n", GetLastError());
        return 3;
    }
    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
        std::printf("GenerateConsoleCtrlEvent failed, error=%lu\n", GetLastError());
        return 4;
    }
    Sleep(500);
    FreeConsole();
    std::printf("CTRL_C_EVENT sent to console of pid %lu\n", pid);
    return 0;
}
