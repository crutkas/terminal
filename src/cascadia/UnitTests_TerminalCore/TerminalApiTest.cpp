// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include <WexTestClass.h>

#include "../cascadia/TerminalCore/Terminal.hpp"
#include "MockTermSettings.h"
#include "../renderer/inc/DummyRenderer.hpp"
#include "consoletaeftemplates.hpp"

#include <wil/resource.h>

using namespace winrt::Microsoft::Terminal::Core;
using namespace Microsoft::Terminal::Core;

using namespace WEX::Logging;
using namespace WEX::TestExecution;

namespace TerminalCoreUnitTests
{
#define WCS(x) WCSHELPER(x)
#define WCSHELPER(x) L## #x

    class TerminalApiTest
    {
        TEST_CLASS(TerminalApiTest);

        TEST_METHOD(SetColorTableEntry);

        TEST_METHOD(CursorVisibilityViaStateMachine);

        // Terminal::_WriteBuffer used to enter infinite loops under certain conditions.
        // This test ensures that Terminal::_WriteBuffer doesn't get stuck when
        // PrintString() is called with more code units than the buffer width.
        TEST_METHOD(PrintStringOfSurrogatePairs);

        TEST_METHOD(AddHyperlink);
        TEST_METHOD(AddHyperlinkCustomId);
        TEST_METHOD(AddHyperlinkCustomIdDifferentUri);

        TEST_METHOD(SetTaskbarProgress);
        TEST_METHOD(SetWorkingDirectory);

        TEST_METHOD(GetCellSizeFallsBackWhenFontUnset);

        // Kitty graphics file-transmission host I/O (t=f / t=t). These exercise the real
        // filesystem path of Terminal::ReadKittyImageFile (the shared til::read_image_file
        // helper), including the security gates that the adapter-level mock cannot cover.
        TEST_METHOD(ReadKittyImageFileReadsFromTemp);
        TEST_METHOD(ReadKittyImageFileDeletesTempWithMarker);
        TEST_METHOD(ReadKittyImageFileOutsideTempNotDeleted);
        TEST_METHOD(ReadKittyImageFileNoMarkerNotDeleted);
        TEST_METHOD(ReadKittyImageFileOffsetAndSizeSlice);
        TEST_METHOD(ReadKittyImageFileOversizeSizeClampsToEof);
        TEST_METHOD(ReadKittyImageFileRejectsUncPath);
        TEST_METHOD(ReadKittyImageFileRejectsRelativePath);
        TEST_METHOD(ReadKittyImageFileNonexistentFails);
        TEST_METHOD(ReadKittyImageFileRejectsCharDevice);
        TEST_METHOD(ReadKittyImageFileRejectsDirectory);
        TEST_METHOD(ReadKittyImageFileCapsAtMaxBytes);
        TEST_METHOD(ReadKittyImageFileFailedReadKeepsTempFile);

        // Kitty graphics shared-memory host I/O (t=s).
        TEST_METHOD(ReadKittySharedMemoryCopiesSliceAndCloses);
        TEST_METHOD(ReadKittySharedMemoryCapsAtMaxBytes);
        TEST_METHOD(ReadKittySharedMemoryRejectsUnsafeNames);
        TEST_METHOD(ReadKittySharedMemoryMissingFails);
        TEST_METHOD(ReadKittySharedMemoryRejectsOffsetPastEnd);
    };
};

using namespace TerminalCoreUnitTests;

void TerminalApiTest::SetColorTableEntry()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto settings = winrt::make<MockTermSettings>(100, 100, 100);
    term.UpdateSettings(settings);

    VERIFY_NO_THROW(term._renderSettings.SetColorTableEntry(0, 100));
    VERIFY_NO_THROW(term._renderSettings.SetColorTableEntry(128, 100));
    VERIFY_NO_THROW(term._renderSettings.SetColorTableEntry(255, 100));

    VERIFY_THROWS(term._renderSettings.SetColorTableEntry(512, 100), std::exception);
}

