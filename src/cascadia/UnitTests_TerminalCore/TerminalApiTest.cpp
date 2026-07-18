// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include <WexTestClass.h>

#include "../cascadia/TerminalCore/Terminal.hpp"
#include "MockTermSettings.h"
#include "../renderer/inc/DummyRenderer.hpp"
#include "../renderer/inc/RenderEngineBase.hpp"
#include "consoletaeftemplates.hpp"

#include <wil/resource.h>

using namespace winrt::Microsoft::Terminal::Core;
using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console::Render;

using namespace WEX::Logging;
using namespace WEX::TestExecution;

namespace
{
    struct PaintedCluster
    {
        til::point position;
        til::CoordType columns;
        std::wstring text;
        TextAttribute attribute;
    };

    struct PaintedImage
    {
        til::CoordType targetRow;
        til::CoordType columnOffset;
        std::vector<uint32_t> columnOwners;
        std::vector<bool> columnsContainRed;
        std::vector<bool> columnsContainGreen;
    };

    struct PaintedFrame
    {
        std::vector<PaintedCluster> clusters;
        std::vector<PaintedImage> images;
    };

    class KittyRecordingRenderEngine final : public RenderEngineBase
    {
    public:
        HRESULT StartPaint() noexcept override
        {
            const auto guard = _lock.lock_exclusive();
            _clusters.clear();
            _images.clear();
            _paintingInvalidation = _invalidationGeneration.load();
            return S_OK;
        }

        HRESULT EndPaint() noexcept override
        {
            return S_OK;
        }

        HRESULT Present() noexcept override
        try
        {
            {
                const auto guard = _lock.lock_exclusive();
                _presentedClusters = _clusters;
                _presentedImages = _images;
                _presentedInvalidation.store(_paintingInvalidation);
            }
            ++_frameCount;
            _frameReady.SetEvent();
            return S_OK;
        }
        CATCH_RETURN()

        HRESULT ScrollFrame() noexcept override
        {
            return S_OK;
        }

        HRESULT Invalidate(const til::rect* /*region*/) noexcept override
        {
            return S_OK;
        }

        HRESULT InvalidateCursor(const til::rect* /*region*/) noexcept override
        {
            return S_OK;
        }

        HRESULT InvalidateSystem(const til::rect* /*region*/) noexcept override
        {
            return S_OK;
        }

        HRESULT InvalidateScroll(const til::point* /*delta*/) noexcept override
        {
            return S_OK;
        }

        HRESULT InvalidateAll() noexcept override
        {
            ++_invalidationGeneration;
            return S_OK;
        }

        HRESULT PaintBackground() noexcept override
        {
            return S_OK;
        }

        HRESULT PaintBufferLine(const std::span<const Cluster> clusters, til::point position, bool /*trimLeft*/) noexcept override
        try
        {
            const auto guard = _lock.lock_exclusive();
            for (const auto& cluster : clusters)
            {
                _clusters.emplace_back(position, cluster.GetColumns(), std::wstring{ cluster.GetText() }, _currentAttribute);
                position.x += cluster.GetColumns();
            }
            return S_OK;
        }
        CATCH_RETURN()

        HRESULT PaintBufferGridLines(GridLineSet /*lines*/, COLORREF /*gridlineColor*/, COLORREF /*underlineColor*/, size_t /*length*/, til::point /*position*/) noexcept override
        {
            return S_OK;
        }

        HRESULT PaintImageSlice(const ImageSlice& imageSlice,
                                const ImageSlice::RenderPosition position,
                                const til::CoordType targetRow,
                                const til::CoordType viewportLeft,
                                const std::span<const uint8_t> /*backgroundMask*/) noexcept override
        try
        {
            PaintedImage image{
                targetRow,
                imageSlice.ColumnOffset() - viewportLeft,
            };

            const auto columnCount = imageSlice.PixelWidth() / std::max(1, imageSlice.CellSize().width);
            const auto pixels = imageSlice.Pixels(position);
            image.columnOwners.reserve(columnCount);
            for (til::CoordType i = 0; i < columnCount; ++i)
            {
                const auto column = imageSlice.ColumnOffset() + i;
                image.columnOwners.emplace_back(imageSlice.ColumnOwner(column));
                auto pixel = std::next(pixels.data(), i * imageSlice.CellSize().width);
                auto containsRed = false;
                auto containsGreen = false;
                for (auto y = 0; y < imageSlice.CellSize().height && !(containsRed && containsGreen); ++y)
                {
                    for (auto x = 0; x < imageSlice.CellSize().width; ++x)
                    {
                        if (pixel[x].rgbRed == 255 && pixel[x].rgbGreen == 0 && pixel[x].rgbBlue == 0)
                        {
                            containsRed = true;
                        }
                        if (pixel[x].rgbRed == 0 && pixel[x].rgbGreen == 255 && pixel[x].rgbBlue == 0)
                        {
                            containsGreen = true;
                        }
                    }
                    std::advance(pixel, imageSlice.PixelWidth());
                }
                image.columnsContainRed.emplace_back(containsRed);
                image.columnsContainGreen.emplace_back(containsGreen);
            }

            const auto guard = _lock.lock_exclusive();
            _images.emplace_back(std::move(image));
            return S_OK;
        }
        CATCH_RETURN()

