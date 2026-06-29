// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Terminal.hpp"
#include "tracing.hpp"

#include "../src/inc/unicode.hpp"

#include <wincodec.h>
#include <wil/com.h>
#include <wil/resource.h>

using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console::Render;
using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::VirtualTerminal;

// Note: Generate GUID using TlgGuid.exe tool
#pragma warning(suppress : 26477) // One of the macros uses 0/NULL. We don't have control to make it nullptr.
TRACELOGGING_DEFINE_PROVIDER(g_hCTerminalCoreProvider,
                             "Microsoft.Terminal.Core",
                             // {103ac8cf-97d2-51aa-b3ba-5ffd5528fa5f}
                             (0x103ac8cf, 0x97d2, 0x51aa, 0xb3, 0xba, 0x5f, 0xfd, 0x55, 0x28, 0xfa, 0x5f),
                             TraceLoggingOptionMicrosoftTelemetry());

void Terminal::ReturnResponse(const std::wstring_view response)
{
    if (_pfnWriteInput && !response.empty())
    {
        _pfnWriteInput(response);
    }
}

bool Terminal::IsConPTY() const noexcept
{
    return false;
}

Microsoft::Console::VirtualTerminal::StateMachine& Terminal::GetStateMachine() noexcept
{
    return *_stateMachine;
}

ITerminalApi::BufferState Terminal::GetBufferAndViewport() noexcept
{
    return { _activeBuffer(), til::rect{ _GetMutableViewport().ToInclusive() }, !_inAltBuffer() };
}

void Terminal::SetViewportPosition(til::point position) noexcept
try
{
    // The viewport is fixed at 0,0 for the alt buffer, so this is a no-op.
    if (!_inAltBuffer())
    {
        const auto bufferSize = _mainBuffer->GetSize().Dimensions();
        const auto viewSize = _GetMutableViewport().Dimensions();

        // Ensure the given position is in bounds.
        position.x = std::clamp(position.x, 0, bufferSize.width - viewSize.width);
        position.y = std::clamp(position.y, 0, bufferSize.height - viewSize.height);

        const auto viewportDelta = position.y - _GetMutableViewport().Origin().y;
        _mutableViewport = Viewport::FromDimensions(position, viewSize);
        _PreserveUserScrollOffset(viewportDelta);
        _NotifyScrollEvent();
    }
}
CATCH_LOG()

void Terminal::SetSystemMode(const Mode mode, const bool enabled) noexcept
{
    _assertLocked();
    _systemMode.set(mode, enabled);
}

bool Terminal::GetSystemMode(const Mode mode) const noexcept
{
    _assertLocked();
    return _systemMode.test(mode);
}

void Terminal::ReturnAnswerback()
{
    ReturnResponse(_answerbackMessage);
}

void Terminal::WarningBell()
{
    _pfnWarningBell();
}

void Terminal::SetWindowTitle(const std::wstring_view title)
{
    _assertLocked();
    if (!_suppressApplicationTitle)
    {
        _title.reset();
        if (!title.empty())
        {
            _title.emplace(title);
        }
        _pfnTitleChanged(GetConsoleTitle());
    }
}

CursorType Terminal::GetUserDefaultCursorStyle() const noexcept
{
    _assertLocked();
    return _defaultCursorShape;
}

bool Terminal::ResizeWindow(const til::CoordType width, const til::CoordType height)
{
    // TODO: This will be needed to support various resizing sequences. See also GH#1860.
    _assertLocked();

    if (width <= 0 || height <= 0 || width > SHRT_MAX || height > SHRT_MAX)
    {
        return false;
    }

    const auto currentDimensions = _GetMutableViewport().Dimensions();

    if (width == currentDimensions.width && height == currentDimensions.height)
    {
        return false;
    }

    if (_pfnWindowSizeChanged)
    {
        _pfnWindowSizeChanged(width, height);
        return true;
    }

    return false;
}

void Terminal::SetCodePage(const unsigned int /*codepage*/) noexcept
{
    // Code pages are dealt with in ConHost, so this isn't needed.
}

void Terminal::ResetCodePage() noexcept
{
    // There is nothing to reset, since the code page never changes.
}

unsigned int Terminal::GetOutputCodePage() const noexcept
{
    // See above. The code page is always UTF-8.
    return CP_UTF8;
}

unsigned int Terminal::GetInputCodePage() const noexcept
{
    // See above. The code page is always UTF-8.
    return CP_UTF8;
}

void Terminal::CopyToClipboard(wil::zwstring_view content)
{
    if (_clipboardOperationsAllowed && _focused)
    {
        _pfnCopyToClipboard(content);
    }
}