// Terminal::_WriteBuffer used to enter infinite loops under certain conditions.
// This test ensures that Terminal::_WriteBuffer doesn't get stuck when
// PrintString() is called with more code units than the buffer width.
void TerminalApiTest::PrintStringOfSurrogatePairs()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 3, renderer);

    std::wstring text;
    text.reserve(600);

    for (size_t i = 0; i < 100; ++i)
    {
        text.append(L"𐐌𐐜𐐬");
    }

    struct Baton
    {
        HANDLE done;
        std::wstring text;
        Terminal* pTerm;
    } baton{
        CreateEventW(nullptr, TRUE, FALSE, L"done signal"),
        text,
        &term,
    };

    Log::Comment(L"Launching thread to write data.");
    const auto thread = CreateThread(
        nullptr,
        0,
        [](LPVOID data) -> DWORD {
            const auto& baton = *reinterpret_cast<Baton*>(data);
            Log::Comment(L"Writing data.");
            baton.pTerm->_stateMachine->ProcessString(baton.text);
            Log::Comment(L"Setting event.");
            SetEvent(baton.done);
            return 0;
        },
        (LPVOID)&baton,
        0,
        nullptr);

    Log::Comment(L"Waiting for the write.");
    switch (WaitForSingleObject(baton.done, 2000))
    {
    case WAIT_OBJECT_0:
        Log::Comment(L"Didn't get stuck. Success.");
        break;
    case WAIT_TIMEOUT:
        Log::Comment(L"Wait timed out. It got stuck.");
        Log::Result(WEX::Logging::TestResults::Failed);
        break;
    case WAIT_FAILED:
        Log::Comment(L"Wait failed for some reason. We didn't expect this.");
        Log::Result(WEX::Logging::TestResults::Failed);
        break;
    default:
        Log::Comment(L"Wait return code that no one expected. Fail.");
        Log::Result(WEX::Logging::TestResults::Failed);
        break;
    }

    TerminateThread(thread, 0);
    CloseHandle(baton.done);
    return;
}

void TerminalApiTest::CursorVisibilityViaStateMachine()
{
    // This is a nearly literal copy-paste of ScreenBufferTests::TestCursorIsOn, adapted for the Terminal
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& tbi = *(term._mainBuffer);
    auto& stateMachine = *(term._stateMachine);
    auto& cursor = tbi.GetCursor();

    stateMachine.ProcessString(L"Hello World");
    VERIFY_IS_TRUE(cursor.IsBlinking());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?12l");
    VERIFY_IS_FALSE(cursor.IsBlinking());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?12h");
    VERIFY_IS_TRUE(cursor.IsBlinking());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?12l");
    VERIFY_IS_FALSE(cursor.IsBlinking());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?12h");
    VERIFY_IS_TRUE(cursor.IsBlinking());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?25l");
    VERIFY_IS_TRUE(cursor.IsBlinking());
    VERIFY_IS_FALSE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?25h");
    VERIFY_IS_TRUE(cursor.IsBlinking());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?12;25l");
    VERIFY_IS_FALSE(cursor.IsBlinking());
    VERIFY_IS_FALSE(cursor.IsVisible());
}

void TerminalCoreUnitTests::TerminalApiTest::AddHyperlink()
{
    // This is a nearly literal copy-paste of ScreenBufferTests::TestAddHyperlink, adapted for the Terminal

    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& tbi = *(term._mainBuffer);
    auto& stateMachine = *(term._stateMachine);

    // Process the opening osc 8 sequence
    stateMachine.ProcessString(L"\x1b]8;;test.url\x1b\\");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"test.url");

    // Send any other text
    stateMachine.ProcessString(L"Hello World");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"test.url");

    // Process the closing osc 8 sequences
    stateMachine.ProcessString(L"\x1b]8;;\x1b\\");
    VERIFY_IS_FALSE(tbi.GetCurrentAttributes().IsHyperlink());
}

void TerminalCoreUnitTests::TerminalApiTest::AddHyperlinkCustomId()
{
    // This is a nearly literal copy-paste of ScreenBufferTests::TestAddHyperlinkCustomId, adapted for the Terminal

    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& tbi = *(term._mainBuffer);
    auto& stateMachine = *(term._stateMachine);

    // Process the opening osc 8 sequence
    stateMachine.ProcessString(L"\x1b]8;id=myId;test.url\x1b\\");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"test.url");
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkId(L"test.url", L"myId"), tbi.GetCurrentAttributes().GetHyperlinkId());

    // Send any other text
    stateMachine.ProcessString(L"Hello World");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"test.url");
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkId(L"test.url", L"myId"), tbi.GetCurrentAttributes().GetHyperlinkId());

    // Process the closing osc 8 sequences
    stateMachine.ProcessString(L"\x1b]8;;\x1b\\");
    VERIFY_IS_FALSE(tbi.GetCurrentAttributes().IsHyperlink());
}