        HRESULT PaintSelection(const til::rect& /*rect*/) noexcept override
        {
            return S_OK;
        }

        HRESULT PaintCursor(const CursorOptions& /*options*/) noexcept override
        {
            return S_OK;
        }

        HRESULT UpdateDrawingBrushes(const TextAttribute& textAttributes,
                                     const RenderSettings& /*renderSettings*/,
                                     gsl::not_null<IRenderData*> /*renderData*/,
                                     bool /*usingSoftFont*/,
                                     bool /*isSettingDefaultBrushes*/) noexcept override
        {
            const auto guard = _lock.lock_exclusive();
            _currentAttribute = textAttributes;
            return S_OK;
        }

        HRESULT UpdateFont(const FontInfoDesired& /*desired*/, _Out_ FontInfo& /*actual*/) noexcept override
        {
            return S_OK;
        }

        HRESULT UpdateDpi(int /*dpi*/) noexcept override
        {
            return S_OK;
        }

        HRESULT UpdateViewport(const til::inclusive_rect& /*viewport*/) noexcept override
        {
            return S_OK;
        }

        HRESULT GetProposedFont(const FontInfoDesired& /*desired*/, _Out_ FontInfo& /*actual*/, int /*dpi*/) noexcept override
        {
            return S_OK;
        }

        HRESULT GetDirtyArea(std::span<const til::rect>& area) noexcept override
        {
            area = { &_dirtyArea, 1 };
            return S_OK;
        }

        HRESULT GetFontSize(_Out_ til::size* const fontSize) noexcept override
        {
            *fontSize = { 10, 20 };
            return S_OK;
        }

        HRESULT IsGlyphWideByFont(std::wstring_view /*glyph*/, _Out_ bool* const isWide) noexcept override
        {
            *isWide = false;
            return S_OK;
        }

        void WaitUntilCanRender() noexcept override
        {
        }

        uint64_t PrepareForNextFrame() noexcept
        {
            _frameReady.ResetEvent();
            return _frameCount.load();
        }

        bool WaitForFrameAfter(const uint64_t frameCount) noexcept
        {
            return (_frameCount.load() > frameCount || _frameReady.wait(5000)) && _frameCount.load() > frameCount;
        }

        uint64_t PrepareForFullRepaint() noexcept
        {
            _frameReady.ResetEvent();
            return _invalidationGeneration.load() + 1;
        }

        bool WaitForFullRepaint(const uint64_t invalidation) noexcept
        {
            for (auto attempt = 0; attempt < 5 && _presentedInvalidation.load() < invalidation; ++attempt)
            {
                _frameReady.ResetEvent();
                if (_presentedInvalidation.load() >= invalidation)
                {
                    break;
                }
                if (!_frameReady.wait(1000))
                {
                    return false;
                }
            }
            return _presentedInvalidation.load() >= invalidation;
        }

        PaintedFrame Snapshot() const
        {
            const auto guard = _lock.lock_shared();
            return { _presentedClusters, _presentedImages };
        }

    protected:
        HRESULT _DoUpdateTitle(const std::wstring_view /*title*/) noexcept override
        {
            return S_OK;
        }

    private:
        mutable wil::srwlock _lock;
        wil::slim_event_manual_reset _frameReady;
        std::atomic<uint64_t> _frameCount{ 0 };
        std::atomic<uint64_t> _invalidationGeneration{ 1 };
        std::atomic<uint64_t> _presentedInvalidation{ 0 };
        uint64_t _paintingInvalidation = 0;
        til::rect _dirtyArea{ 0, 0, SHRT_MAX, SHRT_MAX };
        TextAttribute _currentAttribute;
        std::vector<PaintedCluster> _clusters;
        std::vector<PaintedImage> _images;
        std::vector<PaintedCluster> _presentedClusters;
        std::vector<PaintedImage> _presentedImages;
    };

