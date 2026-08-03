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
#include <wincrypt.h>
#include <wincodec.h>

using namespace winrt::Microsoft::Terminal::Core;
using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console::Render;
using namespace Microsoft::Console::VirtualTerminal;

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using namespace std::string_view_literals;

namespace
{
    std::vector<uint8_t> DecodeFixture(const std::string_view encoded)
    {
        DWORD size = 0;
        THROW_IF_WIN32_BOOL_FALSE(CryptStringToBinaryA(encoded.data(), gsl::narrow<DWORD>(encoded.size()), CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr));
        std::vector<uint8_t> decoded(size);
        THROW_IF_WIN32_BOOL_FALSE(CryptStringToBinaryA(encoded.data(), gsl::narrow<DWORD>(encoded.size()), CRYPT_STRING_BASE64, decoded.data(), &size, nullptr, nullptr));
        decoded.resize(size);
        return decoded;
    }

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
        ImagePlacement::Key key;
        const Image* surface = nullptr;
        uint64_t revision = 0;
        til::size surfaceSize;
        til::rect bounds;
        til::rect originalBounds;
        til::rect source;
        ImagePlacement::PixelGeometry geometry;
        int32_t zIndex = 0;
        ImagePlacement::RenderPosition position = ImagePlacement::RenderPosition::AboveText;
        bool containsRed = false;
        bool containsGreen = false;
        std::vector<uint32_t> columnOwners;
        std::vector<bool> columnsContainRed;
        std::vector<bool> columnsContainGreen;
        std::vector<uint8_t> defaultBackgroundMask;
        std::vector<COLORREF> cellBackgrounds;
    };

    struct PaintedFrame
    {
        std::vector<PaintedCluster> clusters;
        std::vector<PaintedImage> images;
        size_t imageFramePreparations = 0;
        size_t surfaceUploads = 0;
        size_t surfaceRefreshes = 0;
        size_t surfaceEvictions = 0;
    };

    class KittyRecordingRenderEngine final : public RenderEngineBase
    {
    public:
        HRESULT StartPaint() noexcept override
        {
            const auto guard = _lock.lock_exclusive();
            _clusters.clear();
            _images.clear();
            _imageFramePreparations = 0;
            _surfaceUploads = 0;
            _surfaceRefreshes = 0;
            _surfaceEvictions = 0;
            return S_OK;
        }

        HRESULT EndPaint() noexcept override
        {
            const auto guard = _lock.lock_exclusive();
            _dirtyArea = {};
            return S_OK;
        }

        HRESULT Present() noexcept override
        try
        {
            const auto guard = _lock.lock_exclusive();
            _presentedClusters = _clusters;
            _presentedImages = _images;
            _presentedImageFramePreparations = _imageFramePreparations;
            _presentedSurfaceUploads = _surfaceUploads;
            _presentedSurfaceRefreshes = _surfaceRefreshes;
            _presentedSurfaceEvictions = _surfaceEvictions;
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
            const auto guard = _lock.lock_exclusive();
            _dirtyArea = { 0, 0, SHRT_MAX, SHRT_MAX };
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

        HRESULT PrepareImageFrame(const ImageFrameInfo info) noexcept override
        try
        {
            const auto guard = _lock.lock_exclusive();
            ++_imageFramePreparationCalls;
            const auto result = std::exchange(_nextImageFrameResult, S_OK);
            RETURN_IF_FAILED(result);
            _framePlacements.assign(info.placements.begin(), info.placements.end());
            _frameSurfaces.assign(info.surfaces.begin(), info.surfaces.end());
            _imageViewportOrigin = info.viewportOrigin;
            ++_imageFramePreparations;

            std::vector<const Image*> seen;
            for (const auto& surface : _frameSurfaces)
            {
                const auto key = surface.image.get();
                seen.push_back(key);
                const auto cached = _surfaceRevisions.find(key);
                if (cached == _surfaceRevisions.end())
                {
                    _surfaceRevisions.emplace(key, surface.revision);
                    ++_surfaceUploads;
                }
                else if (cached->second != surface.revision)
                {
                    cached->second = surface.revision;
                    ++_surfaceRefreshes;
                }
            }
            for (auto cached = _surfaceRevisions.begin(); cached != _surfaceRevisions.end();)
            {
                if (std::find(seen.begin(), seen.end(), cached->first) == seen.end())
                {
                    cached = _surfaceRevisions.erase(cached);
                    ++_surfaceEvictions;
                }
                else
                {
                    ++cached;
                }
            }
            return S_OK;
        }
        CATCH_RETURN()

        HRESULT BeginRowImages(const til::CoordType targetRow,
                               const til::CoordType /*viewportLeft*/,
                               const std::span<const uint8_t> defaultBackgroundMask,
                               const std::span<const COLORREF> cellBackgrounds) noexcept override
        try
        {
            const auto bufferRow = targetRow + _imageViewportOrigin.y;
            for (const auto& placement : _framePlacements)
            {
                const auto bounds = placement.CellBounds();
                if (bufferRow < bounds.top || bufferRow >= bounds.bottom)
                {
                    continue;
                }
                const auto surface = std::ranges::find(_frameSurfaces, placement.SurfacePointer().get(), [](const auto& candidate) {
                    return candidate.image.get();
                });
                THROW_HR_IF(E_UNEXPECTED, surface == _frameSurfaces.end() || !surface->pixels);

                PaintedImage image{
                    .targetRow = targetRow,
                    .columnOffset = bounds.left - _imageViewportOrigin.x,
                    .key = placement.Identity(),
                    .surface = &placement.Surface(),
                    .revision = surface->revision,
                    .surfaceSize = surface->size,
                    .bounds = bounds,
                    .originalBounds = placement.OriginalCellBounds(),
                    .source = placement.SourceInPixels(),
                    .geometry = placement.Geometry(),
                    .zIndex = placement.ZIndex(),
                    .position = placement.Position(),
                };
                image.defaultBackgroundMask.assign(defaultBackgroundMask.begin(), defaultBackgroundMask.end());
                image.cellBackgrounds.assign(cellBackgrounds.begin(), cellBackgrounds.end());
                for (const auto& pixel : *surface->pixels)
                {
                    image.containsRed |= pixel.rgbRed == 255 && pixel.rgbGreen == 0 && pixel.rgbBlue == 0;
                    image.containsGreen |= pixel.rgbRed == 0 && pixel.rgbGreen == 255 && pixel.rgbBlue == 0;
                }
                _images.emplace_back(std::move(image));
            }
            return S_OK;
        }
        CATCH_RETURN()

        HRESULT EndRowImages() noexcept override
        {
            return S_OK;
        }

        HRESULT PaintSelection(const til::rect& /*rect*/) noexcept override
        {
            return S_OK;
        }

        HRESULT PaintCursor(const CursorOptions& options) noexcept override
        {
            static_cast<void>(options);
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

        PaintedFrame Snapshot() const
        {
            const auto guard = _lock.lock_shared();
            return {
                _presentedClusters,
                _presentedImages,
                _presentedImageFramePreparations,
                _presentedSurfaceUploads,
                _presentedSurfaceRefreshes,
                _presentedSurfaceEvictions,
            };
        }

        void FailNextImagePreparation(const HRESULT result) noexcept
        {
            const auto guard = _lock.lock_exclusive();
            _nextImageFrameResult = result;
        }

        size_t ImageFramePreparationCalls() const noexcept
        {
            const auto guard = _lock.lock_shared();
            return _imageFramePreparationCalls;
        }

    protected:
        HRESULT _DoUpdateTitle(const std::wstring_view /*title*/) noexcept override
        {
            return S_OK;
        }

    private:
        mutable wil::srwlock _lock;
        til::rect _dirtyArea{ 0, 0, SHRT_MAX, SHRT_MAX };
        TextAttribute _currentAttribute;
        std::vector<PaintedCluster> _clusters;
        std::vector<PaintedImage> _images;
        std::vector<ImagePlacement> _framePlacements;
        std::vector<ImageFrameInfo::Surface> _frameSurfaces;
        til::point _imageViewportOrigin;
        HRESULT _nextImageFrameResult = S_OK;
        size_t _imageFramePreparationCalls = 0;
        std::unordered_map<const Image*, uint64_t> _surfaceRevisions;
        size_t _imageFramePreparations = 0;
        size_t _surfaceUploads = 0;
        size_t _surfaceRefreshes = 0;
        size_t _surfaceEvictions = 0;
        std::vector<PaintedCluster> _presentedClusters;
        std::vector<PaintedImage> _presentedImages;
        size_t _presentedImageFramePreparations = 0;
        size_t _presentedSurfaceUploads = 0;
        size_t _presentedSurfaceRefreshes = 0;
        size_t _presentedSurfaceEvictions = 0;
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

        // Painting normally happens on the renderer's own thread, but a test that starts
        // one is at the mercy of the scheduler: on a machine busy with I/O the thread can
        // fail to run at all for tens of seconds, and every wait for a frame then expires
        // for reasons that have nothing to do with what is being tested. Paint on the
        // calling thread instead. Nothing here needs a frame to arrive on its own -- the
        // tests write to the buffer, ask for a paint, and look at what the engine was
        // handed -- so the thread bought nothing but flakiness.
        void StartPainting()
        {
            Repaint();
        }

        void Repaint()
        {
            renderer.TriggerRedrawAll();
            THROW_IF_FAILED(renderer.PaintFrame());
        }

        KittyRecordingRenderEngine engine;
        Terminal terminal;
        DummyRenderer renderer;
    };

    constexpr std::wstring_view StoreRedKittyImage{ L"\x1b_Ga=T,U=1,i=1,f=24,s=1,v=1,c=1,r=1,q=2;/wAA\x1b\\" };
    constexpr std::wstring_view SelectKittyImageAndBackground{ L"\x1b[38;2;0;0;1;48;2;4;5;6m" };
    constexpr std::wstring_view KittyPlaceholder{ L"\xDBFB\xDEEE\x0305\x0305\x0305" };
    constexpr std::wstring_view OrdinaryCombiningText{ L"A\x0301" };
    // A 4x1 red image drawn across four columns, between the background and the text.
    constexpr std::wstring_view PlaceRedKittyImageBehindText{ L"\x1b_Ga=T,f=24,s=4,v=1,c=4,r=1,z=-1,q=2;/wAA/wAA/wAA/wAA\x1b\\" };

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
            if (image.targetRow == screenPosition.y &&
                image.key.imageId == imageId &&
                screenPosition.x >= image.columnOffset &&
                screenPosition.x < image.columnOffset + image.bounds.width() &&
                image.containsRed)
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
            return image.key.imageId == imageId &&
                   (green ? image.containsGreen : image.containsRed);
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
        TEST_METHOD(ImageDecodePolicyEnforcesContainerAndFrameCount);
        TEST_METHOD(ImageDecodeRejectsMalformedAndTruncatedData);
        TEST_METHOD(Iterm2ImgcatLegacyMultipartAndDividerRender);

        TEST_METHOD(DirectImageRendererPreservesSharedSurfaceGeometryAndLifetime);
        TEST_METHOD(KittyAnimationSurvivesFontAndRendererRefresh);
        TEST_METHOD(KittyPlaceholderRendersInRealTerminal);
        TEST_METHOD(KittyImageRowCarriesEachCellsBackgroundColor);
        TEST_METHOD(RendererRetriesImagePreparationFailure);
        TEST_METHOD(KittyPlaceholderSuppressesGlyphOnPaintAndRepaint);
        TEST_METHOD(KittyPlaceholderLeavesUnrecognizedTextUntouched);
        TEST_METHOD(KittyPlaceholderSuppressesGlyphAfterResize);
        TEST_METHOD(KittyPlaceholderSuppressesGlyphAfterScroll);
        // Kitty graphics file-transmission host I/O (t=f / t=t). These exercise the real
        // filesystem path of Terminal::ReadLocalFile (the shared til::read_file_as_bytes
        // helper), including the security gates that the adapter-level mock cannot cover.
        TEST_METHOD(ReadLocalFileReadsFromTemp);
        TEST_METHOD(ReadLocalFileMarkerGatesDeletion);
        TEST_METHOD(ReadLocalFileOffsetAndSizeSlice);
        TEST_METHOD(ReadLocalFileOversizeSizeClampsToEof);
        TEST_METHOD(ReadLocalFileRejectsInvalidPaths);
        TEST_METHOD(ReadLocalFileNormalizesWin32Path);
        TEST_METHOD(ReadLocalFileRejectsIntermediateJunction);
        TEST_METHOD(ReadLocalFileRejectsCharDevice);
        TEST_METHOD(ReadLocalFileCapsAtMaxBytes);
        TEST_METHOD(ReadLocalFileFailedReadKeepsTempFile);

        // Kitty graphics shared-memory host I/O (t=s).
        TEST_METHOD(ReadSharedMemoryCopiesSliceAndCloses);
        TEST_METHOD(ReadSharedMemoryCapsAtMaxBytes);
        TEST_METHOD(ReadSharedMemoryRejectsUnsafeNames);
        TEST_METHOD(ReadSharedMemoryMissingFails);
        TEST_METHOD(ReadSharedMemoryRejectsOffsetPastEnd);
        TEST_METHOD(ReadSharedMemoryRejectsReservedPages);
        TEST_METHOD(ReadSharedMemoryRejectsPartiallyCommittedMapping);
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

void TerminalApiTest::ImageDecodePolicyEnforcesContainerAndFrameCount()
{
    struct Fixture
    {
        std::string_view encoded;
        GUID container;
    };
    const std::array staticFixtures{
        Fixture{ "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFUlEQVR4nGP8z8Dwn4GBgYEJRIAwAB8XAgICR7MUAAAAAElFTkSuQmCC", GUID_ContainerFormatPng },
        Fixture{ "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/2wBDAQkJCQwLDBgNDRgyIRwhMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjIyMjL/wAARCAACAAIDASIAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwDi6KKK+ZP3E//Z", GUID_ContainerFormatJpeg },
        Fixture{ "Qk1GAAAAAAAAADYAAAAoAAAAAgAAAAIAAAABACAAAAAAABAAAADEDgAAxA4AAAAAAAAAAAAAAAD//wAA//8AAP//AAD//w==", GUID_ContainerFormatBmp },
        Fixture{ "R0lGODdhAgACAIEAAP8AAAAAAAAAAAAAACwAAAAAAgACAAAIBgABCAQQEAA7", GUID_ContainerFormatGif },
        Fixture{ "SUkqAAgAAAALAAABBAABAAAAAgAAAAEBBAABAAAAAgAAAAIBAwAEAAAAkgAAAAMBAwABAAAAAQAAAAYBAwABAAAAAgAAABEBBAABAAAAmgAAABUBAwABAAAABAAAABYBBAABAAAAAgAAABcBBAABAAAAEAAAABwBAwABAAAAAQAAAFIBAwABAAAAAgAAAAAAAAAIAAgACAAIAP8AAP//AAD//wAA//8AAP8=", GUID_ContainerFormatTiff },
    };

    Terminal terminal{ Terminal::TestDummyMarker{} };
    for (const auto& fixture : staticFixtures)
    {
        const auto data = DecodeFixture(fixture.encoded);
        const auto iterm2 = terminal.DecodeImageToBgra(data, ITerminalApi::ImageDecodePolicy::Iterm2SingleFrame);
        VERIFY_IS_TRUE(static_cast<bool>(iterm2));
        VERIFY_ARE_EQUAL(uint32_t{ 1 }, iterm2.frameCount);
        VERIFY_IS_TRUE(iterm2.containerFormat == fixture.container);
        VERIFY_ARE_EQUAL((til::size{ 2, 2 }), iterm2.size);
        VERIFY_ARE_EQUAL(size_t{ 4 }, iterm2.pixels.size());

        const auto kitty = terminal.DecodeImageToBgra(data, ITerminalApi::ImageDecodePolicy::KittyPng);
        VERIFY_ARE_EQUAL(fixture.container == GUID_ContainerFormatPng, static_cast<bool>(kitty));
    }

    static constexpr std::array animatedFixtures{
        "R0lGODlhAgACAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQACgAAACwAAAAAAgACAAAIBgABCAQQEAAh+QQBCgABACwAAAAAAgACAIEA/wAAAAAAAAAAAAAIBgABCAQQEAA7"sv,
        "SUkqAAgAAAALAAABBAABAAAAAgAAAAEBBAABAAAAAgAAAAIBAwAEAAAAkgAAAAMBAwABAAAAAQAAAAYBAwABAAAAAgAAABEBBAABAAAAmgAAABUBAwABAAAABAAAABYBBAABAAAAAgAAABcBBAABAAAAEAAAABwBAwABAAAAAQAAAFIBAwABAAAAAgAAALgAAAAIAAgACAAIAP8AAP//AAD//wAA//8AAP8AAAAAAABJSSoACAAAAAsAAAEEAAEAAAACAAAAAQEEAAEAAAACAAAAAgEDAAQAAABCAQAAAwEDAAEAAAABAAAABgEDAAEAAAACAAAAEQEEAAEAAABKAQAAFQEDAAEAAAAEAAAAFgEEAAEAAAACAAAAFwEEAAEAAAAQAAAAHAEDAAEAAAABAAAAUgEDAAEAAAACAAAAAAAAAAgACAAIAAgAAP8A/wD/AP8A/wD/AP8A/wAAAAAAAA=="sv,
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAACGFjVEwAAAACAAAAAPONk3AAAAAaZmNUTAAAAAAAAAACAAAAAgAAAAAAAAAAAAEACgAA6FTcAAAAABVJREFUeJxj/M/A8J+BgYGBCUSAMAAfFwICAkezFAAAABpmY1RMAAAAAQAAAAIAAAACAAAAAAAAAAAAAQAKAABzJzbUAAAAGWZkQVQAAAACeJxjZPjP8J+BgYGBCUSAMAAeGAICRb04jgAAAABJRU5ErkJggg=="sv,
    };
    for (const auto encoded : animatedFixtures)
    {
        const auto result = terminal.DecodeImageToBgra(DecodeFixture(encoded), ITerminalApi::ImageDecodePolicy::Iterm2SingleFrame);
        VERIFY_IS_TRUE(result.status == ITerminalApi::ImageDecodeResult::Status::MultipleFrames);
        VERIFY_IS_TRUE(result.frameCount > 1);
        VERIFY_IS_TRUE(result.pixels.empty());
    }
}

void TerminalApiTest::ImageDecodeRejectsMalformedAndTruncatedData()
{
    Terminal terminal{ Terminal::TestDummyMarker{} };
    const std::array malformed{
        std::vector<uint8_t>{ 0, 1, 2, 3 },
        DecodeFixture("iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFUlEQVR4nGP8z8Dwn4GBgYEJRIAwAB8XAgICR7MUAAAAAElFTkSuQmCC"),
    };
    auto truncated = malformed[1];
    truncated.resize(truncated.size() / 2);

    for (const auto& data : std::array{ malformed[0], truncated })
    {
        const auto result = terminal.DecodeImageToBgra(data, ITerminalApi::ImageDecodePolicy::Iterm2SingleFrame);
        VERIFY_IS_FALSE(static_cast<bool>(result));
        VERIFY_IS_TRUE(result.pixels.empty());
    }

    auto oversized = DecodeFixture("SUkqAAgAAAALAAABBAABAAAAAgAAAAEBBAABAAAAAgAAAAIBAwAEAAAAkgAAAAMBAwABAAAAAQAAAAYBAwABAAAAAgAAABEBBAABAAAAmgAAABUBAwABAAAABAAAABYBBAABAAAAAgAAABcBBAABAAAAEAAAABwBAwABAAAAAQAAAFIBAwABAAAAAgAAAAAAAAAIAAgACAAIAP8AAP//AAD//wAA//8AAP8=");
    const std::array oversizedDimension{ uint8_t{ 0x01 }, uint8_t{ 0x20 }, uint8_t{ 0x00 }, uint8_t{ 0x00 } };
    std::copy(oversizedDimension.begin(), oversizedDimension.end(), oversized.begin() + 18);
    std::copy(oversizedDimension.begin(), oversizedDimension.end(), oversized.begin() + 30);
    const auto oversizedResult = terminal.DecodeImageToBgra(oversized, ITerminalApi::ImageDecodePolicy::Iterm2SingleFrame);
    VERIFY_IS_FALSE(static_cast<bool>(oversizedResult));
    VERIFY_IS_TRUE(oversizedResult.pixels.empty());
}

void TerminalApiTest::Iterm2ImgcatLegacyMultipartAndDividerRender()
{
    static constexpr std::wstring_view image{ L"iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAFUlEQVR4nGP8z8Dwn4GBgYEJRIAwAB8XAgICR7MUAAAAAElFTkSuQmCC" };
    KittyRenderFixture fixture{ { 20, 10 }, 0 };
    auto& stateMachine = *fixture.terminal._stateMachine;

    // Current official imgcat --legacy framing.
    stateMachine.ProcessString(L"\x1b]1337;File=inline=1;width=2;height=2:" + std::wstring{ image } + L"\x1b\\\r\n");

    // Current official imgcat multipart framing (one FilePart here because the
    // fixture is smaller than imgcat's 200-character chunk size).
    stateMachine.ProcessString(L"\x1b]1337;MultipartFile=inline=1;width=2;height=2\x1b\\");
    stateMachine.ProcessString(L"\x1b]1337;FilePart=" + std::wstring{ image } + L"\x1b\\");
    stateMachine.ProcessString(L"\x1b]1337;FileEnd\x1b\\\r\n");

    // The official divider example stretches one image row across the grid.
    stateMachine.ProcessString(L"\x1b]1337;File=inline=1;width=100%;height=1;preserveAspectRatio=0:" + std::wstring{ image } + L"\a\r\n");

    fixture.StartPainting();
    const auto frame = fixture.engine.Snapshot();
    VERIFY_ARE_EQUAL(size_t{ 5 }, frame.images.size());
    VERIFY_IS_TRUE(std::ranges::all_of(frame.images, [](const auto& painted) {
        return painted.key.protocol == ImagePlacement::Key::Protocol::Iterm2 &&
               painted.containsRed;
    }));

    std::unordered_set<uint64_t> layers;
    for (const auto& painted : frame.images)
    {
        layers.emplace(painted.key.layerId);
    }
    VERIFY_ARE_EQUAL(size_t{ 3 }, layers.size());

    const auto divider = std::ranges::find_if(frame.images, [](const auto& painted) {
        return painted.bounds.width() == 20;
    });
    VERIFY_IS_TRUE(divider != frame.images.end());
    if (divider != frame.images.end())
    {
        VERIFY_ARE_EQUAL(til::CoordType{ 1 }, divider->bounds.height());
        VERIFY_ARE_EQUAL(uint64_t{ 200 }, divider->geometry.targetWidth);
        VERIFY_ARE_EQUAL(uint64_t{ 20 }, divider->geometry.targetHeight);
    }
}

void TerminalApiTest::DirectImageRendererPreservesSharedSurfaceGeometryAndLifetime()
{
    KittyRenderFixture fixture{ { 6, 3 }, 0 };
    auto& buffer = *fixture.terminal._mainBuffer;

    auto pixels = std::make_shared<std::vector<RGBQUAD>>(8, RGBQUAD{ 0, 0, 128, 128 });
    pixels->at(1) = RGBQUAD{ 0, 0, 255, 255 };
    const auto surface = std::make_shared<Image>(til::size{ 4, 2 }, pixels);
    const ImagePlacement::PixelGeometry croppedGeometry{
        .cellSize = { 10, 20 },
        .targetWidth = 40,
        .targetHeight = 40,
        .offset = { 2, 3 },
    };
    const ImagePlacement::PixelGeometry fullGeometry{
        .cellSize = { 10, 20 },
        .targetWidth = 20,
        .targetHeight = 40,
    };

    fixture.terminal.LockConsole();
    auto unlock = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    auto& images = buffer.GetMutableImages();
    images.AddOrReplace(ImagePlacement{
        { 9, 100 },
        surface,
        { 0, 1, 2, 3 },
        ImagePlacement::BackgroundZThreshold - 1,
        { 1, 0, 3, 2 },
        croppedGeometry,
    });
    images.AddOrReplace(ImagePlacement{
        { 9, 101 },
        surface,
        { 2, 0, 4, 2 },
        -2,
        { 0, 0, 4, 2 },
        fullGeometry,
    });
    images.AddOrReplaceArea(ImagePlacement::FromFragment(
        { 9, 102 },
        surface,
        { 4, 0, 5, 1 },
        { 4, 0, 6, 2 },
        5,
        { 0, 0, 4, 2 },
        fullGeometry));
    images.AddOrReplaceArea(ImagePlacement::FromFragment(
        { 9, 102 },
        surface,
        { 5, 1, 6, 2 },
        { 4, 0, 6, 2 },
        5,
        { 0, 0, 4, 2 },
        fullGeometry));
    unlock.reset();

    fixture.StartPainting();
    auto frame = fixture.engine.Snapshot();
    VERIFY_ARE_EQUAL(size_t{ 1 }, frame.imageFramePreparations, L"placements must be queried once per engine frame");
    VERIFY_ARE_EQUAL(size_t{ 1 }, frame.surfaceUploads, L"all placements and fragments must share one complete upload");
    VERIFY_ARE_EQUAL(size_t{ 0 }, frame.surfaceRefreshes);
    VERIFY_ARE_EQUAL(static_cast<BYTE>(128), surface->Pixels().front().rgbReserved, L"surface alpha must remain intact");
    VERIFY_IS_TRUE(std::ranges::all_of(frame.images, [&](const auto& image) {
        return image.surface == surface.get();
    }));

    const auto cropped = std::ranges::find_if(frame.images, [](const auto& image) {
        return image.key.layerId == 100;
    });
    VERIFY_IS_TRUE(cropped != frame.images.end());
    if (cropped != frame.images.end())
    {
        VERIFY_ARE_EQUAL((til::rect{ 1, 0, 3, 2 }), cropped->source);
        VERIFY_ARE_EQUAL(uint64_t{ 40 }, cropped->geometry.targetWidth);
        VERIFY_ARE_EQUAL(uint64_t{ 40 }, cropped->geometry.targetHeight);
        VERIFY_ARE_EQUAL((til::point{ 2, 3 }), cropped->geometry.offset);
        VERIFY_ARE_EQUAL(static_cast<int>(ImagePlacement::RenderPosition::BehindBackground), static_cast<int>(cropped->position));
    }

    const auto fragmentCount = std::ranges::count_if(frame.images, [](const auto& image) {
        return image.key.layerId == 102;
    });
    VERIFY_ARE_EQUAL(ptrdiff_t{ 2 }, fragmentCount, L"disjoint fragments must reuse one placement surface");
    std::vector<int32_t> rowOneZ;
    for (const auto& image : frame.images)
    {
        if (image.targetRow == 1)
        {
            rowOneZ.push_back(image.zIndex);
        }
    }
    VERIFY_IS_TRUE(std::ranges::is_sorted(rowOneZ), L"the renderer must preserve stable z composition across all three phases");

    fixture.terminal.LockConsole();
    auto unlockAfterScroll = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    buffer.GetMutableImages().AdvanceRows(1, buffer.GetSize().Height());
    unlockAfterScroll.reset();
    fixture.Repaint();
    frame = fixture.engine.Snapshot();
    VERIFY_ARE_EQUAL(size_t{ 0 }, frame.surfaceUploads, L"epoch scrolling must reuse the cached surface");
    const auto scrolledFragment = std::ranges::find_if(frame.images, [](const auto& image) {
        return image.key.layerId == 102;
    });
    VERIFY_IS_TRUE(scrolledFragment != frame.images.end());
    if (scrolledFragment != frame.images.end())
    {
        VERIFY_ARE_EQUAL(0, scrolledFragment->bounds.top);
        VERIFY_ARE_EQUAL(-1, scrolledFragment->originalBounds.top, L"logical bounds must follow the monotonic row epoch");
    }

    auto changedPixels = std::make_shared<std::vector<RGBQUAD>>(15);
    changedPixels->front() = RGBQUAD{ 0, 255, 0, 192 };
    fixture.terminal.LockConsole();
    auto unlockAfterRevision = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    surface->UpdatePixels({ 5, 3 }, std::move(changedPixels));
    unlockAfterRevision.reset();
    fixture.Repaint();
    frame = fixture.engine.Snapshot();
    VERIFY_ARE_EQUAL(size_t{ 0 }, frame.surfaceUploads);
    VERIFY_ARE_EQUAL(size_t{ 1 }, frame.surfaceRefreshes, L"a new revision must refresh the existing cached surface");
    VERIFY_IS_TRUE(std::ranges::all_of(frame.images, [](const auto& image) {
                       return image.surfaceSize == til::size{ 5, 3 };
                   }),
                   L"renderer frames must retain dimensions with their immutable pixel snapshot");

    fixture.terminal.LockConsole();
    auto unlockAfterDelete = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    buffer.GetMutableImages().Clear();
    unlockAfterDelete.reset();
    fixture.Repaint();
    frame = fixture.engine.Snapshot();
    VERIFY_IS_TRUE(frame.images.empty());
    VERIFY_ARE_EQUAL(size_t{ 1 }, frame.surfaceEvictions, L"deletion must release the backend cache entry");
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
    auto frame = fixture.engine.Snapshot();
    VERIFY_IS_TRUE(FrameContainsKittyColor(frame, 7, false), L"frame 1 must be visible before the lifecycle transition");
    VERIFY_ARE_EQUAL(size_t{ 1 }, frame.imageFramePreparations);
    VERIFY_ARE_EQUAL(size_t{ 1 }, frame.surfaceUploads, L"the complete image must upload once per shared surface");

    const auto placements = buffer.GetImages().All();
    VERIFY_ARE_EQUAL(size_t{ 1 }, placements.size());
    const auto surface = placements.front().SurfacePointer();
    VERIFY_IS_NOT_NULL(surface.get());

    // Simulate an out-of-date backend-facing surface while the animation source of
    // truth remains frame 1 (red). A font refresh must update the same shared object.
    auto stalePixels = std::make_shared<std::vector<RGBQUAD>>(1, RGBQUAD{ 0, 255, 0, 0 });
    fixture.terminal.LockConsole();
    auto unlockAfterStaleSurface = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    surface->UpdatePixels(std::move(stalePixels));
    unlockAfterStaleSurface.reset();
    fixture.Repaint();
    frame = fixture.engine.Snapshot();
    VERIFY_IS_TRUE(FrameContainsKittyColor(frame, 7, true), L"the test must reproduce a stale shared animation surface");
    VERIFY_ARE_EQUAL(size_t{ 0 }, frame.surfaceUploads);
    VERIFY_ARE_EQUAL(size_t{ 1 }, frame.surfaceRefreshes, L"a revision change must refresh rather than allocate another surface");

    fixture.terminal.LockConsole();
    auto unlockAfterFontChange = wil::scope_exit([&fixture]() {
        fixture.terminal.UnlockConsole();
    });
    fixture.terminal.SetFontInfo(FontInfo{ DEFAULT_FONT_FACE, TMPF_TRUETYPE, 10, { 12, 24 }, CP_UTF8, false });
    unlockAfterFontChange.reset();
    fixture.Repaint();
    frame = fixture.engine.Snapshot();
    VERIFY_IS_TRUE(FrameContainsKittyColor(frame, 7, false), L"font/DPI recreation must restore the current animation frame");
    if (!frame.images.empty())
    {
        VERIFY_ARE_EQUAL(surface.get(), frame.images.front().surface, L"refresh must retain the complete shared surface");
    }
    VERIFY_ARE_EQUAL(size_t{ 1 }, frame.surfaceRefreshes);

    fixture.terminal.LockConsole();
    stateMachine.ProcessString(L"\x1b_Ga=a,i=7,r=1,z=20,q=2;\x1b\\");
    fixture.terminal.UnlockConsole();
    // Painting ticks the renderer's timers, so the frame the animation is waiting to
    // show arrives by painting again once its gap has elapsed -- no render thread, and
    // no waiting on one to be scheduled. The gap is 20ms; sleep a little and repaint
    // until the next frame appears.
    auto advanced = FrameContainsKittyColor(fixture.engine.Snapshot(), 7, true);
    for (auto attempt = 0; attempt < 50 && !advanced; ++attempt)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        fixture.Repaint();
        advanced = FrameContainsKittyColor(fixture.engine.Snapshot(), 7, true);
    }
    VERIFY_IS_TRUE(advanced, L"animation playback must continue to the next frame after recreation");
}

// --- Kitty graphics file-transmission host I/O ------------------------------------
//
// These build a Terminal in TestDummyMarker mode and call ReadLocalFile against
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

    wil::unique_handle KittyCreateMapping(const std::wstring& name,
                                          const uint64_t size,
                                          const std::span<const uint8_t> content = {},
                                          const DWORD protection = PAGE_READWRITE)
    {
        wil::unique_handle mapping{ CreateFileMappingW(INVALID_HANDLE_VALUE,
                                                       nullptr,
                                                       protection,
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

    // What the adapter passes for t=t: only a temporary file carrying this in its
    // name may be deleted after a successful read.
    constexpr std::wstring_view TempFileMarker{ L"tty-graphics-protocol" };
}

void TerminalApiTest::ReadLocalFileReadsFromTemp()
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
    VERIFY_IS_TRUE(til::read_file_result::ok == term.ReadLocalFile(path, 0, 0, false, TempFileMarker, out), L"a readable local temp file must succeed");
    VERIFY_ARE_EQUAL(content.size(), out.size());
    VERIFY_IS_TRUE(out == content, L"the bytes read must equal the file contents");
    VERIFY_IS_TRUE(KittyFileExists(path), L"deleteAfter=false must not remove the file");

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadLocalFileMarkerGatesDeletion()
{
    // A t=t read only deletes the file it read when the file is under the system temp
    // directory AND its name carries the "tty-graphics-protocol" marker AND the caller
    // passes that same marker. Every other combination must leave the file in place --
    // the gate exists so a client cannot turn "read this file" into "delete that file".

    // Under temp, marked name, marker passed: the file is deleted.
    {
        Terminal term{ Terminal::TestDummyMarker{} };
        DummyRenderer renderer{ &term };
        term.Create({ 100, 100 }, 0, renderer);

        const auto dir = KittyTempDir();
        VERIFY_IS_FALSE(dir.empty());
        const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-del-");
        VERIFY_IS_TRUE(KittyWriteAllBytes(path, { 9, 8, 7 }));

        std::vector<uint8_t> out;
        VERIFY_IS_TRUE(til::read_file_result::ok == term.ReadLocalFile(path, 0, 0, true, TempFileMarker, out), L"the read should succeed");
        VERIFY_ARE_EQUAL(static_cast<size_t>(3), out.size());
        VERIFY_IS_FALSE(KittyFileExists(path), L"a temp file carrying the kitty marker must be deleted after a t=t read");

        if (KittyFileExists(path))
        {
            DeleteFileW(path.c_str());
        }
    }

    // Outside the temp dir: never deleted, even with the marker and deleteAfter.
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
        VERIFY_IS_TRUE(til::read_file_result::ok == term.ReadLocalFile(path, 0, 0, true, TempFileMarker, out), L"the read should still succeed");
        VERIFY_IS_TRUE(KittyFileExists(path), L"a file OUTSIDE the temp dir must never be deleted, even with deleteAfter (anti arbitrary-delete)");

        DeleteFileW(path.c_str());
    }

    // Under temp but the name lacks the marker: not deleted.
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
        VERIFY_IS_TRUE(til::read_file_result::ok == term.ReadLocalFile(path, 0, 0, true, TempFileMarker, out), L"the read should succeed");
        VERIFY_IS_TRUE(KittyFileExists(path), L"a temp file WITHOUT the kitty marker must not be auto-deleted");

        DeleteFileW(path.c_str());
    }

    // Marked name under temp, but the caller passes an EMPTY marker: deletes nothing.
    {
        Terminal term{ Terminal::TestDummyMarker{} };
        DummyRenderer renderer{ &term };
        term.Create({ 100, 100 }, 0, renderer);

        const auto dir = KittyTempDir();
        VERIFY_IS_FALSE(dir.empty());
        // Named exactly the way a caller that DOES pass a marker would name it, so the only
        // thing standing between this file and deletion is the empty marker below.
        const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-nomarker-");
        VERIFY_IS_TRUE(KittyWriteAllBytes(path, { 1, 1, 1 }));

        std::vector<uint8_t> out;
        VERIFY_IS_TRUE(til::read_file_result::ok == term.ReadLocalFile(path, 0, 0, true, L"", out), L"the read should succeed");
        VERIFY_IS_TRUE(KittyFileExists(path), L"an empty marker must delete nothing: a caller cannot opt out of the name check by omitting it");

        DeleteFileW(path.c_str());
    }

    // The marker on a PARENT directory does not authorize deleting a file inside it.
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
        VERIFY_IS_TRUE(til::read_file_result::ok == term.ReadLocalFile(path, 0, 0, true, TempFileMarker, out), L"the read should succeed");
        VERIFY_IS_TRUE(KittyFileExists(path), L"a marker in a parent directory must not authorize deleting an unrelated file");

        DeleteFileW(path.c_str());
        RemoveDirectoryW(subdir.c_str());
    }
}