void TerminalCoreUnitTests::TerminalApiTest::AddHyperlinkCustomIdDifferentUri()
{
    // This is a nearly literal copy-paste of ScreenBufferTests::TestAddHyperlinkCustomId, adapted for the Terminal

    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& tbi = *(term._mainBuffer);
    auto& stateMachine = *(term._stateMachine);

    // Process the opening osc 8 sequence
    stateMachine.ProcessString(L"\x1b]8;id=myId;test.url\x1b\\");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"test.url");
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkId(L"test.url", L"myId"), tbi.GetCurrentAttributes().GetHyperlinkId());

    const auto oldAttributes{ tbi.GetCurrentAttributes() };

    // Send any other text
    stateMachine.ProcessString(L"\x1b]8;id=myId;other.url\x1b\\");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"other.url");
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkId(L"other.url", L"myId"), tbi.GetCurrentAttributes().GetHyperlinkId());

    // This second URL should not change the URL of the original ID!
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(oldAttributes.GetHyperlinkId()), L"test.url");
    VERIFY_ARE_NOT_EQUAL(oldAttributes.GetHyperlinkId(), tbi.GetCurrentAttributes().GetHyperlinkId());
}

void TerminalCoreUnitTests::TerminalApiTest::SetTaskbarProgress()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& stateMachine = *(term._stateMachine);

    // Initial values for taskbar state and progress should be 0
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(0));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));

    // Set some values for taskbar state and progress through state machine
    stateMachine.ProcessString(L"\x1b]9;4;1;50\x1b\\");
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(1));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(50));

    // Reset to 0
    stateMachine.ProcessString(L"\x1b]9;4;0;0\x1b\\");
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(0));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));

    // Set an out of bounds value for state
    stateMachine.ProcessString(L"\x1b]9;4;5;50\x1b\\");
    // Nothing should have changed (dispatch should have returned false)
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(0));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));

    // Set an out of bounds value for progress
    stateMachine.ProcessString(L"\x1b]9;4;1;999\x1b\\");
    // Progress should have been clamped to 100
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(1));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(100));

    // Don't specify any params
    stateMachine.ProcessString(L"\x1b]9;4\x1b\\");
    // State and progress should both be reset to 0
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(0));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));

    // Specify additional params
    stateMachine.ProcessString(L"\x1b]9;4;1;80;123\x1b\\");
    // Additional params should be ignored, state and progress still set normally
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(1));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(80));

    // Edge cases + trailing semicolon testing
    stateMachine.ProcessString(L"\x1b]9;4;2;\x1b\\");
    // String should be processed correctly despite the trailing semicolon,
    // taskbar progress should remain unchanged from previous value
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(2));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(80));

    stateMachine.ProcessString(L"\x1b]9;4;3;75\x1b\\");
    // Given progress value should be ignored because this is the indeterminate state,
    // so the progress value should remain unchanged
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(3));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(80));

    stateMachine.ProcessString(L"\x1b]9;4;0;50\x1b\\");
    // Taskbar progress should be 0 (the given value should be ignored)
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(0));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));

    stateMachine.ProcessString(L"\x1b]9;4;2;\x1b\\");
    // String should be processed correctly despite the trailing semicolon,
    // taskbar progress should be set to a 'minimum', non-zero value
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(2));
    VERIFY_IS_GREATER_THAN(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));
}

