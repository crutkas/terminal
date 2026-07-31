// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "inc/sharedMemory.hpp"

using namespace Microsoft::Console::Utils;

// These two exist because the pages behind a client-supplied mapping can be
// decommitted at any moment; a plain memcpy would take an access violation on
// memory we do not control the lifetime of.
namespace
{
    bool is_readable_range(const void* source, const size_t size) noexcept
    {
        if (size == 0)
        {
            return true;
        }

        const auto begin = reinterpret_cast<uintptr_t>(source);
        if (size > UINTPTR_MAX - begin)
        {
            return false;
        }

        const auto end = begin + size;
        auto current = begin;
        while (current < end)
        {
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) != sizeof(info) ||
                info.State != MEM_COMMIT)
            {
                return false;
            }

            const auto protection = info.Protect & 0xff;
            const auto readable = protection == PAGE_READONLY ||
                                  protection == PAGE_READWRITE ||
                                  protection == PAGE_WRITECOPY ||
                                  protection == PAGE_EXECUTE_READ ||
                                  protection == PAGE_EXECUTE_READWRITE ||
                                  protection == PAGE_EXECUTE_WRITECOPY;
            if (!readable || (info.Protect & PAGE_GUARD) != 0)
            {
                return false;
            }

            const auto regionBegin = reinterpret_cast<uintptr_t>(info.BaseAddress);
            if (info.RegionSize > UINTPTR_MAX - regionBegin)
            {
                return false;
            }
            const auto regionEnd = regionBegin + info.RegionSize;
            if (regionEnd <= current)
            {
                return false;
            }
            current = std::min(regionEnd, end);
        }

        return true;
    }

    // A file-backed section can fault after it has been mapped (for example if its
    // backing file is truncated). Keep SEH in a leaf with no C++ objects requiring
    // unwinding, then translate the fault into a normal protocol read error.
    bool guarded_copy(void* destination, const void* source, const size_t size) noexcept
    {
        __try
        {
            memcpy(destination, source, size);
            return true;
        }
        __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ||
                          GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ||
                          GetExceptionCode() == EXCEPTION_GUARD_PAGE ?
                      EXCEPTION_EXECUTE_HANDLER :
                      EXCEPTION_CONTINUE_SEARCH)
        {
            return false;
        }
    }
}

