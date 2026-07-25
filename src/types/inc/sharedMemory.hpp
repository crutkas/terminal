/*++
Copyright (c) Microsoft Corporation

Module Name:
- sharedMemory.hpp

Abstract:
- Reading a named shared-memory object handed to us by a terminal client.
--*/

#pragma once

namespace Microsoft::Console::Utils
{
    // What ReadSharedMemory managed to do. not_found: no object of that name exists.
    // invalid: the request was malformed -- an empty or over-long name, a name we will
    // not open, or a byte range the mapping cannot satisfy. read_error: the object
    // exists but could not be mapped or copied.
    enum class ReadSharedMemoryResult : uint8_t
    {
        ok,
        not_found,
        invalid,
        read_error,
    };

    // Copies bytes out of a named shared-memory object into 'out'.
    //
    // The name arrives from outside, so this is deliberately narrow about what it will
    // open. Windows file mappings are session-local by default; only unprefixed names
    // and an explicit Local\ prefix are accepted, so a terminal client cannot reach a
    // service's Global\ object. The mapping is opened read-only, never inherited, copied
    // into memory we own, and closed immediately. Unlike POSIX shm there is no unlink
    // step: the object goes away once every process has closed its handles.
    //
    // 'size' of 0 means "to the end of the mapping". Windows rounds paging-file sections
    // up to a page boundary, so a caller that needs an exact non-page-aligned length has
    // to know it and pass it. Never throws.
    ReadSharedMemoryResult ReadSharedMemory(const std::wstring_view name, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) noexcept;
}