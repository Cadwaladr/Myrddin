// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Force-included into AMD's FidelityFX SDK 1.1.4 host sources (externals/FidelityFX-SDK/src) on
// platforms without the MSVC secure CRT, which those sources call. Each stand-in truncates and
// terminates as the MSVC function does on success; none of AMD's calls relies on its failure
// handler.

#pragma once

#if !defined(_MSC_VER)

// Headers MSVC's standard library brings in by the way: ffx_util.h uses std::popcount, and
// ffx_vk.cpp log2 and std::wstring_convert.
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <locale>

#ifndef _countof
#define _countof(array) (sizeof(array) / sizeof((array)[0]))
#endif

inline int wcscpy_s(wchar_t* destination, std::size_t size, const wchar_t* source) {
    if (size == 0) {
        return 1;
    }
    std::wcsncpy(destination, source, size - 1);
    destination[size - 1] = L'\0';
    return 0;
}

template <std::size_t N>
inline int wcscpy_s(wchar_t (&destination)[N], const wchar_t* source) {
    return wcscpy_s(destination, N, source);
}

inline int strcpy_s(char* destination, std::size_t size, const char* source) {
    if (size == 0) {
        return 1;
    }
    std::strncpy(destination, source, size - 1);
    destination[size - 1] = '\0';
    return 0;
}

template <std::size_t N>
inline int strcpy_s(char (&destination)[N], const char* source) {
    return strcpy_s(destination, N, source);
}

/// MSVC's wcstombs_s: converts at most count wide characters (or until size - 1 bytes) and always
/// terminates the destination; converted counts the terminator.
inline int wcstombs_s(std::size_t* converted, char* destination, std::size_t size,
                      const wchar_t* source, std::size_t count) {
    if (size == 0) {
        return 1;
    }
    std::size_t limit = size - 1;
    if (count < limit) {
        limit = count;
    }
    std::size_t written = std::wcstombs(destination, source, limit);
    if (written == static_cast<std::size_t>(-1)) {
        written = 0;
    }
    if (written > limit) {
        written = limit;
    }
    destination[written] = '\0';
    if (converted) {
        *converted = written + 1;
    }
    return 0;
}

#define sprintf_s snprintf
#define swprintf_s swprintf

// ffx_message.cpp uses it outside _WIN32 without including ffx_util.h, which defines it the same.
#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif

#endif // !defined(_MSC_VER)
