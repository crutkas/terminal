/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- ITerminalApi.hpp

Abstract:
- This serves as an abstraction layer for the dispatch class to connect to conhost/terminal API functions.

Author(s):
- Michael Niksa (MiNiksa) 30-July-2015
--*/

#pragma once

#include "../parser/stateMachine.hpp"
#include "../../types/inc/IInputEvent.hpp"
#include "../../types/inc/sharedMemory.hpp"
#include "../../buffer/out/LineRendition.hpp"
#include "../../buffer/out/textBuffer.hpp"
#include "../../renderer/inc/RenderSettings.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>

#include <til/io.h>

namespace Microsoft::Console::VirtualTerminal
{
    class ITerminalApi
    {
        using RenderSettings = Microsoft::Console::Render::RenderSettings;

    public:
        virtual ~ITerminalApi() = default;
        ITerminalApi() = default;
        ITerminalApi(const ITerminalApi&) = delete;
        ITerminalApi(ITerminalApi&&) = delete;
        ITerminalApi& operator=(const ITerminalApi&) = delete;
        ITerminalApi& operator=(ITerminalApi&&) = delete;

        virtual void UnknownSequence() noexcept = 0;
        virtual void ReturnResponse(const std::wstring_view response) = 0;

        struct BufferState
        {
            TextBuffer& buffer;
            til::rect viewport;
            bool isMainBuffer;
        };

        virtual bool IsConPTY() const noexcept = 0;
        virtual StateMachine& GetStateMachine() = 0;
        virtual BufferState GetBufferAndViewport() = 0;
        virtual void SetViewportPosition(const til::point position) = 0;
        virtual void SetViewportPositionInternal(const til::point position) = 0;
        virtual void RestoreViewportPositionInternal(const til::point position) = 0;

        virtual bool IsVtInputEnabled() const = 0;

        enum class Mode : size_t
        {
            AutoWrap,
            LineFeed,
            BracketedPaste
        };

        virtual void SetSystemMode(const Mode mode, const bool enabled) = 0;
        virtual bool GetSystemMode(const Mode mode) const = 0;

        virtual void ReturnAnswerback() = 0;
        virtual void WarningBell() = 0;
        virtual void SetWindowTitle(const std::wstring_view title) = 0;
        virtual void UseAlternateScreenBuffer(const TextAttribute& attrs) = 0;
        virtual void UseMainScreenBuffer() = 0;

        virtual CursorType GetUserDefaultCursorStyle() const = 0;

        virtual void ShowWindow(bool showOrHide) = 0;

        virtual void SetCodePage(const unsigned int codepage) = 0;
        virtual void ResetCodePage() = 0;
        virtual unsigned int GetOutputCodePage() const = 0;
        virtual unsigned int GetInputCodePage() const = 0;

        virtual void CopyToClipboard(const wil::zwstring_view content) = 0;
        virtual void SetTaskbarProgress(const DispatchTypes::TaskbarState state, const size_t progress) = 0;
        virtual void SetWorkingDirectory(const std::wstring_view uri) = 0;
        virtual void PlayMidiNote(const int noteNumber, const int velocity, const std::chrono::microseconds duration) = 0;

        virtual bool ResizeWindow(const til::CoordType width, const til::CoordType height) = 0;

        virtual void NotifyBufferRotation(const int delta) = 0;
        virtual void NotifyShellIntegrationMark() = 0;

        virtual void InvokeCompletions(std::wstring_view menuJson, unsigned int replaceLength) = 0;

        virtual void SearchMissingCommand(const std::wstring_view command) = 0;

        virtual void ShowNotification(const std::wstring_view title, const std::wstring_view body) = 0;

        enum class ImageDecodePolicy : uint8_t
        {
            KittyPng,
            Iterm2SingleFrame,
        };

        struct ImageDecodeResult
        {
            enum class Status : uint8_t
            {
                Success,
                Unavailable,
                InvalidData,
                UnsupportedContainer,
                MultipleFrames,
                TooLarge,
            };

            Status status = Status::InvalidData;
            GUID containerFormat{};
            uint32_t frameCount = 0;
            til::size size{};
            std::vector<RGBQUAD> pixels;

            explicit operator bool() const noexcept
            {
                return status == Status::Success;
            }
        };

        // Decodes an encoded raster image into premultiplied BGRA pixels under a
        // protocol-specific container/frame policy. Hosts without a decoder return
        // Unavailable; callers never infer success from partially populated output.
        virtual ImageDecodeResult DecodeImageToBgra(std::span<const uint8_t> data, ImageDecodePolicy policy) noexcept = 0;

        // Some content changes on a clock rather than on input, so the adapter has work
        // to do when nothing is arriving to drive it. It hands the host a routine to run
        // and then reports the deadline it next wants that routine run at; no deadline
        // withdraws the request. Arranging the wakeup is the host's business, because the
        // host owns the render thread: the adapter says when it has work, not when to run.
        virtual void SetTimedContentHandler(std::function<void()> handler) = 0;
        virtual void RequestTimedContentUpdate(const std::optional<std::chrono::steady_clock::time_point> deadline) = 0;
        // Reads up to 'size' bytes of a local file (or to EOF when size == 0) starting at
        // byte 'offset'. The path came off the wire, so the host is expected to be strict
        // about what it will open and to bound the read; til::read_file_as_bytes is the
        // shared implementation of both. 'deleteAfter' asks for the file to be removed
        // once read, which the host honours only for a temporary file whose name contains
        // 'deleteNameMarker' -- the caller names its own files, so the caller supplies the
        // marker. A host that cannot reach the filesystem at all returns read_error.
        virtual til::read_file_result ReadLocalFile(const std::wstring_view path, uint64_t offset, uint64_t size, bool deleteAfter, const std::wstring_view deleteNameMarker, std::vector<uint8_t>& out) noexcept = 0;

        // Copies bytes out of a named shared-memory object. As with a file path, the name
        // came off the wire; Microsoft::Console::Utils::ReadSharedMemory is the shared
        // implementation and documents which names are safe to open.
        virtual Microsoft::Console::Utils::ReadSharedMemoryResult ReadSharedMemory(const std::wstring_view name, uint64_t offset, uint64_t size, std::vector<uint8_t>& out) noexcept = 0;

        // The pixel size of a text cell, for anything that has to lay pixels out against
        // the grid.
        virtual til::size GetCellSize() const noexcept = 0;
    };
}
