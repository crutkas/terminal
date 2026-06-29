// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "outputStream.hpp"

#include "_stream.h"
#include "getset.h"
#include "handle.h"
#include "directio.h"
#include "output.h"

#include "../interactivity/inc/ServiceLocator.hpp"

#pragma hdrstop

using namespace Microsoft::Console;
using namespace Microsoft::Console::VirtualTerminal;
using Microsoft::Console::Interactivity::ServiceLocator;

ConhostInternalGetSet::ConhostInternalGetSet(_In_ IIoProvider& io) :
    _io{ io }
{
}

void ConhostInternalGetSet::UnknownSequence() noexcept
{
    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    // VT sequences unknown to us may cause the cursor position to change in a way that
    // we don't know about. In this case, we need to mark the cursor position as "dirty".
    //
    // The worst offender is likely PowerShell. It uses VT sequences but also calls
    // GetConsoleScreenBufferInfoEx for *every single line of output* (!!!). This prevents
    // us from using a more conservative solution (e.g. always fetching the cursor position).
    if (gci.IsInVtIoMode())
    {
        gci.GetActiveOutputBuffer().GetActiveBuffer().SetConptyCursorPositionMayBeWrong();
    }
}

// - Sends a string response to the input stream of the console.
// - Used by various commands where the program attached would like a reply to one of the commands issued.
// - This will generate two "key presses" (one down, one up) for every character in the string and place them into the head of the console's input stream.
// Arguments:
// - response - The response string to transmit back to the input stream
// Return Value:
// - <none>
void ConhostInternalGetSet::ReturnResponse(const std::wstring_view response)
{
    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    // ConPTY should not respond to requests. That's the job of the terminal.
    if (gci.IsInVtIoMode())
    {
        return;
    }

    // TODO GH#4954 During the input refactor we may want to add a "priority" input list
    // to make sure that "response" input is spooled directly into the application.
    // We switched this to an append (vs. a prepend) to fix GH#1637, a bug where two CPR
    // could collide with each other.
    _io.GetActiveInputBuffer()->WriteString(response);
}

bool ConhostInternalGetSet::IsConPTY() const noexcept
{
    const auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    return gci.IsInVtIoMode();
}

// Routine Description:
// - Retrieves the state machine for the active output buffer.
// Arguments:
// - <none>
// Return Value:
// - a reference to the StateMachine instance.
StateMachine& ConhostInternalGetSet::GetStateMachine()
{
    return _io.GetActiveOutputBuffer().GetStateMachine();
}

// Routine Description:
// - Retrieves the text buffer and virtual viewport for the active output
//   buffer. Also returns a flag indicating whether it's the main buffer.
// Arguments:
// - <none>
// Return Value:
// - a tuple with the buffer reference, viewport, and main buffer flag.
ITerminalApi::BufferState ConhostInternalGetSet::GetBufferAndViewport()
{
    auto& info = _io.GetActiveOutputBuffer();
    return { info.GetTextBuffer(), info.GetVirtualViewport().ToExclusive(), info.Next == nullptr };
}

// Routine Description:
// - Sets the position of the window viewport.
// Arguments:
// - position - the new position of the viewport.
// Return Value:
// - <none>
void ConhostInternalGetSet::SetViewportPosition(const til::point position)
{
    auto& info = _io.GetActiveOutputBuffer();
    THROW_IF_FAILED(info.SetViewportOrigin(true, position, true));
    // SetViewportOrigin() only updates the virtual bottom (the bottom coordinate of the area
    // in the text buffer a VT client writes its output into) when it's moving downwards.
    // But this function is meant to truly move the viewport no matter what. Otherwise, `tput reset` breaks.
    info.UpdateBottom();
}

// Routine Description:
// - Sets the state of one of the system modes.
// Arguments:
// - mode - The mode being updated.
// - enabled - True to enable the mode, false to disable it.
// Return Value:
// - <none>
void ConhostInternalGetSet::SetSystemMode(const Mode mode, const bool enabled)
{
    switch (mode)
    {
    case Mode::AutoWrap:
        WI_UpdateFlag(_io.GetActiveOutputBuffer().OutputMode, ENABLE_WRAP_AT_EOL_OUTPUT, enabled);
        break;
    case Mode::LineFeed:
        WI_UpdateFlag(_io.GetActiveOutputBuffer().OutputMode, DISABLE_NEWLINE_AUTO_RETURN, !enabled);
        break;
    case Mode::BracketedPaste:
        ServiceLocator::LocateGlobals().getConsoleInformation().SetBracketedPasteMode(enabled);
        break;
    default:
        THROW_HR(E_INVALIDARG);
    }
}