    class KittyRenderFixture
    {
    public:
        KittyRenderFixture(const til::size viewport, const til::CoordType history) :
            terminal{ Terminal::TestDummyMarker{} },
            renderer{ &terminal }
        {
            renderer.AddRenderEngine(&engine);
            terminal.Create(viewport, history, renderer);
        }

        ~KittyRenderFixture()
        {
            renderer.TriggerTeardown();
        }

        void StartPainting()
        {
            const auto previousFrame = engine.PrepareForNextFrame();
            renderer.EnablePainting();
            VERIFY_IS_TRUE(engine.WaitForFrameAfter(previousFrame), L"initial render timed out");
        }

        void Repaint()
        {
            const auto invalidation = engine.PrepareForFullRepaint();
            renderer.TriggerRedrawAll();
            VERIFY_IS_TRUE(engine.WaitForFullRepaint(invalidation), L"full repaint timed out");
        }

        KittyRecordingRenderEngine engine;
        Terminal terminal;
        DummyRenderer renderer;
    };

    constexpr std::wstring_view StoreRedKittyImage{ L"\x1b_Ga=T,U=1,i=1,f=24,s=1,v=1,c=1,r=1,q=2;/wAA\x1b\\" };
    constexpr std::wstring_view SelectKittyImageAndBackground{ L"\x1b[38;2;0;0;1;48;2;4;5;6m" };
    constexpr std::wstring_view KittyPlaceholder{ L"\xDBFB\xDEEE\x0305\x0305\x0305" };
    constexpr std::wstring_view OrdinaryCombiningText{ L"A\x0301" };

    std::optional<til::point> FindKittyPlaceholder(const TextBuffer& buffer)
    {
        for (til::CoordType y = 0; y < buffer.GetSize().Height(); ++y)
        {
            const auto& row = buffer.GetRowByOffset(y);
            for (til::CoordType x = 0; x < buffer.GetSize().Width(); ++x)
            {
                if (row.GetKittyPlaceholderCell(x))
                {
                    return til::point{ x, y };
                }
            }
        }
        return std::nullopt;
    }

    void VerifyPlaceholderFrame(const PaintedFrame& frame,
                                const til::point screenPosition,
                                const TextAttribute& expectedAttribute,
                                const uint32_t imageId)
    {
        const auto leakedPlaceholder = std::ranges::any_of(frame.clusters, [](const auto& cluster) {
            return cluster.text.find(L'\xDBFB') != std::wstring::npos ||
                   cluster.text.find(L'\xDEEE') != std::wstring::npos ||
                   cluster.text.find(L'\x0305') != std::wstring::npos;
        });
        VERIFY_IS_FALSE(leakedPlaceholder, L"the KGP placeholder grapheme reached the glyph renderer");

        const auto cluster = std::ranges::find(frame.clusters, screenPosition, &PaintedCluster::position);
        VERIFY_IS_TRUE(cluster != frame.clusters.end(), L"the placeholder cell was not painted");
        if (cluster != frame.clusters.end())
        {
            VERIFY_ARE_EQUAL(std::wstring{ L" " }, cluster->text, L"the full placeholder grapheme must be replaced by one non-rendering cell");
            VERIFY_ARE_EQUAL(1, cluster->columns, L"the placeholder must retain its cell occupancy");
            VERIFY_IS_TRUE(cluster->attribute == expectedAttribute, L"placeholder colors and attributes must survive glyph suppression");
        }

        auto imageAtPlaceholder = false;
        for (const auto& image : frame.images)
        {
            const auto relativeColumn = screenPosition.x - image.columnOffset;
            if (image.targetRow == screenPosition.y &&
                relativeColumn >= 0 &&
                relativeColumn < gsl::narrow_cast<til::CoordType>(image.columnOwners.size()) &&
                til::at(image.columnOwners, relativeColumn) == imageId &&
                til::at(image.columnsContainRed, relativeColumn))
            {
                imageAtPlaceholder = true;
                break;
            }
        }
        VERIFY_IS_TRUE(imageAtPlaceholder, L"the KGP image must remain painted at the placeholder cell");
    }