void TerminalApiTest::ReadLocalFileOffsetAndSizeSlice()
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
    VERIFY_IS_TRUE(til::read_file_result::ok == term.ReadLocalFile(path, 2, 3, false, TempFileMarker, out), L"offset+size read should succeed");
    const std::vector<uint8_t> expected{ 'C', 'D', 'E' };
    VERIFY_ARE_EQUAL(expected.size(), out.size());
    VERIFY_IS_TRUE(out == expected, L"O=2,S=3 must read exactly bytes [2,5) of the file");

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadLocalFileOversizeSizeClampsToEof()
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
    VERIFY_IS_TRUE(til::read_file_result::ok == term.ReadLocalFile(path, 0, 100ull * 1024 * 1024, false, TempFileMarker, out), L"read should succeed");
    VERIFY_ARE_EQUAL(content.size(), out.size(), L"an oversize S= must clamp to EOF (the file size)");
    VERIFY_IS_TRUE(out == content);

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadLocalFileRejectsInvalidPaths()
{
    // A path that is malformed or does not name a readable regular file is refused before
    // any bytes are returned, and a rejected read always leaves the output empty. Each
    // class of bad path reports its own result code.

    // A UNC path is rejected up front (no SMB connection, no NTLM handshake): invalid.
    {
        Terminal term{ Terminal::TestDummyMarker{} };
        DummyRenderer renderer{ &term };
        term.Create({ 100, 100 }, 0, renderer);

        std::vector<uint8_t> out{ 7, 7, 7 };
        VERIFY_IS_TRUE(til::read_file_result::invalid == term.ReadLocalFile(L"\\\\127.0.0.1\\share\\never.bin", 0, 0, false, TempFileMarker, out), L"a UNC path must be rejected as an invalid request");
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size(), L"a rejected read must leave the output empty");
    }

    // A relative path is rejected so a client cannot read a file resolved against the
    // terminal's own working directory: invalid.
    {
        Terminal term{ Terminal::TestDummyMarker{} };
        DummyRenderer renderer{ &term };
        term.Create({ 100, 100 }, 0, renderer);

        std::vector<uint8_t> out;
        VERIFY_IS_TRUE(til::read_file_result::invalid == term.ReadLocalFile(L"relative\\file.bin", 0, 0, false, TempFileMarker, out), L"a non-absolute path must be rejected as an invalid request");
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
    }

    // A well-formed absolute path that names no file reports not_found (ENOENT).
    {
        Terminal term{ Terminal::TestDummyMarker{} };
        DummyRenderer renderer{ &term };
        term.Create({ 100, 100 }, 0, renderer);

        const auto dir = KittyTempDir();
        VERIFY_IS_FALSE(dir.empty());
        // A unique path we never create.
        const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-missing-");

        std::vector<uint8_t> out;
        VERIFY_IS_TRUE(til::read_file_result::not_found == term.ReadLocalFile(path, 0, 0, false, TempFileMarker, out), L"a missing file must report not_found (ENOENT)");
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
    }

    // A real, drive-absolute directory passes every pre-open path check but must not be
    // readable as an image. FILE_NON_DIRECTORY_FILE reports it as read_error (EBADF).
    {
        Terminal term{ Terminal::TestDummyMarker{} };
        DummyRenderer renderer{ &term };
        term.Create({ 100, 100 }, 0, renderer);

        const auto dir = KittyTempDir();
        VERIFY_IS_FALSE(dir.empty());
        const auto subdir = KittyUniquePath(dir, L"tty-graphics-protocol-dir-");
        VERIFY_IS_TRUE(CreateDirectoryW(subdir.c_str(), nullptr) != FALSE, L"failed to create the test directory");

        std::vector<uint8_t> out{ 1, 2, 3 };
        VERIFY_IS_TRUE(til::read_file_result::read_error == term.ReadLocalFile(subdir, 0, 0, false, TempFileMarker, out), L"a directory must be refused as unreadable (EBADF): only regular files may be read");
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size(), L"a rejected read must leave the output empty");

        RemoveDirectoryW(subdir.c_str());
    }
}