// Method Description:
// - Updates the taskbar progress indicator
// Arguments:
// - state: indicates the progress state
// - progress: indicates the progress value
// Return Value:
// - <none>
void Terminal::SetTaskbarProgress(const ::Microsoft::Console::VirtualTerminal::DispatchTypes::TaskbarState state, const size_t progress)
{
    _assertLocked();

    _taskbarState = static_cast<size_t>(state);

    switch (state)
    {
    case DispatchTypes::TaskbarState::Clear:
        // Always set progress to 0 in this case
        _taskbarProgress = 0;
        break;
    case DispatchTypes::TaskbarState::Set:
        // Always set progress to the value given in this case
        _taskbarProgress = progress;
        break;
    case DispatchTypes::TaskbarState::Indeterminate:
        // Leave the progress value unchanged in this case
        break;
    case DispatchTypes::TaskbarState::Error:
    case DispatchTypes::TaskbarState::Paused:
        // In these 2 cases, if the given progress value is 0, then
        // leave the progress value unchanged, unless the current progress
        // value is 0, in which case set it to a 'minimum' value (10 in our case);
        // if the given progress value is greater than 0, then set the progress value
        if (progress == 0)
        {
            if (_taskbarProgress == 0)
            {
                _taskbarProgress = TaskbarMinProgress;
            }
        }
        else
        {
            _taskbarProgress = progress;
        }
        break;
    }

    if (_pfnTaskbarProgressChanged)
    {
        _pfnTaskbarProgressChanged();
    }
}