    bool FrameContainsKittyColor(const PaintedFrame& frame, const uint32_t imageId, const bool green)
    {
        return std::ranges::any_of(frame.images, [&](const auto& image) {
            for (size_t column = 0; column < image.columnOwners.size(); ++column)
            {
                if (til::at(image.columnOwners, column) == imageId &&
                    til::at(green ? image.columnsContainGreen : image.columnsContainRed, column))
                {
                    return true;
                }
            }
            return false;
        });
    }
}

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

        TEST_METHOD(KittyAnimationSurvivesFontAndRendererRefresh);
        TEST_METHOD(KittyPlaceholderRendersInRealTerminal);
        TEST_METHOD(KittyPlaceholderSuppressesGlyphOnPaintAndRepaint);
        TEST_METHOD(KittyPlaceholderLeavesUnrecognizedTextUntouched);
        TEST_METHOD(KittyPlaceholderSuppressesGlyphAfterResize);
        TEST_METHOD(KittyPlaceholderSuppressesGlyphAfterScroll);
        // Kitty graphics file-transmission host I/O (t=f / t=t). These exercise the real
        // filesystem path of Terminal::ReadKittyImageFile (the shared til::read_image_file
        // helper), including the security gates that the adapter-level mock cannot cover.
        TEST_METHOD(ReadKittyImageFileReadsFromTemp);
        TEST_METHOD(ReadKittyImageFileDeletesTempWithMarker);
        TEST_METHOD(ReadKittyImageFileOutsideTempNotDeleted);
        TEST_METHOD(ReadKittyImageFileNoMarkerNotDeleted);
        TEST_METHOD(ReadKittyImageFileParentMarkerNotDeleted);
        TEST_METHOD(ReadKittyImageFileOffsetAndSizeSlice);
        TEST_METHOD(ReadKittyImageFileOversizeSizeClampsToEof);
        TEST_METHOD(ReadKittyImageFileRejectsUncPath);
        TEST_METHOD(ReadKittyImageFileRejectsRelativePath);
        TEST_METHOD(ReadKittyImageFileNormalizesWin32Path);
        TEST_METHOD(ReadKittyImageFileRejectsIntermediateJunction);
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