void TerminalApiTest::ReadLocalFileNormalizesWin32Path()
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
    VERIFY_IS_TRUE(til::read_file_result::ok == term.ReadLocalFile(equivalentPath, 0, 0, false, TempFileMarker, out), L"Win32-equivalent dot segments, duplicate separators, and a trailing period must resolve to the same file");
    VERIFY_IS_TRUE(out == content);

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadLocalFileRejectsIntermediateJunction()
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
    VERIFY_IS_TRUE(til::read_file_result::read_error == term.ReadLocalFile(junctionFile, 0, 0, false, TempFileMarker, out), L"an intermediate reparse point must be rejected before its target is opened");
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
}

void TerminalApiTest::ReadLocalFileRejectsCharDevice()
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
        VERIFY_IS_TRUE(til::read_file_result::read_error == term.ReadLocalFile(devicePath, 0, 0, false, TempFileMarker, out), L"a DOS character device must be refused as unreadable (EBADF): only regular files may be read");
        VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size(), L"a rejected read must leave the output empty");
    }
}

void TerminalApiTest::ReadLocalFileCapsAtMaxBytes()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto dir = KittyTempDir();
    VERIFY_IS_FALSE(dir.empty());
    const auto path = KittyUniquePath(dir, L"tty-graphics-protocol-huge-");

    // The 32 MiB hard cap in til::read_file_as_bytes. Kept in sync by intent; a mismatch here
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
    VERIFY_IS_TRUE(til::read_file_result::ok == term.ReadLocalFile(path, 0, 0, false, TempFileMarker, out), L"reading a huge file must still succeed (clamped)");
    VERIFY_ARE_EQUAL(static_cast<size_t>(cap), out.size(), L"the read must be capped at 32 MiB regardless of the file's size");

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadLocalFileFailedReadKeepsTempFile()
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
    VERIFY_IS_TRUE(til::read_file_result::invalid == term.ReadLocalFile(path, 1000, 0, true, TempFileMarker, out), L"an offset past EOF must fail as an invalid request");
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size(), L"a failed read must leave the output empty");
    VERIFY_IS_TRUE(KittyFileExists(path), L"a failed t=t read must NOT delete its target, even a marked temp file");

    DeleteFileW(path.c_str());
}