void TerminalCoreUnitTests::TerminalApiTest::SetWorkingDirectory()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& stateMachine = *(term._stateMachine);

    // Test setting working directory using OSC 9;9
    // Initial CWD should be empty
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    // Invalid sequences should not change CWD
    stateMachine.ProcessString(L"\x1b]9;9\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9\"C:\\\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b"
                               LR"(]9;9;"C:\invalid path "with" quotes")"
                               L"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    // These OSC 9;9 sequences will result in invalid CWD. It should end up empty, like above.
    stateMachine.ProcessString(L"\x1b]9;9;\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9;\"\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9;\"\"\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9;\"\"\"\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9;No quotes \"until\" later\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    // Valid sequences should change CWD
    stateMachine.ProcessString(L"\x1b]9;9;\"C:\\\"\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"C:\\");

    stateMachine.ProcessString(L"\x1b]9;9;\"C:\\Program Files\"\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"C:\\Program Files");

    stateMachine.ProcessString(L"\x1b]9;9;\"D:\\中文\"\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"D:\\中文");

    // Test OSC 9;9 sequences without quotation marks
    stateMachine.ProcessString(L"\x1b]9;9;C:\\\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"C:\\");

    stateMachine.ProcessString(L"\x1b]9;9;C:\\Program Files\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"C:\\Program Files");

    stateMachine.ProcessString(L"\x1b]9;9;D:\\中文\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"D:\\中文");
}

void TerminalApiTest::GetCellSizeFallsBackWhenFontUnset()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    // Before the renderer reports the real font, _fontInfo is a placeholder whose
    // width is 0. GetCellSize must fall back to a real grid, not a degenerate 1px
    // cell that would stretch Kitty/Sixel images ~cell-width times horizontally.
    const auto cellSize = term.GetCellSize();
    VERIFY_IS_GREATER_THAN(cellSize.width, 1, L"cell width must not be a degenerate 1px");
    VERIFY_IS_GREATER_THAN(cellSize.height, 1, L"cell height must not be a degenerate 1px");
}

// --- Kitty graphics file-transmission host I/O ------------------------------------
//
// These build a Terminal in TestDummyMarker mode and call ReadKittyImageFile against
// real files, validating the security-critical behavior the adapter-level mock cannot:
// local fixed-drive enforcement, temp-dir + "tty-graphics-protocol" marker gated
// deletion via the open handle, offset/size slicing, and the 32 MiB read bound.
namespace
{
    // The system temp directory (ends with a backslash), or empty on failure.
    std::wstring KittyTempDir()
    {
        wchar_t buf[MAX_PATH + 1]{};
        const auto len = GetTempPathW(ARRAYSIZE(buf), buf);
        if (len == 0 || len >= ARRAYSIZE(buf))
        {
            return {};
        }
        return std::wstring{ buf, len };
    }

    // The process's current directory (ends with a backslash); used as a location that
    // is reliably NOT under the system temp directory.
    std::wstring KittyCurrentDir()
    {
        wchar_t buf[MAX_PATH + 1]{};
        const auto len = GetCurrentDirectoryW(ARRAYSIZE(buf), buf);
        if (len == 0 || len >= ARRAYSIZE(buf))
        {
            return {};
        }
        std::wstring dir{ buf, len };
        if (!dir.empty() && dir.back() != L'\\')
        {
            dir.push_back(L'\\');
        }
        return dir;
    }

    std::wstring KittyUniquePath(const std::wstring& dir, const std::wstring& namePart)
    {
        static unsigned int counter = 0;
        return dir + namePart + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(counter++) + L".bin";
    }

    bool KittyWriteAllBytes(const std::wstring& path, const std::vector<uint8_t>& bytes)
    {
        wil::unique_hfile file{ CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
        if (!file)
        {
            return false;
        }
        if (bytes.empty())
        {
            return true;
        }
        DWORD written = 0;
        return WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    }

    bool KittyFileExists(const std::wstring& path)
    {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    std::wstring KittyUniqueMappingName()
    {
        static unsigned int counter = 0;
        return L"Local\\tty-graphics-protocol-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(counter++);
    }

    wil::unique_handle KittyCreateMapping(const std::wstring& name, const uint64_t size, const std::span<const uint8_t> content = {})
    {
        wil::unique_handle mapping{ CreateFileMappingW(INVALID_HANDLE_VALUE,
                                                       nullptr,
                                                       PAGE_READWRITE,
                                                       static_cast<DWORD>(size >> 32),
                                                       static_cast<DWORD>(size),
                                                       name.c_str()) };
        if (!mapping || content.empty())
        {
            return mapping;
        }

        const auto view = MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, content.size());
        if (!view)
        {
            return {};
        }
        memcpy(view, content.data(), content.size());
        UnmapViewOfFile(view);
        return mapping;
    }
}

void TerminalApiTest::ReadKittyImageFileReadsFromTemp()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty(), L"need a usable system temp directory");
    const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-read-");
    const std::vector<uint8_t> content{ 1, 2, 3, 4, 5 };
    VERIFY_IS_TRUE(KittyWriteAllBytes(path, content), L"failed to create the test file");

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_image_result::ok == term.ReadKittyImageFile(path, 0, 0, false, out), L"a readable local temp file must succeed");
    VERIFY_ARE_EQUAL(content.size(), out.size());
    VERIFY_IS_TRUE(out == content, L"the bytes read must equal the file contents");
    VERIFY_IS_TRUE(KittyFileExists(path), L"deleteAfter=false must not remove the file");

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadKittyImageFileDeletesTempWithMarker()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-del-");
    VERIFY_IS_TRUE(KittyWriteAllBytes(path, { 9, 8, 7 }));

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_image_result::ok == term.ReadKittyImageFile(path, 0, 0, true, out), L"the read should succeed");
    VERIFY_ARE_EQUAL(static_cast<size_t>(3), out.size());
    VERIFY_IS_FALSE(KittyFileExists(path), L"a temp file carrying the kitty marker must be deleted after a t=t read");

    if (KittyFileExists(path))
    {
        DeleteFileW(path.c_str());
    }
}

void TerminalApiTest::ReadKittyImageFileOutsideTempNotDeleted()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    // The current directory (the test's output folder) is a local, writable location
    // that is not under the system temp directory.
    const auto dir = KittyCurrentDir();
    VERIFY_IS_FALSE(dir.empty());
    const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-outside-");
    VERIFY_IS_TRUE(KittyWriteAllBytes(path, { 4, 5, 6 }));

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_image_result::ok == term.ReadKittyImageFile(path, 0, 0, true, out), L"the read should still succeed");
    VERIFY_IS_TRUE(KittyFileExists(path), L"a file OUTSIDE the temp dir must never be deleted, even with deleteAfter (anti arbitrary-delete)");

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadKittyImageFileNoMarkerNotDeleted()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    // Under temp, but the name lacks the "tty-graphics-protocol" marker.
    const auto path = KittyUniquePath(dir, L"kitty-no-marker-");
    VERIFY_IS_TRUE(KittyWriteAllBytes(path, { 1, 1, 1 }));

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_image_result::ok == term.ReadKittyImageFile(path, 0, 0, true, out), L"the read should succeed");
    VERIFY_IS_TRUE(KittyFileExists(path), L"a temp file WITHOUT the kitty marker must not be auto-deleted");

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadKittyImageFileOffsetAndSizeSlice()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-slice-");
    const std::vector<uint8_t> content{ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H' };
    VERIFY_IS_TRUE(KittyWriteAllBytes(path, content));

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_image_result::ok == term.ReadKittyImageFile(path, 2, 3, false, out), L"offset+size read should succeed");
    const std::vector<uint8_t> expected{ 'C', 'D', 'E' };
    VERIFY_ARE_EQUAL(expected.size(), out.size());
    VERIFY_IS_TRUE(out == expected, L"O=2,S=3 must read exactly bytes [2,5) of the file");

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadKittyImageFileOversizeSizeClampsToEof()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-clamp-");
    const std::vector<uint8_t> content{ 10, 20, 30, 40, 50 };
    VERIFY_IS_TRUE(KittyWriteAllBytes(path, content));

    std::vector<uint8_t> out;
    // Ask for far more than the file holds; the read must clamp to the file's size.
    VERIFY_IS_TRUE(til::read_image_result::ok == term.ReadKittyImageFile(path, 0, 100ull * 1024 * 1024, false, out), L"read should succeed");
    VERIFY_ARE_EQUAL(content.size(), out.size(), L"an oversize S= must clamp to EOF (the file size)");
    VERIFY_IS_TRUE(out == content);

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadKittyImageFileRejectsUncPath()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    // A UNC path must be rejected up front (no SMB connection, no NTLM handshake).
    std::vector<uint8_t> out{ 7, 7, 7 };
    VERIFY_IS_TRUE(til::read_image_result::invalid == term.ReadKittyImageFile(L"\\\\127.0.0.1\\share\\never.bin", 0, 0, false, out), L"a UNC path must be rejected as an invalid request");
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size(), L"a rejected read must leave the output empty");
}