void Terminal::SetWorkingDirectory(std::wstring_view uri)
{
    _assertLocked();

    static bool logged = false;
    if (!logged)
    {
        TraceLoggingWrite(
            g_hCTerminalCoreProvider,
            "ShellIntegrationWorkingDirSet",
            TraceLoggingDescription("The CWD was set by the client application"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        logged = true;
    }

    _workingDirectory = uri;
}

void Terminal::PlayMidiNote(const int noteNumber, const int velocity, const std::chrono::microseconds duration)
{
    _pfnPlayMidiNote(noteNumber, velocity, duration);
}

void Terminal::UseAlternateScreenBuffer(const TextAttribute& attrs)
{
    _assertLocked();

    // the new alt buffer is exactly the size of the viewport.
    _altBufferSize = _mutableViewport.Dimensions();

    const auto cursorSize = _mainBuffer->GetCursor().GetSize();

    ClearSelection();

    // Create a new alt buffer
    _altBuffer = std::make_unique<TextBuffer>(_altBufferSize,
                                              attrs,
                                              cursorSize,
                                              true,
                                              _mainBuffer->GetRenderer());
    _mainBuffer->SetAsActiveBuffer(false);

    // Copy our cursor state to the new buffer's cursor
    {
        // Update the alt buffer's cursor style, visibility, and position to match our own.
        const auto& myCursor = _mainBuffer->GetCursor();
        auto& tgtCursor = _altBuffer->GetCursor();
        tgtCursor.SetStyle(myCursor.GetSize(), myCursor.GetType());
        tgtCursor.SetIsVisible(myCursor.IsVisible());
        tgtCursor.SetIsBlinking(myCursor.IsBlinking());

        // The new position should match the viewport-relative position of the main buffer.
        auto tgtCursorPos = myCursor.GetPosition();
        tgtCursorPos.y -= _mutableViewport.Top();
        tgtCursor.SetPosition(tgtCursorPos);
    }

    // update all the hyperlinks on the screen
    _updateUrlDetection();

    // GH#3321: Make sure we let the TerminalInput know that we switched
    // buffers. This might affect how we interpret certain mouse events.
    _getTerminalInput().UseAlternateScreenBuffer();

    // Update scrollbars
    _NotifyScrollEvent();

    // redraw the screen
    try
    {
        _activeBuffer().TriggerRedrawAll();
    }
    CATCH_LOG();
}
void Terminal::UseMainScreenBuffer()
{
    // To make UserResize() work as if we're back in the main buffer, we first need to unset
    // _altBuffer, which is used throughout this class as an indicator via _inAltBuffer().
    //
    // We delay destroying the alt buffer instance to get a valid altBuffer->GetCursor() reference below.
    const auto altBuffer = std::exchange(_altBuffer, nullptr);
    if (!altBuffer)
    {
        return;
    }

    ClearSelection();

    _mainBuffer->SetAsActiveBuffer(true);

    if (_deferredResize.has_value())
    {
        LOG_IF_FAILED(UserResize(_deferredResize.value()));
        _deferredResize = std::nullopt;
    }

    // After exiting the alt buffer, the main buffer should adopt the current cursor position and style.
    // This is the equal and opposite effect of what we did in UseAlternateScreenBuffer and matches xterm.
    //
    // We have to do this after the call to UserResize() to ensure that the TextBuffer sizes match up.
    // Otherwise the cursor position may be temporarily out of bounds and some code may choke on that.
    {
        const auto& altCursor = altBuffer->GetCursor();
        auto& mainCursor = _mainBuffer->GetCursor();

        mainCursor.SetStyle(altCursor.GetSize(), altCursor.GetType());
        mainCursor.SetIsVisible(altCursor.IsVisible());
        mainCursor.SetIsBlinking(altCursor.IsBlinking());

        auto tgtCursorPos = altCursor.GetPosition();
        tgtCursorPos.y += _mutableViewport.Top();
        mainCursor.SetPosition(tgtCursorPos);
    }

    // update all the hyperlinks on the screen
    _updateUrlDetection();

    // GH#3321: Make sure we let the TerminalInput know that we switched
    // buffers. This might affect how we interpret certain mouse events.
    _getTerminalInput().UseMainScreenBuffer();

    // Update scrollbars
    _NotifyScrollEvent();

    // redraw the screen
    _activeBuffer().TriggerRedrawAll();
}

// Method Description:
// - Reacts to a client asking us to show or hide the window.
// Arguments:
// - showOrHide - True for show. False for hide.
// Return Value:
// - <none>
void Terminal::ShowWindow(bool showOrHide)
{
    if (_pfnShowWindowChanged)
    {
        _pfnShowWindowChanged(showOrHide);
    }
}

bool Terminal::IsVtInputEnabled() const noexcept
{
    return false;
}

void Terminal::InvokeCompletions(std::wstring_view menuJson, unsigned int replaceLength)
{
    if (_pfnCompletionsChanged)
    {
        _pfnCompletionsChanged(menuJson, replaceLength);
    }
}

void Terminal::SearchMissingCommand(const std::wstring_view command)
{
    if (_pfnSearchMissingCommand)
    {
        const auto bufferRow = _activeBuffer().GetCursor().GetPosition().y;
        _pfnSearchMissingCommand(command, bufferRow);
    }
}

void Terminal::ShowNotification(const std::wstring_view title, const std::wstring_view body)
{
    if (_pfnShowNotification)
    {
        _pfnShowNotification(title, body);
    }
}

// Decodes an encoded image (PNG etc.) into premultiplied BGRA pixels using WIC.
// Returns false on any failure; never throws.
bool Terminal::DecodeImageToBgra(const std::span<const uint8_t> data, std::vector<RGBQUAD>& pixels, til::size& size) noexcept
try
{
    pixels.clear();
    if (data.empty())
    {
        return false;
    }

    // WIC requires COM. It may already be initialized on this thread (in either
    // apartment, both fine for WIC); only balance the call we actually made.
    const auto coInitHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto coUninit = wil::scope_exit([coInitHr]() noexcept {
        if (SUCCEEDED(coInitHr))
        {
            CoUninitialize();
        }
    });

    const auto factory = wil::CoCreateInstance<IWICImagingFactory2>(CLSID_WICImagingFactory2);

    wil::com_ptr<IWICStream> stream;
    THROW_IF_FAILED(factory->CreateStream(stream.addressof()));
    THROW_IF_FAILED(stream->InitializeFromMemory(const_cast<WICInProcPointer>(data.data()), gsl::narrow<DWORD>(data.size())));

    wil::com_ptr<IWICBitmapDecoder> decoder;
    THROW_IF_FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.addressof()));

    // Kitty's f=100 means PNG specifically; reject any other container WIC accepts.
    GUID container{};
    THROW_IF_FAILED(decoder->GetContainerFormat(&container));
    if (container != GUID_ContainerFormatPng)
    {
        return false;
    }

    wil::com_ptr<IWICBitmapFrameDecode> frame;
    THROW_IF_FAILED(decoder->GetFrame(0, frame.addressof()));

    UINT width = 0;
    UINT height = 0;
    THROW_IF_FAILED(frame->GetSize(&width, &height));
    // Bound the decoded size so a small but hostile image can't claim enormous
    // dimensions and force a multi-gigabyte allocation.
    if (width == 0 || height == 0 || static_cast<uint64_t>(width) * height > 64ull * 1024 * 1024)
    {
        return false;
    }

    wil::com_ptr<IWICFormatConverter> converter;
    THROW_IF_FAILED(factory->CreateFormatConverter(converter.addressof()));
    THROW_IF_FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));

    pixels.resize(static_cast<size_t>(width) * height);
    const auto stride = width * static_cast<UINT>(sizeof(RGBQUAD));
    const auto bufferSize = gsl::narrow<UINT>(pixels.size() * sizeof(RGBQUAD));
    THROW_IF_FAILED(converter->CopyPixels(nullptr, stride, bufferSize, reinterpret_cast<BYTE*>(pixels.data())));

    size = { static_cast<til::CoordType>(width), static_cast<til::CoordType>(height) };
    return true;
}
catch (...)
{
    pixels.clear();
    return false;
}