// Routine Description:
// - Retrieves the current state of one of the system modes.
// Arguments:
// - mode - The mode being queried.
// Return Value:
// - true if the mode is enabled. false otherwise.
bool ConhostInternalGetSet::GetSystemMode(const Mode mode) const
{
    switch (mode)
    {
    case Mode::AutoWrap:
        return WI_IsFlagSet(_io.GetActiveOutputBuffer().OutputMode, ENABLE_WRAP_AT_EOL_OUTPUT);
    case Mode::LineFeed:
        return WI_IsFlagClear(_io.GetActiveOutputBuffer().OutputMode, DISABLE_NEWLINE_AUTO_RETURN);
    case Mode::BracketedPaste:
        return ServiceLocator::LocateGlobals().getConsoleInformation().GetBracketedPasteMode();
    default:
        THROW_HR(E_INVALIDARG);
    }
}

// Routine Description:
// - Sends the configured answerback message in response to an ENQ query.
// Return Value:
// - <none>
void ConhostInternalGetSet::ReturnAnswerback()
{
    const auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    ReturnResponse(gci.GetAnswerbackMessage());
}

// Routine Description:
// - Sends a notify message to play the "SystemHand" sound event.
// Return Value:
// - <none>
void ConhostInternalGetSet::WarningBell()
{
    _io.GetActiveOutputBuffer().SendNotifyBeep();
}

// Routine Description:
// - Sets the title of the console window.
// Arguments:
// - title - The string to set as the window title
// Return Value:
// - <none>
void ConhostInternalGetSet::SetWindowTitle(std::wstring_view title)
{
    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    gci.SetTitle(title.empty() ? gci.GetOriginalTitle() : title);
}

// Routine Description:
// - Swaps to the alternate screen buffer. In virtual terminals, there exists both a "main"
//     screen buffer and an alternate. This creates a new alternate, and switches to it.
//     If there is an already existing alternate, it is discarded.
// Arguments:
// - attrs - the attributes for initializing the buffer.
// Return Value:
// - <none>
void ConhostInternalGetSet::UseAlternateScreenBuffer(const TextAttribute& attrs)
{
    THROW_IF_NTSTATUS_FAILED(_io.GetActiveOutputBuffer().UseAlternateScreenBuffer(attrs));
}

// Routine Description:
// - Swaps to the main screen buffer. From the alternate buffer, returns to the main screen
//     buffer. From the main screen buffer, does nothing. The alternate is discarded.
// Return Value:
// - <none>
void ConhostInternalGetSet::UseMainScreenBuffer()
{
    _io.GetActiveOutputBuffer().UseMainScreenBuffer();
}

// Method Description:
// - Retrieves the current user default cursor style.
// Arguments:
// - <none>
// Return Value:
// - the default cursor style.
CursorType ConhostInternalGetSet::GetUserDefaultCursorStyle() const
{
    const auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    return gci.GetCursorType();
}

// Routine Description:
// - Shows or hides the active window when asked.
// Arguments:
// - showOrHide - True for show, False for hide. Matching WM_SHOWWINDOW lParam.
// Return Value:
// - <none>
void ConhostInternalGetSet::ShowWindow(bool showOrHide)
{
    // ConPTY is supposed to be "transparent" to the VT application. Any VT it processes is given to the terminal.
    // As such, it must not react to this "CSI 1 t" or "CSI 2 t" sequence. That's the job of the terminal.
    // If the terminal encounters such a sequence, it can show/hide itself and let ConPTY know via its signal API.
    const auto window = ServiceLocator::LocateConsoleWindow();
    if (!window)
    {
        return;
    }

    // GH#13301 - When we send this ShowWindow message, if we send it to the
    // conhost HWND, it's going to need to get processed by the window message
    // thread before returning.
    // However, ShowWindowAsync doesn't have this problem. It'll post the
    // message to the window thread, then immediately return, so we don't have
    // to worry about deadlocking.
    const auto hwnd = window->GetWindowHandle();
    ::ShowWindowAsync(hwnd, showOrHide ? SW_SHOWNOACTIVATE : SW_MINIMIZE);
}