void TerminalApiTest::ReadKittyImageFileRejectsRelativePath()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    // Relative paths are rejected so a client cannot read a file resolved against the
    // terminal's own working directory.
    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_image_result::invalid == term.ReadKittyImageFile(L"relative\\file.bin", 0, 0, false, out), L"a non-absolute path must be rejected as an invalid request");
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
}

void TerminalApiTest::ReadKittyImageFileNonexistentFails()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    // A unique path we never create.
    const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-missing-");

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_image_result::not_found == term.ReadKittyImageFile(path, 0, 0, false, out), L"a missing file must report not_found (ENOENT)");
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
}

void TerminalApiTest::ReadKittyImageFileRejectsCharDevice()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    // A drive-absolute path to the NUL character device on the temp dir's (fixed) drive. NUL is a
    // reserved name that resolves to a device in any directory, so this passes the drive/namespace
    // checks and opens successfully -- but it is NOT a regular file. The spec's "only regular files
    // may be read" rule (GetFileType == FILE_TYPE_DISK) must refuse it, so a client cannot make the
    // terminal read from a device.
    const auto devicePath = dir.substr(0, 3) + L"NUL"; // e.g. "C:\\NUL"

    std::vector<uint8_t> out{ 1, 2, 3 };
    VERIFY_IS_TRUE(til::read_image_result::read_error == term.ReadKittyImageFile(devicePath, 0, 0, false, out), L"a character device (NUL) must be refused as unreadable (EBADF): only regular files may be read");
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size(), L"a rejected read must leave the output empty");
}