ReadSharedMemoryResult Microsoft::Console::Utils::ReadSharedMemory(const std::wstring_view name, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) noexcept
{
    out.clear();

    constexpr uint64_t maxBytes = 32ull * 1024 * 1024;
    constexpr size_t maxNameChars = 32767;

    const auto hasControlCharacter = std::any_of(name.begin(), name.end(), [](const wchar_t value) noexcept {
        return value < L' ' || value == 0x7f;
    });
    if (name.empty() || name.size() > maxNameChars || hasControlCharacter)
    {
        return ReadSharedMemoryResult::invalid;
    }

    // Kernel object prefixes are case-sensitive. Permit the current-session
    // namespace, either implicitly or via Local\, and reject every other backslash
    // (including Global\, Session\, and nested object-manager paths).
    auto remainder = name;
    if (remainder.starts_with(L"Local\\"))
    {
        remainder.remove_prefix(6);
    }
    if (remainder.empty() || remainder.find(L'\\') != std::wstring_view::npos)
    {
        return ReadSharedMemoryResult::invalid;
    }

    std::wstring nameStr;
    try
    {
        nameStr.assign(name);
    }
    catch (...)
    {
        return ReadSharedMemoryResult::read_error;
    }

    wil::unique_handle mapping{ OpenFileMappingW(FILE_MAP_READ, FALSE, nameStr.c_str()) };
    if (!mapping)
    {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
        {
            return ReadSharedMemoryResult::not_found;
        }
        if (error == ERROR_INVALID_NAME || error == ERROR_INVALID_PARAMETER)
        {
            return ReadSharedMemoryResult::invalid;
        }
        return ReadSharedMemoryResult::read_error;
    }

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto granularity = static_cast<uint64_t>(systemInfo.dwAllocationGranularity);
    if (granularity == 0)
    {
        return ReadSharedMemoryResult::read_error;
    }

    const auto alignedOffset = offset - (offset % granularity);
    const auto delta = static_cast<size_t>(offset - alignedOffset);
    const auto offsetHigh = static_cast<DWORD>(alignedOffset >> 32);
    const auto offsetLow = static_cast<DWORD>(alignedOffset);

    const auto map = [&](const size_t bytes) noexcept {
        return MapViewOfFile(mapping.get(), FILE_MAP_READ, offsetHigh, offsetLow, bytes);
    };
    const auto mapFailure = []() noexcept {
        const auto error = GetLastError();
        return (error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_OUTOFMEMORY) ? ReadSharedMemoryResult::read_error : ReadSharedMemoryResult::invalid;
    };

    auto toRead = static_cast<size_t>(std::min(size, maxBytes));
    void* view = nullptr;

    if (size != 0)
    {
        view = map(delta + toRead);
        if (!view)
        {
            return mapFailure();
        }
    }
    else
    {
        // Avoid mapping an attacker-sized section merely to discover its end. A
        // bounded view succeeds for large sections; only smaller sections need the
        // map-to-end fallback whose extent is then measured with VirtualQuery.
        view = map(delta + static_cast<size_t>(maxBytes));
        if (view)
        {
            toRead = static_cast<size_t>(maxBytes);
        }
        else
        {
            view = map(0);
            if (!view)
            {
                return mapFailure();
            }

            const auto viewBegin = reinterpret_cast<uintptr_t>(view);
            const auto maximumExtent = delta + static_cast<size_t>(maxBytes);
            if (maximumExtent > UINTPTR_MAX - viewBegin)
            {
                UnmapViewOfFile(view);
                return ReadSharedMemoryResult::read_error;
            }

            const auto limit = viewBegin + maximumExtent;
            auto current = viewBegin;
            while (current < limit)
            {
                MEMORY_BASIC_INFORMATION info{};
                if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) != sizeof(info))
                {
                    UnmapViewOfFile(view);
                    return ReadSharedMemoryResult::read_error;
                }
                if (info.AllocationBase != view)
                {
                    break;
                }

                const auto regionBegin = reinterpret_cast<uintptr_t>(info.BaseAddress);
                if (regionBegin > current || info.RegionSize > UINTPTR_MAX - regionBegin)
                {
                    UnmapViewOfFile(view);
                    return ReadSharedMemoryResult::read_error;
                }
                const auto regionEnd = regionBegin + info.RegionSize;
                if (regionEnd <= current)
                {
                    UnmapViewOfFile(view);
                    return ReadSharedMemoryResult::read_error;
                }
                current = std::min(regionEnd, limit);
            }

            const auto viewExtent = current - viewBegin;
            if (viewExtent <= delta)
            {
                UnmapViewOfFile(view);
                return ReadSharedMemoryResult::read_error;
            }
            toRead = std::min(viewExtent - delta, static_cast<size_t>(maxBytes));
        }
    }

    struct mapped_view
    {
        void* value;
        ~mapped_view()
        {
            if (value)
            {
                UnmapViewOfFile(value);
            }
        }
    } viewGuard{ view };

    const auto first = static_cast<const uint8_t*>(view) + delta;
    if (!is_readable_range(first, toRead))
    {
        return ReadSharedMemoryResult::read_error;
    }

    try
    {
        out.resize(toRead);
    }
    catch (...)
    {
        out.clear();
        return ReadSharedMemoryResult::read_error;
    }
    if (!guarded_copy(out.data(), first, toRead))
    {
        out.clear();
        return ReadSharedMemoryResult::read_error;
    }

    return ReadSharedMemoryResult::ok;
}