void TerminalApiTest::KittyAnimationSurvivesFontAndRendererRefresh()
{
    KittyRenderFixture fixture{ { 6, 3 }, 0 };
    auto& buffer = *fixture.terminal._mainBuffer;
    auto& stateMachine = *fixture.terminal._stateMachine;

    stateMachine.ProcessString(L"\x1b_Ga=T,i=7,f=24,s=1,v=1,c=1,r=1,C=1,z=5000,q=2;/wAA\x1b\\");
    stateMachine.ProcessString(L"\x1b_Ga=f,i=7,f=24,s=1,v=1,z=1000,q=2;AP8A\x1b\\");
    stateMachine.ProcessString(L"\x1b_Ga=a,i=7,c=1,r=1,z=5000,s=3,v=1,q=2;\x1b\\");
    fixture.StartPainting();
    VERIFY_IS_TRUE(FrameContainsKittyColor(fixture.engine.Snapshot(), 7, false), L"frame 1 must be visible before the lifecycle transition");

    // Reproduce the stale retained-layer state observed after renderer recreation:
    // the animation source of truth is frame 1 (red), while the retained pixels
    // contain the wrong frame (green).
    const std::array stalePixels{ RGBQUAD{ 0, 255, 0, 0 } };
    fixture.terminal.LockConsole();
    auto unlockAfterStaleLayer = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    auto* slice = buffer.GetMutableRowByOffset(0).GetMutableImageSlice();
    VERIFY_IS_NOT_NULL(slice);
    VERIFY_IS_TRUE(slice && slice->UpdateKittyImage(7, stalePixels));
    unlockAfterStaleLayer.reset();
    fixture.Repaint();
    VERIFY_IS_TRUE(FrameContainsKittyColor(fixture.engine.Snapshot(), 7, true), L"the test must reproduce a stale retained animation layer");

    fixture.terminal.LockConsole();
    auto unlockAfterFontChange = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    fixture.terminal.SetFontInfo(FontInfo{ DEFAULT_FONT_FACE, TMPF_TRUETYPE, 10, { 12, 24 }, CP_UTF8, false });
    unlockAfterFontChange.reset();
    fixture.Repaint();
    VERIFY_IS_TRUE(FrameContainsKittyColor(fixture.engine.Snapshot(), 7, false), L"font/DPI recreation must restore the current animation frame");

    fixture.terminal.LockConsole();
    stateMachine.ProcessString(L"\x1b_Ga=a,i=7,r=1,z=20,q=2;\x1b\\");
    fixture.terminal.UnlockConsole();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto advanced = FrameContainsKittyColor(fixture.engine.Snapshot(), 7, true);
    for (auto attempt = 0; attempt < 10 && !advanced; ++attempt)
    {
        const auto previousFrame = fixture.engine.PrepareForNextFrame();
        fixture.renderer.NotifyPaintFrame();
        if (!fixture.engine.WaitForFrameAfter(previousFrame))
        {
            break;
        }
        advanced = FrameContainsKittyColor(fixture.engine.Snapshot(), 7, true);
    }
    VERIFY_IS_TRUE(advanced, L"animation playback must continue to the next frame after recreation");
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

    struct KittyMountPointReparseBuffer
    {
        ULONG reparseTag;
        USHORT reparseDataLength;
        USHORT reserved;
        USHORT substituteNameOffset;
        USHORT substituteNameLength;
        USHORT printNameOffset;
        USHORT printNameLength;
        WCHAR pathBuffer[MAX_PATH * 2];
    };

    bool KittyCreateJunction(const std::wstring& junctionPath, const std::wstring& targetPath)
    {
        static constexpr DWORD fsctlSetReparsePoint{ 0x000900A4 };

        if (!CreateDirectoryW(junctionPath.c_str(), nullptr))
        {
            return false;
        }

        wil::unique_hfile junction{ CreateFileW(junctionPath.c_str(),
                                                GENERIC_WRITE,
                                                0,
                                                nullptr,
                                                OPEN_EXISTING,
                                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                                nullptr) };
        if (!junction)
        {
            return false;
        }

        const auto substituteName = L"\\??\\" + targetPath;
        const auto substituteBytes = substituteName.size() * sizeof(wchar_t);
        const auto printBytes = targetPath.size() * sizeof(wchar_t);
        KittyMountPointReparseBuffer buffer{};
        if (substituteBytes + printBytes + 2 * sizeof(wchar_t) > sizeof(buffer.pathBuffer))
        {
            return false;
        }

        buffer.reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
        buffer.substituteNameLength = static_cast<USHORT>(substituteBytes);
        buffer.printNameOffset = static_cast<USHORT>(substituteBytes + sizeof(wchar_t));
        buffer.printNameLength = static_cast<USHORT>(printBytes);
        memcpy(buffer.pathBuffer, substituteName.c_str(), substituteBytes);
        memcpy(reinterpret_cast<std::byte*>(buffer.pathBuffer) + buffer.printNameOffset, targetPath.c_str(), printBytes);

        const auto pathBytes = buffer.printNameOffset + buffer.printNameLength + sizeof(wchar_t);
        buffer.reparseDataLength = static_cast<USHORT>(8 + pathBytes);
        const auto inputBytes = static_cast<DWORD>(FIELD_OFFSET(KittyMountPointReparseBuffer, pathBuffer) + pathBytes);
        DWORD returned{};
        return DeviceIoControl(junction.get(), fsctlSetReparsePoint, &buffer, inputBytes, nullptr, 0, &returned, nullptr) != FALSE;
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

void TerminalApiTest::ReadKittyImageFileParentMarkerNotDeleted()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    const auto subdir = KittyUniquePath(dir, L"tty-graphics-protocol-parent-");
    VERIFY_IS_TRUE(CreateDirectoryW(subdir.c_str(), nullptr) != FALSE, L"failed to create the marked parent directory");
    const auto path = KittyUniquePath(subdir + L"\\", L"unrelated-");
    VERIFY_IS_TRUE(KittyWriteAllBytes(path, { 1, 2, 3 }));

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_image_result::ok == term.ReadKittyImageFile(path, 0, 0, true, out), L"the read should succeed");
    VERIFY_IS_TRUE(KittyFileExists(path), L"a marker in a parent directory must not authorize deleting an unrelated file");

    DeleteFileW(path.c_str());
    RemoveDirectoryW(subdir.c_str());
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

void TerminalApiTest::ReadKittyImageFileNormalizesWin32Path()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-normalize-");
    const auto filename = path.substr(path.find_last_of(L'\\') + 1);
    const std::vector<uint8_t> content{ 1, 2, 3 };
    VERIFY_IS_TRUE(KittyWriteAllBytes(path, content));

    const auto equivalentPath = dir + L".\\unused\\..\\\\" + filename + L".";
    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(til::read_image_result::ok == term.ReadKittyImageFile(equivalentPath, 0, 0, false, out), L"Win32-equivalent dot segments, duplicate separators, and a trailing period must resolve to the same file");
    VERIFY_IS_TRUE(out == content);

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadKittyImageFileRejectsIntermediateJunction()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    const auto targetDir = KittyUniquePath(dir, L"kitty-junction-target-");
    const auto junctionPath = KittyUniquePath(dir, L"kitty-junction-link-");
    const auto targetFile = targetDir + L"\\payload.bin";
    const auto junctionFile = junctionPath + L"\\payload.bin";
    auto cleanup = wil::scope_exit([&]() {
        DeleteFileW(targetFile.c_str());
        RemoveDirectoryW(junctionPath.c_str());
        RemoveDirectoryW(targetDir.c_str());
    });

    VERIFY_IS_TRUE(CreateDirectoryW(targetDir.c_str(), nullptr) != FALSE, L"failed to create the junction target directory");
    VERIFY_IS_TRUE(KittyWriteAllBytes(targetFile, { 1, 2, 3 }), L"failed to create the junction target file");
    VERIFY_IS_TRUE(KittyCreateJunction(junctionPath, targetDir), L"failed to create the test junction");

    std::vector<uint8_t> out{ 7, 7, 7 };
    VERIFY_IS_TRUE(til::read_image_result::read_error == term.ReadKittyImageFile(junctionFile, 0, 0, false, out), L"an intermediate reparse point must be rejected before its target is opened");
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
    // DOS device aliases resolve in any directory, with optional extensions and legacy
    // superscript digits. They must retain EBADF semantics after switching to native opens.
    for (const auto deviceName : { L"NUL", L"NUL.txt", L"COM\u00B9", L"LPT\u00B3.txt" })
    {
        const auto devicePath = dir.substr(0, 3) + deviceName; // e.g. "C:\\NUL"
        std::vector<uint8_t> out{ 1, 2, 3 };
        VERIFY_IS_TRUE(til::read_image_result::read_error == term.ReadKittyImageFile(devicePath, 0, 0, false, out), L"a DOS character device must be refused as unreadable (EBADF): only regular files may be read");
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size(), L"a rejected read must leave the output empty");
    }
}