// Routine Description:
// - Set the code page used for translating text when calling A versions of I/O functions.
// Arguments:
// - codepage - the new code page of the console.
// Return Value:
// - <none>
void ConhostInternalGetSet::SetCodePage(const unsigned int codepage)
{
    LOG_IF_FAILED(DoSrvSetConsoleOutputCodePage(codepage));
    LOG_IF_FAILED(DoSrvSetConsoleInputCodePage(codepage));
}

// Routine Description:
// - Reset the code pages to their default values.
// Arguments:
// - <none>
// Return Value:
// - <none>
void ConhostInternalGetSet::ResetCodePage()
{
    const auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    LOG_IF_FAILED(DoSrvSetConsoleOutputCodePage(gci.DefaultOutputCP));
    LOG_IF_FAILED(DoSrvSetConsoleInputCodePage(gci.DefaultCP));
}

// Routine Description:
// - Gets the code page used for translating text when calling A versions of output functions.
// Arguments:
// - <none>
// Return Value:
// - the output code page of the console.
unsigned int ConhostInternalGetSet::GetOutputCodePage() const
{
    return ServiceLocator::LocateGlobals().getConsoleInformation().OutputCP;
}

// Routine Description:
// - Gets the code page used for translating text when calling A versions of input functions.
// Arguments:
// - <none>
// Return Value:
// - the input code page of the console.
unsigned int ConhostInternalGetSet::GetInputCodePage() const
{
    return ServiceLocator::LocateGlobals().getConsoleInformation().CP;
}

// Routine Description:
// - Copies the given content to the clipboard.
// Arguments:
// - content - the text to be copied.
// Return Value:
// - <none>
void ConhostInternalGetSet::CopyToClipboard(const wil::zwstring_view content)
{
    auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();

    // Only allow VT clipboard writes when the console has focus
    if (WI_IsFlagSet(gci.Flags, CONSOLE_HAS_FOCUS))
    {
        gci.CopyTextToClipboard(content);
    }
}

// Routine Description:
// - Updates the taskbar progress indicator.
// Arguments:
// - state: indicates the progress state
// - progress: indicates the progress value
// Return Value:
// - <none>
void ConhostInternalGetSet::SetTaskbarProgress(const DispatchTypes::TaskbarState /*state*/, const size_t /*progress*/)
{
    // TODO
}

// Routine Description:
// - Set the active working directory. Not used in conhost.
// Arguments:
// - content - the text to be copied.
// Return Value:
// - <none>
void ConhostInternalGetSet::SetWorkingDirectory(const std::wstring_view /*uri*/)
{
}

// Routine Description:
// - Plays a single MIDI note, blocking for the duration.
// Arguments:
// - noteNumber - The MIDI note number to be played (0 - 127).
// - velocity - The force with which the note should be played (0 - 127).
// - duration - How long the note should be sustained (in milliseconds).
// Return value:
// - true if successful. false otherwise.
void ConhostInternalGetSet::PlayMidiNote(const int noteNumber, const int velocity, const std::chrono::microseconds duration)
{
    const auto window = ServiceLocator::LocateConsoleWindow();
    if (!window)
    {
        return;
    }

    // Unlock the console, so the UI doesn't hang while we're busy.
    UnlockConsole();

    // This call will block for the duration, unless shutdown early.
    const auto windowHandle = window->GetWindowHandle();
    auto& midiAudio = ServiceLocator::LocateGlobals().getConsoleInformation().GetMidiAudio();
    midiAudio.PlayNote(windowHandle, noteNumber, velocity, std::chrono::duration_cast<std::chrono::milliseconds>(duration));

    LockConsole();
}

