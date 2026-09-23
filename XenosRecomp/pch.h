#pragma once

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

// MSVC has no __builtin_bswap16/32/64 and spells the same operations with an underscore, which
// is what byteSwap below uses when it is compiled by MSVC or by clang-cl, both of which define
// _MSC_VER. GCC and Clang keep the builtins.
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <dxcapi.h>

#include <bit>
#include <cassert>
#include <cstdint>
#include <execution>
#include <filesystem>
#include <map>
#include <set>
#include <smolv.h>
#include <fmt/core.h>
#include <string>
#include <unordered_map>
#include <xxhash.h>
#include <zstd.h>

template<typename T>
static T byteSwap(T value)
{
    if constexpr (sizeof(T) == 1)
        return value;
    else if constexpr (sizeof(T) == 2)
#if defined(_MSC_VER)
        return static_cast<T>(_byteswap_ushort(static_cast<uint16_t>(value)));
#else
        return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(value)));
#endif
    else if constexpr (sizeof(T) == 4)
#if defined(_MSC_VER)
        return static_cast<T>(_byteswap_ulong(static_cast<uint32_t>(value)));
#else
        return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(value)));
#endif
    else if constexpr (sizeof(T) == 8)
#if defined(_MSC_VER)
        return static_cast<T>(_byteswap_uint64(static_cast<uint64_t>(value)));
#else
        return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(value)));
#endif
    else
    {
        static_assert(sizeof(T) == 8, "Unexpected byte size.");
        return value;
    }
}

template<typename T>
struct be
{
    T value;

    T get() const
    {
        if constexpr (std::is_enum_v<T>)
            return T(byteSwap(std::underlying_type_t<T>(value)));
        else
            return byteSwap(value);
    }

    operator T() const
    {
        return get();
    }
};  
