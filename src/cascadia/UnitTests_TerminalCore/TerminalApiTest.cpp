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

        HRESULT BeginRowImages(const ImageSlice& imageSlice,
                               const til::CoordType targetRow,
                               const til::CoordType viewportLeft,
                               const std::span<const uint8_t> /*defaultBackgroundMask*/) noexcept override
        try
        {
            // The real engines decide when each plane lands relative to the
            // text; this mock only cares what pixels each plane holds, so it
            // records one entry per plane and lets the assertions search.
            for (size_t plane = 0; plane < ImageSlice::RenderPositionCount; ++plane)
            {
                _recordPlane(imageSlice, static_cast<ImageSlice::RenderPosition>(plane), targetRow, viewportLeft);
            }
            return S_OK;
        }
        CATCH_RETURN()

        HRESULT EndRowImages() noexcept override
        {
            return S_OK;
        }

        void _recordPlane(const ImageSlice& imageSlice,
                          const ImageSlice::RenderPosition position,
                          const til::CoordType targetRow,
                          const til::CoordType viewportLeft)
        {
            PaintedImage image{
                targetRow,
                imageSlice.ColumnOffset() - viewportLeft,
            };

            const auto columnCount = imageSlice.PixelWidth() / std::max(1, imageSlice.CellSize().width);
            const auto pixels = imageSlice.Pixels(position);
            if (pixels.empty())
            {
                return;
            }
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
        }

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

        bool WaitForFrameAfter(const uint64_t frameCount, const DWORD timeout) noexcept
        {
            return (_frameCount.load() > frameCount || _frameReady.wait(timeout)) && _frameCount.load() > frameCount;
        }

        uint64_t PrepareForFullRepaint() noexcept
        {
            _frameReady.ResetEvent();
            return _invalidationGeneration.load() + 1;
        }

        bool WaitForFullRepaint(const uint64_t invalidation, const DWORD timeout) noexcept
        {
            // Reset before the check, so a frame presented between the two is seen
            // by the check rather than lost by the reset.
            _frameReady.ResetEvent();
            return _presentedInvalidation.load() >= invalidation ||
                   (_frameReady.wait(timeout) && _presentedInvalidation.load() >= invalidation);
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
            renderer.EnablePainting();
            // Enabling painting only starts the render thread; the thread then sleeps until
            // something asks for a frame. Whether one is already pending depends on what the
            // caller happened to write beforehand, so ask for a frame outright rather than
            // waiting on one that may never be requested -- and keep asking. A request is a
            // flag the thread clears when it starts painting, so one that lands while a frame
            // is already in flight is not a request for the frame being waited on.
            VERIFY_IS_TRUE(_pump([&](const DWORD timeout) {
                               const auto previousFrame = engine.PrepareForNextFrame();
                               renderer.TriggerRedrawAll();
                               return engine.WaitForFrameAfter(previousFrame, timeout);
                           }),
                           L"initial render timed out");
        }

        void Repaint()
        {
            const auto invalidation = engine.PrepareForFullRepaint();
            VERIFY_IS_TRUE(_pump([&](const DWORD timeout) {
                               renderer.TriggerRedrawAll();
                               return engine.WaitForFullRepaint(invalidation, timeout);
                           }),
                           L"full repaint timed out");
        }

        KittyRecordingRenderEngine engine;
        Terminal terminal;
        DummyRenderer renderer;

    private:
        // Re-asks for a frame every second rather than issuing one request and waiting
        // out a single long timeout, which a loaded machine can lose to scheduling.
        template<typename F>
        static bool _pump(F&& request)
        {
            for (auto attempt = 0; attempt < 15; ++attempt)
            {
                if (request(1000))
                {
                    return true;
                }
            }
            return false;
        }
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
                if (row.GetImageCellRef(x))
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

        TEST_METHOD(KittyPlaceholderRendersInRealTerminal);
        TEST_METHOD(KittyPlaceholderSuppressesGlyphOnPaintAndRepaint);
        TEST_METHOD(KittyPlaceholderLeavesUnrecognizedTextUntouched);
        TEST_METHOD(KittyPlaceholderSuppressesGlyphAfterResize);
        TEST_METHOD(KittyPlaceholderSuppressesGlyphAfterScroll);
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
            for (const auto& px : slice->Pixels(ImageSlice::RenderPosition::AboveText))
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

    VERIFY_IS_NULL(buffer.GetRowByOffset(0).GetImageCellRef(0), L"no stored virtual placement should leave the private-use text unrecognized");
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
    VERIFY_IS_NOT_NULL(initialBuffer.GetRowByOffset(oldPosition.y).GetImageCellRef(oldPosition.x));
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