void TerminalApiTest::ReadKittyImageFileRejectsDirectory()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    // A real, drive-absolute directory passes every pre-open path check but must not be
    // readable as an image. FILE_NON_DIRECTORY_FILE reports it as read_error (EBADF).
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

void TerminalApiTest::KittyPlaceholderRendersInRealTerminal()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& tbi = *(term._mainBuffer);
    auto& sm = *(term._stateMachine);

    // Virtually store a 1x1 red image as id 1 (U=1 => no cursor draw).
    sm.ProcessString(L"\x1b_Ga=T,i=1,U=1,f=24,s=1,v=1;/wAA\x1b\\");
    // Foreground = image id 1 (RGB 0,0,1), then print one U+10EEEE placeholder cell.
    sm.ProcessString(L"\x1b[38;2;0;0;1m");
    sm.ProcessString(std::wstring{ L'\xDBFB', L'\xDEEE' });

    // The placeholder must render the stored image's actual color (red), not merely produce
    // an empty slice. Scan every slice's pixels for RGB(255,0,0).
    auto renderedRed = false;
    for (til::CoordType y = 0; y < 100 && !renderedRed; ++y)
    {
        if (const auto slice = tbi.GetRowByOffset(y).GetImageSlice())
        {
            for (const auto& px : slice->Pixels())
            {
                if (px.rgbRed == 255 && px.rgbGreen == 0 && px.rgbBlue == 0)
                {
                    renderedRed = true;
                    break;
                }
            }
        }
    }
    VERIFY_IS_TRUE(renderedRed, L"a placeholder cell must render the stored image's red pixels in the real Terminal");
}

