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
#include "../../buffer/out/LineRendition.hpp"
#include "../../buffer/out/textBuffer.hpp"
#include "../../renderer/inc/RenderSettings.hpp"

#include <deque>
#include <memory>
#include <span>

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

        // Decodes an encoded image (e.g. PNG) into premultiplied BGRA pixels. Hosts
        // without an image decoder (conhost) leave this unimplemented and return
        // false; the Kitty graphics handler then skips display of that image.
        virtual bool DecodeImageToBgra(const std::span<const uint8_t> /*data*/, std::vector<RGBQUAD>& /*pixels*/, til::size& /*size*/) noexcept
        {
            return false;
        }

        // Reads the contents of an image file for Kitty graphics file/temporary
        // transmission (t=f / t=t). Reads up to 'size' bytes (or to EOF when size==0)
        // starting at byte 'offset' into 'out'; the host bounds the read to a safe
        // maximum so a hostile size cannot force an unbounded allocation. When
        // 'deleteAfter' is true (t=t) the host deletes the file after a successful
        // read ONLY if it resides under the system temporary directory, so the medium
        // cannot be abused to delete arbitrary files. Hosts without file access (e.g.
        // the unit-test mock by default) leave this unimplemented and return false;
        // the caller then reports the transfer as unreadable (EBADF).
        virtual bool ReadKittyImageFile(const std::wstring_view /*path*/, uint64_t /*offset*/, uint64_t /*size*/, bool /*deleteAfter*/, std::vector<uint8_t>& /*out*/) noexcept
        {
            return false;
        }

        // Returns the pixel size of a text cell, used to lay out graphics images.
        // The default is a reasonable fallback; hosts with real font metrics override.
        virtual til::size GetCellSize() const noexcept
        {
            return { 10, 20 };
        }
    };
}