void TerminalApiTest::ReadSharedMemoryCopiesSliceAndCloses()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto name = KittyUniqueMappingName();
    const std::vector<uint8_t> content{ 'A', 'B', 'C', 'D', 'E', 'F' };
    auto mapping = KittyCreateMapping(name, content.size(), content);
    VERIFY_IS_TRUE(static_cast<bool>(mapping), L"failed to create the shared-memory test object");

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(Microsoft::Console::Utils::ReadSharedMemoryResult::ok == term.ReadSharedMemory(name, 2, 3, out));
    const std::vector<uint8_t> expected{ 'C', 'D', 'E' };
    VERIFY_IS_TRUE(out == expected, L"O=2,S=3 must copy exactly bytes [2,5)");

    // The protocol requires Windows terminals to close (not unlink) the object. Once
    // the creator closes its handle, no leaked terminal handle may keep the name alive.
    mapping.reset();
    wil::unique_handle reopened{ OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str()) };
    VERIFY_IS_FALSE(static_cast<bool>(reopened), L"the terminal must close its mapping handle before returning");
}

void TerminalApiTest::ReadSharedMemoryCapsAtMaxBytes()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    constexpr uint64_t cap = 32ull * 1024 * 1024;
    const auto name = KittyUniqueMappingName();
    auto mapping = KittyCreateMapping(name, cap + 1024 * 1024);
    VERIFY_IS_TRUE(static_cast<bool>(mapping), L"failed to create the oversize shared-memory object");

    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(Microsoft::Console::Utils::ReadSharedMemoryResult::ok == term.ReadSharedMemory(name, 0, 0, out));
    VERIFY_ARE_EQUAL(static_cast<size_t>(cap), out.size(), L"S=0 must still honor the 32 MiB hard cap");
}

