#pragma once

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <cstdio>
#include <cstdlib>

// 不使用 <cassert>：定義 NDEBUG 時 assert 會整個消失，測試就不再檢查任何東西。
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, __LINE__); \
            std::fflush(stderr);                                                          \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const unsigned long long va_ = static_cast<unsigned long long>(a);                         \
        const unsigned long long vb_ = static_cast<unsigned long long>(b);                         \
        if (va_ != vb_) {                                                                           \
            std::fprintf(stderr, "CHECK_EQ failed: %s (%llu) != %s (%llu)  (%s:%d)\n", #a, va_, #b, vb_, \
                         __FILE__, __LINE__);                                                       \
            std::fflush(stderr);                                                                    \
            std::exit(1);                                                                           \
        }                                                                                           \
    } while (0)

// 測試用的另一個 provider GUID（任意值，與 Kernel-Process 不同）
constexpr GUID kTestOtherProvider = {
    0x11111111, 0x2222, 0x3333, {0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55}};

inline EVENT_RECORD MakeFakeRecord(const GUID& provider, void* userData, USHORT length, void* userContext)
{
    EVENT_RECORD r{};
    r.EventHeader.Size = sizeof(EVENT_HEADER);
    r.EventHeader.ProviderId = provider;
    r.UserData = userData;
    r.UserDataLength = length;
    r.UserContext = userContext;
    return r;
}