void TerminalApiTest::ReadKittyImageFileRejectsDirectory()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    // A real, drive-absolute directory passes every pre-open path check (fixed local drive,
    // not UNC, not relative) but must NOT be readable as an image. The helper opens with
    // FILE_ATTRIBUTE_NORMAL and no FILE_FLAG_BACKUP_SEMANTICS, so CreateFileW on a directory
    // fails with ERROR_ACCESS_DENIED -- NOT a not-found error -- and is reported as
    // read_error (EBADF). Because a junction is itself a directory reparse point, this also
    // covers the reparse-directory case: a client cannot point t=f/t=t at a folder.
    const auto subdir = KittyUniquePath(dir, L"tty-graphics-protocol-dir-");
    VERIFY_IS_TRUE(CreateDirectoryW(subdir.c_str(), nullptr) != FALSE, L"failed to create the test directory");

    std::vector<uint8_t> out{ 1, 2, 3 };
    VERIFY_IS_TRUE(til::read_image_result::read_error == term.ReadKittyImageFile(subdir, 0, 0, false, out), L"a directory must be refused as unreadable (EBADF): only regular files may be read");
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size(), L"a rejected read must leave the output empty");

    RemoveDirectoryW(subdir.c_str());
}

void TerminalApiTest::ReadKittyImageFileCapsAtMaxBytes()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-huge-");

    // The 32 MiB hard cap in til::read_image_file. Kept in sync by intent; a mismatch here
    // means the cap moved and this test must be revisited.
    constexpr uint64_t cap = 32ull * 1024 * 1024;

    // Create a file LARGER than the cap WITHOUT writing 33 MiB of data: extend the size with
    // SetEndOfFile so the [0, EOF) range reads back as zeros via NTFS valid-data-length
    // semantics (no bulk disk I/O), which is enough to exercise the read bound.
    {
        wil::unique_hfile f{ CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr) };
        VERIFY_IS_TRUE(static_cast<bool>(f), L"failed to create the oversize test file");
        LARGE_INTEGER eof{};
        eof.QuadPart = static_cast<LONGLONG>(cap + 1024 * 1024); // 33 MiB, comfortably over the cap
        VERIFY_IS_TRUE(SetFilePointerEx(f.get(), eof, nullptr, FILE_BEGIN) != FALSE);
        VERIFY_IS_TRUE(SetEndOfFile(f.get()) != FALSE, L"failed to size the oversize test file");
    }

    std::vector<uint8_t> out;
    // S=0 means "read the whole file"; the cap must still bound the result so a client cannot
    // make the terminal allocate/return an unbounded amount from a huge (or lying) file.
    VERIFY_IS_TRUE(til::read_image_result::ok == term.ReadKittyImageFile(path, 0, 0, false, out), L"reading a huge file must still succeed (clamped)");
    VERIFY_ARE_EQUAL(static_cast<size_t>(cap), out.size(), L"the read must be capped at 32 MiB regardless of the file's size");

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadKittyImageFileFailedReadKeepsTempFile()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    // A temp file that satisfies BOTH deletion gates (under %TEMP% AND carrying the
    // "tty-graphics-protocol" marker), so a *successful* t=t read WOULD delete it.
    const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-faildelete-");
    VERIFY_IS_TRUE(KittyWriteAllBytes(path, { 1, 2, 3, 4 }));

    // Force the operation to FAIL with an offset past EOF. deleteAfter=true opens the file
    // WITH delete access, but deletion is decided only AFTER a successful read -- so a failed
    // t=t must never delete its target. This proves a malformed/failed transfer cannot be
    // turned into an arbitrary-delete primitive on a file that otherwise qualifies.
    std::vector<uint8_t> out{ 9, 9, 9 };
    VERIFY_IS_TRUE(til::read_image_result::invalid == term.ReadKittyImageFile(path, 1000, 0, true, out), L"an offset past EOF must fail as an invalid request");
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size(), L"a failed read must leave the output empty");
    VERIFY_IS_TRUE(KittyFileExists(path), L"a failed t=t read must NOT delete its target, even a marked temp file");

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadKittySharedMemoryCopiesSliceAndCloses()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto name = KittyUniqueMappingName();
    const std::vector<uint8_t> content{ 'A', 'B', 'C', 'D', 'E', 'F' };
    auto mapping = KittyCreateMapping(name, content.size(), content);
    VERIFY_IS_TRUE(static_cast<bool>(mapping), L"failed to create the shared-memory test object");

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_shared_memory_result::ok == term.ReadKittySharedMemory(name, 2, 3, out));
    const std::vector<uint8_t> expected{ 'C', 'D', 'E' };
    VERIFY_IS_TRUE(out == expected, L"O=2,S=3 must copy exactly bytes [2,5)");

    // The protocol requires Windows terminals to close (not unlink) the object. Once
    // the creator closes its handle, no leaked terminal handle may keep the name alive.
    mapping.reset();
    wil::unique_handle reopened{ OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()) };
    VERIFY_IS_FALSE(static_cast<bool>(reopened), L"the terminal must close its mapping handle before returning");
}