void TerminalApiTest::ReadSharedMemoryRejectsUnsafeNames()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    std::vector<uint8_t> out{ 1, 2, 3 };
    VERIFY_IS_TRUE(Microsoft::Console::Utils::ReadSharedMemoryResult::invalid == term.ReadSharedMemory(L"Global\\service-object", 0, 1, out), L"cross-session Global objects must be unreachable");
    VERIFY_IS_TRUE(Microsoft::Console::Utils::ReadSharedMemoryResult::invalid == term.ReadSharedMemory(L"Session\\1\\object", 0, 1, out), L"system and nested object-manager paths must be rejected");
    VERIFY_IS_TRUE(Microsoft::Console::Utils::ReadSharedMemoryResult::invalid == term.ReadSharedMemory(L"Local\\nested\\object", 0, 1, out), L"Local names may not escape into nested namespaces");
    const std::wstring nulName{ L"Local\\foo\0bar", 13 };
    VERIFY_IS_TRUE(Microsoft::Console::Utils::ReadSharedMemoryResult::invalid == term.ReadSharedMemory(nulName, 0, 1, out), L"embedded NULs must not truncate the opened name");
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
}

void TerminalApiTest::ReadSharedMemoryMissingFails()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto name = KittyUniqueMappingName();
    std::vector<uint8_t> out;
    VERIFY_IS_TRUE(Microsoft::Console::Utils::ReadSharedMemoryResult::not_found == term.ReadSharedMemory(name, 0, 1, out));
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
}