// Routine Description:
// - Resizes the window to the specified dimensions, in characters.
// Arguments:
// - width: The new width of the window, in columns
// - height: The new height of the window, in rows
// Return Value:
// - True if handled successfully. False otherwise.
bool ConhostInternalGetSet::ResizeWindow(const til::CoordType sColumns, const til::CoordType sRows)
{
    // Ensure we can safely use gsl::narrow_cast<short>(...).
    if (sColumns <= 0 || sRows <= 0 || sColumns > SHRT_MAX || sRows > SHRT_MAX)
    {
        return false;
    }

    auto api = ServiceLocator::LocateGlobals().api;
    auto& screenInfo = _io.GetActiveOutputBuffer();

    // We need to save the attributes separately, since the wAttributes field in
    // CONSOLE_SCREEN_BUFFER_INFOEX is not capable of representing the extended
    // attribute values, and can end up corrupting that data when restored.
    const auto attributes = screenInfo.GetTextBuffer().GetCurrentAttributes();
    const auto restoreAttributes = wil::scope_exit([&] {
        screenInfo.GetTextBuffer().SetCurrentAttributes(attributes);
    });

    CONSOLE_SCREEN_BUFFER_INFOEX csbiex = { 0 };
    csbiex.cbSize = sizeof(CONSOLE_SCREEN_BUFFER_INFOEX);
    api->GetConsoleScreenBufferInfoExImpl(screenInfo, csbiex);

    const auto oldViewport = screenInfo.GetVirtualViewport();
    auto newViewport = Viewport::FromDimensions(oldViewport.Origin(), { sColumns, sRows });
    // Always resize the width of the console
    csbiex.dwSize.X = gsl::narrow_cast<short>(sColumns);
    // Only set the screen buffer's height if it's currently less than
    //  what we're requesting.
    if (sRows > csbiex.dwSize.Y)
    {
        csbiex.dwSize.Y = gsl::narrow_cast<short>(sRows);
    }

    // If the cursor row is now past the bottom of the viewport, we'll have to
    // move the viewport down to bring it back into view.
    const auto cursorOverflow = csbiex.dwCursorPosition.Y - newViewport.BottomInclusive();
    if (cursorOverflow > 0)
    {
        newViewport = Viewport::Offset(newViewport, { 0, cursorOverflow });
    }

    // SetWindowInfo expect inclusive rects
    const auto sri = newViewport.ToInclusive();

    // SetConsoleScreenBufferInfoEx however expects exclusive rects
    const auto sre = newViewport.ToExclusive();
    csbiex.srWindow = til::unwrap_exclusive_small_rect(sre);

    THROW_IF_FAILED(api->SetConsoleScreenBufferInfoExImpl(screenInfo, csbiex));
    THROW_IF_FAILED(api->SetConsoleWindowInfoImpl(screenInfo, true, sri));
    return true;
}

// Routine Description:
// - Checks if the InputBuffer is willing to accept VT Input directly
//   IsVtInputEnabled is an internal-only "API" call that the vt commands can execute,
//    but it is not represented as a function call on our public API surface.
// Arguments:
// - <none>
// Return value:
// - true if enabled (see IsInVirtualTerminalInputMode). false otherwise.
bool ConhostInternalGetSet::IsVtInputEnabled() const
{
    return _io.GetActiveInputBuffer()->IsInVirtualTerminalInputMode();
}

// Routine Description:
// - Implements conhost-specific behavior when the buffer is rotated.
// Arguments:
// - delta - the number of cycles that the buffer has rotated.
// Return value:
// - <none>
void ConhostInternalGetSet::NotifyBufferRotation(const int)
{
}

void ConhostInternalGetSet::NotifyShellIntegrationMark()
{
    // Not implemented for conhost - shell integration marks are a Terminal app feature.
}

void ConhostInternalGetSet::InvokeCompletions(std::wstring_view /*menuJson*/, unsigned int /*replaceLength*/)
{
    // Not implemented for conhost.
}
void ConhostInternalGetSet::SearchMissingCommand(std::wstring_view /*missingCommand*/)
{
    // Not implemented for conhost.
}
void ConhostInternalGetSet::ShowNotification(std::wstring_view /*title*/, std::wstring_view /*body*/)
{
    // Not implemented for conhost.
}

namespace
{
    // Kitty graphics file-transmission (t=f / t=t) helpers. NOTE: this logic is
    // intentionally duplicated in Terminal::ReadKittyImageFile (TerminalApi.cpp);
    // keep the two copies in sync. Reads are bounded to the same 32 MiB cap the
    // adapter applies to a direct (base64) payload, so a hostile S= can never force
    // a large allocation regardless of the file's true size.
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

bool ConhostInternalGetSet::ReadKittyImageFile(const std::wstring_view path, uint64_t offset, uint64_t size, bool deleteAfter, std::vector<uint8_t>& out) noexcept
{
    return _readKittyImageFile(path, offset, size, deleteAfter, out);
}