til::size Terminal::GetCellSize() const noexcept
{
    const auto size = _fontInfo.GetSize();
    // Before the renderer reports the real font (via SetFontInfo), _fontInfo is a
    // placeholder whose width is 0. Clamping that to 1 would lay images out on a
    // degenerate 1px-per-cell grid, stretching them ~cell-width times horizontally.
    // Fall back to a sane default cell (matching conhost/Sixel) until the real font
    // is known, so images render at a correct grid rather than badly stretched.
    if (size.width < 2 || size.height < 2)
    {
        return { 10, 20 };
    }
    return size;
}

namespace
{
    // Kitty graphics file-transmission (t=f / t=t) helpers. NOTE: this logic is
    // intentionally duplicated in ConhostInternalGetSet::ReadKittyImageFile
    // (outputStream.cpp); keep the two copies in sync. Reads are bounded to the same
    // 32 MiB cap the adapter applies to a direct (base64) payload, so a hostile S= can
    // never force a large allocation regardless of the file's true size.
    constexpr uint64_t MaxKittyFileBytes = 32ull * 1024 * 1024;

    // Returns the fully-resolved on-disk path for an open handle (resolving symlinks,
    // "..", and 8.3 short names) so the temp-directory containment check cannot be
    // fooled, or an empty string on failure.
    std::wstring _kittyFinalPath(HANDLE handle) noexcept
    {
        const auto needed = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (needed == 0)
        {
            return {};
        }
        std::wstring path;
        try
        {
            path.resize(needed);
        }
        catch (...)
        {
            return {};
        }
        const auto written = GetFinalPathNameByHandleW(handle, path.data(), needed, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (written == 0 || written >= needed)
        {
            return {};
        }
        path.resize(written);
        return path;
    }

    // Returns the canonical system temp directory (resolved through a handle so it is in
    // the same normalized form as _kittyFinalPath) ending in a backslash, or empty on
    // failure. Prefers GetTempPath2W (Win11, per-process) and falls back to GetTempPathW.
    std::wstring _kittyTempDir() noexcept
    {
        wchar_t raw[MAX_PATH + 2]{};
        using PfnGetTempPath2W = DWORD(WINAPI*)(DWORD, LPWSTR);
        static const auto pfnGetTempPath2W = []() noexcept {
            const auto k32 = GetModuleHandleW(L"kernel32.dll");
            return k32 ? reinterpret_cast<PfnGetTempPath2W>(GetProcAddress(k32, "GetTempPath2W")) : nullptr;
        }();
        const auto len = pfnGetTempPath2W ? pfnGetTempPath2W(ARRAYSIZE(raw), raw) : GetTempPathW(ARRAYSIZE(raw), raw);
        if (len == 0 || len >= ARRAYSIZE(raw))
        {
            return {};
        }
        wil::unique_hfile dir{ CreateFileW(raw, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr) };
        if (!dir)
        {
            return {};
        }
        auto canonical = _kittyFinalPath(dir.get());
        if (!canonical.empty() && canonical.back() != L'\\')
        {
            try
            {
                canonical.push_back(L'\\');
            }
            catch (...)
            {
                return {};
            }
        }
        return canonical;
    }

    // True when the canonical file path is contained in the canonical temp directory.
    // Both must be normalized via _kittyFinalPath so a case-insensitive ordinal prefix
    // test is sufficient and safe.
    bool _kittyPathUnderTemp(const std::wstring& canonicalFile) noexcept
    {
        const auto tempDir = _kittyTempDir();
        if (tempDir.empty() || canonicalFile.size() < tempDir.size())
        {
            return false;
        }
        return CompareStringOrdinal(canonicalFile.c_str(), static_cast<int>(tempDir.size()), tempDir.c_str(), static_cast<int>(tempDir.size()), TRUE) == CSTR_EQUAL;
    }

    // Real-filesystem implementation of ITerminalApi::ReadKittyImageFile. Opens 'path',
    // seeks to 'offset', reads up to min(size or EOF, 32 MiB) bytes into 'out', and—when
    // 'deleteAfter' (t=t)—deletes the file afterward ONLY if it resolves to a location
    // under the system temp directory, so the medium cannot delete arbitrary files.
    // Never throws.
    bool _readKittyImageFile(const std::wstring_view path, uint64_t offset, uint64_t size, bool deleteAfter, std::vector<uint8_t>& out) noexcept
    {
        out.clear();
        if (path.empty())
        {
            return false;
        }

        std::wstring pathStr;
        try
        {
            pathStr.assign(path);
        }
        catch (...)
        {
            return false;
        }

        // We only ever read; FILE_SHARE_DELETE lets the file be removed (by us or the
        // client) while the handle is open.
        wil::unique_hfile file{ CreateFileW(pathStr.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (!file)
        {
            return false;
        }

        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(file.get(), &fileSize) || fileSize.QuadPart < 0)
        {
            return false;
        }
        const auto total = static_cast<uint64_t>(fileSize.QuadPart);
        if (offset > total)
        {
            return false; // offset past end of file
        }

        // Bytes to read: what remains after the offset, clamped by the client's S= when
        // nonzero, and always by the hard 32 MiB cap (fits in a single DWORD ReadFile).
        uint64_t toRead = total - offset;
        if (size != 0)
        {
            toRead = std::min(toRead, size);
        }
        toRead = std::min(toRead, MaxKittyFileBytes);

        if (offset != 0)
        {
            LARGE_INTEGER move{};
            move.QuadPart = static_cast<LONGLONG>(offset);
            if (!SetFilePointerEx(file.get(), move, nullptr, FILE_BEGIN))
            {
                return false;
            }
        }

        // Resolve the canonical path while the handle is open so the temp check (and the
        // path we ultimately delete) reflects the real file, not the client's spelling.
        std::wstring canonicalPath;
        if (deleteAfter)
        {
            canonicalPath = _kittyFinalPath(file.get());
        }

        try
        {
            out.resize(static_cast<size_t>(toRead));
        }
        catch (...)
        {
            out.clear();
            return false;
        }

        DWORD read = 0;
        if (toRead != 0 && !ReadFile(file.get(), out.data(), static_cast<DWORD>(toRead), &read, nullptr))
        {
            out.clear();
            return false;
        }
        out.resize(read);

        file.reset(); // close before deleting

        if (deleteAfter && !canonicalPath.empty() && _kittyPathUnderTemp(canonicalPath))
        {
            // Best-effort: a failed delete does not fail the (successful) read. Files
            // outside the temp directory are deliberately left untouched.
            std::ignore = DeleteFileW(canonicalPath.c_str());
        }

        return true;
    }
}

bool Terminal::ReadKittyImageFile(const std::wstring_view path, uint64_t offset, uint64_t size, bool deleteAfter, std::vector<uint8_t>& out) noexcept
{
    return _readKittyImageFile(path, offset, size, deleteAfter, out);
}

void Terminal::NotifyBufferRotation(const int delta)
{
    // Update our selection, so it doesn't move as the buffer is cycled
    if (_selection->active)
    {
        auto selection{ _selection.write() };
        wil::hide_name _selection;
        // If the end of the selection will be out of range after the move, we just
        // clear the selection. Otherwise, we move both the start and end points up
        // by the given delta and clamp to the first row.
        if (selection->end.y < delta)
        {
            selection->active = false;
        }
        else
        {
            // Stash this, so we can make sure to update the pivot to match later.
            const auto pivotWasStart = selection->start == selection->pivot;
            selection->start.y = std::max(selection->start.y - delta, 0);
            selection->end.y = std::max(selection->end.y - delta, 0);
            // Make sure to sync the pivot with whichever value is the right one.
            selection->pivot = pivotWasStart ? selection->start : selection->end;
        }
    }

    // manually erase our pattern intervals since the locations have changed now
    _patternIntervalTree = {};

    const auto oldScrollOffset = _scrollOffset;
    _PreserveUserScrollOffset(delta);
    if (_scrollOffset != oldScrollOffset || AlwaysNotifyOnBufferRotation())
    {
        _NotifyScrollEvent();
    }
}

void Terminal::NotifyShellIntegrationMark()
{
    // Notify the scrollbar that marks have been added so it can refresh the mark indicators
    _NotifyScrollEvent();
}