void TerminalApiTest::ReadSharedMemoryRejectsOffsetPastEnd()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto name = KittyUniqueMappingName();
    auto mapping = KittyCreateMapping(name, 4096);
    VERIFY_IS_TRUE(static_cast<bool>(mapping));

    std::vector<uint8_t> out{ 1 };
    VERIFY_IS_TRUE(Microsoft::Console::Utils::ReadSharedMemoryResult::invalid == term.ReadSharedMemory(name, 128 * 1024, 1, out));
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
}

void TerminalApiTest::ReadSharedMemoryRejectsReservedPages()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    const auto name = KittyUniqueMappingName();
    auto mapping = KittyCreateMapping(name, 64 * 1024, {}, PAGE_READWRITE | SEC_RESERVE);
    VERIFY_IS_TRUE(static_cast<bool>(mapping), L"failed to create the reserved shared-memory object");

    std::vector<uint8_t> out{ 1 };
    VERIFY_IS_TRUE(Microsoft::Console::Utils::ReadSharedMemoryResult::read_error == term.ReadSharedMemory(name, 0, 1, out));
    VERIFY_ARE_EQUAL(static_cast<size_t>(0), out.size());
}

void TerminalApiTest::ReadSharedMemoryRejectsPartiallyCommittedMapping()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const auto pageSize = static_cast<uint64_t>(systemInfo.dwPageSize);
    const auto name = KittyUniqueMappingName();
    auto mapping = KittyCreateMapping(name, pageSize * 2, {}, PAGE_READWRITE | SEC_RESERVE);
    VERIFY_IS_TRUE(static_cast<bool>(mapping), L"failed to create the reserved shared-memory object");

    const auto view = MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, 0);
    VERIFY_IS_NOT_NULL(view);
    const auto unmap = wil::scope_exit([&]() noexcept { UnmapViewOfFile(view); });
    VERIFY_ARE_EQUAL(view, VirtualAlloc(view, gsl::narrow_cast<size_t>(pageSize), MEM_COMMIT, PAGE_READWRITE));
    *static_cast<uint8_t*>(view) = 42;

    std::vector<uint8_t> out{ 1 };
    VERIFY_IS_TRUE(Microsoft::Console::Utils::ReadSharedMemoryResult::read_error == term.ReadSharedMemory(name, 0, 0, out));
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

    const auto placements = tbi.GetImages().All();
    VERIFY_ARE_EQUAL(size_t{ 1 }, placements.size(), L"the placeholder must register one direct-renderer fragment");
    if (!placements.empty())
    {
        const auto& pixel = placements.front().Surface().Pixels().front();
        VERIFY_ARE_EQUAL(static_cast<BYTE>(255), pixel.rgbRed);
        VERIFY_ARE_EQUAL(static_cast<BYTE>(0), pixel.rgbGreen);
        VERIFY_ARE_EQUAL(static_cast<BYTE>(0), pixel.rgbBlue);
        VERIFY_ARE_EQUAL((til::rect{ 0, 0, 1, 1 }), placements.front().CellBounds());
    }
}