void TerminalApiTest::ReadKittySharedMemoryCapsAtMaxBytes()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    constexpr uint64_t cap = 32ull * 1024 * 1024;
    const auto name = KittyUniqueMappingName();
    auto mapping = KittyCreateMapping(name, cap + 1024 * 1024);
    VERIFY_IS_TRUE(static_cast<bool>(mapping), L"failed to create the oversize shared-memory object");

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_shared_memory_result::ok == term.ReadKittySharedMemory(name, 0, 0, out));
    VERIFY_ARE_EQUAL(static_cast<size_t>(cap), out.size(), L"S=0 must still honor the 32 MiB hard cap");
}

void TerminalApiTest::ReadKittySharedMemoryRejectsUnsafeNames()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    std::vector<uint8_t> out{ 1, 2, 3 };
    VERIFY_IS_TRUE(til::read_shared_memory_result::invalid == term.ReadKittySharedMemory(L"Global\\service-object", 0, 1, out), L"cross-session Global objects must be unreachable");
    VERIFY_IS_TRUE(til::read_shared_memory_result::invalid == term.ReadKittySharedMemory(L"Session\\1\\object", 0, 1, out), L"system and nested object-manager paths must be rejected");
    VERIFY_IS_TRUE(til::read_shared_memory_result::invalid == term.ReadKittySharedMemory(L"Local\\nested\\object", 0, 1, out), L"Local names may not escape into nested namespaces");
    const std::wstring nulName{ L"Local\\foo\0bar", 13 };
    VERIFY_IS_TRUE(til::read_shared_memory_result::invalid == term.ReadKittySharedMemory(nulName, 0, 1, out), L"embedded NULs must not truncate the opened name");
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
}

void TerminalApiTest::ReadKittySharedMemoryMissingFails()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto name = KittyUniqueMappingName();
    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_shared_memory_result::not_found == term.ReadKittySharedMemory(name, 0, 1, out));
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
}

void TerminalApiTest::ReadKittySharedMemoryRejectsOffsetPastEnd()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto name = KittyUniqueMappingName();
    auto mapping = KittyCreateMapping(name, 4096);
    VERIFY_IS_TRUE(static_cast<bool>(mapping));

    std::vector<uint8_t> out{ 1 };
    VERIFY_IS_TRUE(til::read_shared_memory_result::invalid == term.ReadKittySharedMemory(name, 128 * 1024, 1, out));
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
}