void TerminalApiTest::KittyPlaceholderSuppressesGlyphOnPaintAndRepaint()
{
    KittyRenderFixture fixture{ { 8, 3 }, 0 };
    auto& buffer = *fixture.terminal._mainBuffer;
    auto& stateMachine = *fixture.terminal._stateMachine;

    stateMachine.ProcessString(StoreRedKittyImage);
    stateMachine.ProcessString(L"XY");
    stateMachine.ProcessString(SelectKittyImageAndBackground);
    stateMachine.ProcessString(KittyPlaceholder);

    const til::point placeholderPosition{ 2, 0 };
    VERIFY_ARE_EQUAL((til::point{ 3, 0 }), buffer.GetCursor().GetPosition(), L"the placeholder must advance the cursor by one cell");
    VERIFY_ARE_EQUAL(std::wstring{ KittyPlaceholder }, std::wstring{ buffer.GetRowByOffset(0).GlyphAt(2) }, L"glyph suppression must not alter stored text");
    const auto placeholderAttribute = buffer.GetRowByOffset(0).GetAttrByColumn(2);

    stateMachine.ProcessString(L"\x1b[0m\x1b[2;1H");
    stateMachine.ProcessString(OrdinaryCombiningText);

    fixture.StartPainting();
    auto frame = fixture.engine.Snapshot();
    VerifyPlaceholderFrame(frame, placeholderPosition, placeholderAttribute, 1);

    const auto ordinary = std::ranges::find(frame.clusters, til::point{ 0, 1 }, &PaintedCluster::position);
    VERIFY_IS_TRUE(ordinary != frame.clusters.end(), L"the ordinary combining sequence was not painted");
    if (ordinary != frame.clusters.end())
    {
        VERIFY_ARE_EQUAL(std::wstring{ OrdinaryCombiningText }, ordinary->text, L"ordinary Unicode combining sequences must remain unchanged");
    }

    fixture.Repaint();
    VerifyPlaceholderFrame(fixture.engine.Snapshot(), placeholderPosition, placeholderAttribute, 1);
}

void TerminalApiTest::KittyPlaceholderLeavesUnrecognizedTextUntouched()
{
    KittyRenderFixture fixture{ { 6, 2 }, 0 };
    auto& buffer = *fixture.terminal._mainBuffer;
    fixture.terminal._stateMachine->ProcessString(KittyPlaceholder);

    VERIFY_IS_NULL(buffer.GetRowByOffset(0).GetKittyPlaceholderCell(0), L"no stored virtual placement should leave the private-use text unrecognized");
    fixture.StartPainting();

    const auto frame = fixture.engine.Snapshot();
    const auto cluster = std::ranges::find(frame.clusters, til::point{ 0, 0 }, &PaintedCluster::position);
    VERIFY_IS_TRUE(cluster != frame.clusters.end(), L"the unrecognized private-use text was not painted");
    if (cluster != frame.clusters.end())
    {
        VERIFY_ARE_EQUAL(std::wstring{ KittyPlaceholder }, cluster->text, L"unrecognized private-use text must reach the glyph renderer unchanged");
        VERIFY_ARE_EQUAL(1, cluster->columns);
    }
}