// An engine that withholds the text pass's background fill from the cells an image
// covers has to paint those backgrounds itself, or the image composites over the
// default background instead of the cell's own. That means the row's per-column
// background colors have to reach the engine, not just which of them are default.
void TerminalApiTest::KittyImageRowCarriesEachCellsBackgroundColor()
{
    KittyRenderFixture fixture{ { 8, 3 }, 0 };
    auto& stateMachine = *fixture.terminal._stateMachine;

    // Two cells with an explicit background, then two that keep the default one.
    stateMachine.ProcessString(L"\x1b[48;2;4;5;6m  \x1b[0m  ");
    stateMachine.ProcessString(L"\x1b[1;1H");
    stateMachine.ProcessString(PlaceRedKittyImageBehindText);

    fixture.StartPainting();
    const auto frame = fixture.engine.Snapshot();

    const auto painted = std::ranges::find(frame.images, til::CoordType{ 0 }, &PaintedImage::targetRow);
    VERIFY_IS_TRUE(painted != frame.images.end(), L"the image row was never handed to the engine");

    const std::vector<uint8_t> expectedMask{ 0, 0, 1, 1, 1, 1, 1, 1 };
    VERIFY_ARE_EQUAL(expectedMask, painted->defaultBackgroundMask, L"only the two cells with an explicit background are non-default");

    VERIFY_ARE_EQUAL(size_t{ 8 }, painted->cellBackgrounds.size(), L"the engine needs one color per visible column");
    VERIFY_ARE_EQUAL(RGB(4, 5, 6), painted->cellBackgrounds.at(0), L"a cell's own background color must survive to the engine");
    VERIFY_ARE_EQUAL(RGB(4, 5, 6), painted->cellBackgrounds.at(1), L"a cell's own background color must survive to the engine");
    VERIFY_ARE_EQUAL(painted->cellBackgrounds.at(2), painted->cellBackgrounds.at(3), L"the default-background cells must agree");
    VERIFY_ARE_NOT_EQUAL(RGB(4, 5, 6), painted->cellBackgrounds.at(2), L"a default cell must not inherit the colored one");
}

void TerminalApiTest::RendererRetriesImagePreparationFailure()
{
    KittyRenderFixture fixture{ { 8, 3 }, 0 };
    fixture.terminal._stateMachine->ProcessString(L"X");
    fixture.engine.FailNextImagePreparation(E_OUTOFMEMORY);

    fixture.StartPainting();

    VERIFY_ARE_EQUAL(size_t{ 2 }, fixture.engine.ImageFramePreparationCalls(), L"a failed image preparation must escape the engine frame and trigger the renderer retry path");
    const auto frame = fixture.engine.Snapshot();
    VERIFY_IS_TRUE(std::ranges::any_of(frame.clusters, [](const auto& cluster) { return cluster.text.find(L'X') != std::wstring::npos; }), L"the retry must re-invalidate content consumed by the aborted frame");
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
    const auto sixelSurface = std::make_shared<Image>(
        til::size{ 1, 1 },
        std::vector<RGBQUAD>{ RGBQUAD{ 255, 0, 0, 255 } });
    initialBuffer.GetMutableImages().Add(ImagePlacement{
        { 0, 1, ImagePlacement::Key::Protocol::Sixel },
        sixelSurface,
        { 0, 0, 1, 1 },
        0,
        {},
        {
            .cellSize = { 1, 1 },
            .targetWidth = 1,
            .targetHeight = 1,
        },
    });
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
    const auto resizedImages = resizedBuffer.GetImages().All();
    const auto resizedSixel = std::ranges::find_if(resizedImages, [](const ImagePlacement& placement) {
        return placement.Identity().protocol == ImagePlacement::Key::Protocol::Sixel;
    });
    VERIFY_IS_TRUE(resizedSixel != resizedImages.end(), L"relocating Kitty pixels must not drop the Sixel placement");
    VERIFY_ARE_EQUAL(sixelSurface.get(), resizedSixel->SurfacePointer().get());
    VERIFY_ARE_EQUAL(static_cast<BYTE>(255), resizedSixel->Surface().Pixels().front().rgbBlue);

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
    const auto widenedImages = widenedBuffer.GetImages().All();
    const auto widenedSixel = std::ranges::find_if(widenedImages, [](const ImagePlacement& placement) {
        return placement.Identity().protocol == ImagePlacement::Key::Protocol::Sixel;
    });
    VERIFY_IS_TRUE(widenedSixel != widenedImages.end(), L"joining wrapped rows must preserve the Sixel placement");
    VERIFY_ARE_EQUAL(sixelSurface.get(), widenedSixel->SurfacePointer().get());
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