void TerminalApiTest::KittyPlaceholderSuppressesGlyphAfterResize()
{
    KittyRenderFixture fixture{ { 12, 4 }, 0 };
    auto& initialBuffer = *fixture.terminal._mainBuffer;
    auto& stateMachine = *fixture.terminal._stateMachine;

    stateMachine.ProcessString(StoreRedKittyImage);
    stateMachine.ProcessString(L"0123456789");
    stateMachine.ProcessString(SelectKittyImageAndBackground);
    stateMachine.ProcessString(KittyPlaceholder);

    const til::point oldPosition{ 10, 0 };
    VERIFY_IS_NOT_NULL(initialBuffer.GetRowByOffset(oldPosition.y).GetKittyPlaceholderCell(oldPosition.x));
    auto initialSlice = initialBuffer.GetMutableRowByOffset(0).GetMutableImageSlice();
    VERIFY_IS_NOT_NULL(initialSlice);
    if (initialSlice)
    {
        auto sixelPixels = initialSlice->MutablePixels(0, 1);
        for (auto y = 0; y < initialSlice->CellSize().height; ++y)
        {
            for (auto x = 0; x < initialSlice->CellSize().width; ++x)
            {
                *sixelPixels++ = RGBQUAD{ 255, 0, 0, 0 };
            }
            std::advance(sixelPixels, initialSlice->PixelWidth() - initialSlice->CellSize().width);
        }
    }
    fixture.StartPainting();

    fixture.terminal.LockConsole();
    auto unlockAfterResize = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    VERIFY_SUCCEEDED(fixture.terminal.UserResize({ 6, 4 }));
    unlockAfterResize.reset();
    fixture.Repaint();

    const auto& resizedBuffer = *fixture.terminal._mainBuffer;
    const auto movedPosition = FindKittyPlaceholder(resizedBuffer);
    VERIFY_IS_TRUE(movedPosition.has_value(), L"the placeholder metadata was lost during reflow");
    if (movedPosition)
    {
        VERIFY_ARE_EQUAL((til::point{ 4, 1 }), *movedPosition, L"the placeholder must reflow with its text cell");
        VERIFY_ARE_NOT_EQUAL(oldPosition, *movedPosition, L"the placeholder did not move during reflow");
        VERIFY_ARE_EQUAL(std::wstring{ KittyPlaceholder }, std::wstring{ resizedBuffer.GetRowByOffset(movedPosition->y).GlyphAt(movedPosition->x) });

        const auto screenPosition = *movedPosition - til::point{ 0, fixture.terminal.GetViewport().Top() };
        const auto attribute = resizedBuffer.GetRowByOffset(movedPosition->y).GetAttrByColumn(movedPosition->x);
        VerifyPlaceholderFrame(fixture.engine.Snapshot(), screenPosition, attribute, 1);
    }
    const auto resizedFirstSlice = resizedBuffer.GetRowByOffset(0).GetImageSlice();
    VERIFY_IS_NOT_NULL(resizedFirstSlice, L"relocating Kitty pixels must not drop unowned legacy image data");
    if (resizedFirstSlice)
    {
        const auto pixel = resizedFirstSlice->Pixels(0);
        VERIFY_ARE_EQUAL(static_cast<BYTE>(255), pixel->rgbBlue, L"the unowned legacy image column must survive Kitty relocation");
        VERIFY_ARE_EQUAL(0u, resizedFirstSlice->ColumnOwner(0));
    }

    fixture.terminal.LockConsole();
    auto unlockAfterWiden = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    VERIFY_SUCCEEDED(fixture.terminal.UserResize({ 12, 4 }));
    unlockAfterWiden.reset();
    fixture.Repaint();

    const auto& widenedBuffer = *fixture.terminal._mainBuffer;
    const auto widenedPosition = FindKittyPlaceholder(widenedBuffer);
    VERIFY_IS_TRUE(widenedPosition.has_value(), L"the placeholder metadata was lost while joining wrapped rows");
    if (widenedPosition)
    {
        VERIFY_ARE_EQUAL(oldPosition, *widenedPosition, L"widening must join the placeholder back into its original logical row");
        const auto attribute = widenedBuffer.GetRowByOffset(widenedPosition->y).GetAttrByColumn(widenedPosition->x);
        VerifyPlaceholderFrame(fixture.engine.Snapshot(), *widenedPosition, attribute, 1);
    }
    const auto widenedFirstSlice = widenedBuffer.GetRowByOffset(0).GetImageSlice();
    VERIFY_IS_NOT_NULL(widenedFirstSlice, L"joining wrapped rows must preserve unowned legacy image data");
    if (widenedFirstSlice)
    {
        const auto pixel = widenedFirstSlice->Pixels(0);
        VERIFY_ARE_EQUAL(static_cast<BYTE>(255), pixel->rgbBlue, L"widening must not replace an earlier row's unowned legacy image column");
        VERIFY_ARE_EQUAL(0u, widenedFirstSlice->ColumnOwner(0));
    }
}

void TerminalApiTest::KittyPlaceholderSuppressesGlyphAfterScroll()
{
    KittyRenderFixture fixture{ { 6, 3 }, 0 };
    auto& buffer = *fixture.terminal._mainBuffer;
    auto& stateMachine = *fixture.terminal._stateMachine;

    stateMachine.ProcessString(StoreRedKittyImage);
    stateMachine.ProcessString(L"\x1b[2;2H");
    stateMachine.ProcessString(SelectKittyImageAndBackground);
    stateMachine.ProcessString(KittyPlaceholder);

    const til::point oldPosition{ 1, 1 };
    fixture.StartPainting();
    fixture.terminal.LockConsole();
    auto unlockAfterScroll = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    stateMachine.ProcessString(L"\x1b[3;1H\n");
    unlockAfterScroll.reset();
    fixture.Repaint();

    const auto movedPosition = FindKittyPlaceholder(buffer);
    VERIFY_IS_TRUE(movedPosition.has_value(), L"the placeholder metadata was lost during scrolling");
    if (movedPosition)
    {
        VERIFY_ARE_EQUAL((til::point{ 1, 0 }), *movedPosition, L"the placeholder must move with the scrolled row");
        VERIFY_ARE_NOT_EQUAL(oldPosition, *movedPosition);
        VERIFY_ARE_EQUAL(std::wstring{ KittyPlaceholder }, std::wstring{ buffer.GetRowByOffset(movedPosition->y).GlyphAt(movedPosition->x) });

        const auto attribute = buffer.GetRowByOffset(movedPosition->y).GetAttrByColumn(movedPosition->x);
        VerifyPlaceholderFrame(fixture.engine.Snapshot(), *movedPosition, attribute, 1);
    }
}
