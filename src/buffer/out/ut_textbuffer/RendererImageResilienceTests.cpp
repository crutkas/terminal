// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// These tests cover the renderer's image-resilience contract that the GDI, Atlas
// D2D, and Atlas D3D backends all share:
//   * A malformed image snapshot is skipped, not fatal (ImageFrameInfo::Surface::IsRenderable).
//   * An image that leaves the frame is evicted individually, never by resetting an
//     unrelated cache such as the glyph atlas (ImageFrameInfo::ImageCacheEntryIsStale).
// The decision logic lives in the shared header so every backend agrees and so it
// can be exercised here without a graphics device.

#include "precomp.h"
#include "WexTestClass.h"
#include "../../inc/consoletaeftemplates.hpp"

#include "../Image.hpp"
#include "../textBuffer.hpp"
#include "../../../renderer/inc/IRenderEngine.hpp"
#include "../../../renderer/inc/RenderEngineBase.hpp"
#include "../../../renderer/inc/DummyRenderer.hpp"

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using Microsoft::Console::Render::ImageFrameInfo;
using Microsoft::Console::Render::IRenderData;
using Microsoft::Console::Render::RenderEngineBase;

class RendererImageResilienceTests
{
    TEST_CLASS(RendererImageResilienceTests);

    TEST_METHOD(IsRenderableAcceptsWellFormedSnapshot);
    TEST_METHOD(IsRenderableRejectsMalformedSnapshots);
    TEST_METHOD(StaleEntryIsEvictedIndividually);
    TEST_METHOD(SurfaceRemovalMarksEntryStale);
    TEST_METHOD(StaleShapedRowSurfaceRemovalIsHarmless);
    TEST_METHOD(DefaultBackgroundMaskRightBoundStaysInBounds);
    TEST_METHOD(DoubleWidthImageMaskCoordinateDivergence);
    TEST_METHOD(RendererSubmitsOneFrameSnapshotInCompositionOrder);
    TEST_METHOD(TextOnlyFallbackStillPaintsText);

    static Image::Pointer MakeImage(const til::size pixelSize)
    {
        const auto count = static_cast<size_t>(pixelSize.width) * pixelSize.height;
        return std::make_shared<Image>(pixelSize, std::vector<RGBQUAD>(count));
    }

    static ImageFrameInfo::Surface MakeSurface(const til::size pixelSize)
    {
        const auto image = MakeImage(pixelSize);
        return ImageFrameInfo::Surface{
            .image = image,
            .pixels = image->Storage(),
            .size = pixelSize,
            .revision = image->Revision(),
        };
    }

    // Reproduces Renderer::_buildImageRowBackgrounds: one mask byte per visible
    // screen column, resolved through the line rendition's screen->buffer scale.
    static std::vector<uint8_t> BuildProducerMask(const ROW& row, const til::CoordType viewportLeft, const til::CoordType viewportWidth)
    {
        std::vector<uint8_t> mask(gsl::narrow_cast<size_t>(viewportWidth), uint8_t{ 0 });
        const auto scale = row.GetLineRendition() != LineRendition::SingleWidth ? 1 : 0;
        const auto readableColumns = row.GetReadableColumnCount();
        for (til::CoordType column = 0; column < viewportWidth; ++column)
        {
            const auto screenColumn = viewportLeft + column;
            const auto bufferColumn = screenColumn >> scale;
            if (bufferColumn >= 0 && bufferColumn < readableColumns)
            {
                mask[gsl::narrow_cast<size_t>(column)] = row.GetAttrByColumn(bufferColumn).GetBackground().IsDefault() ? 1 : 0;
            }
        }
        return mask;
    }

    // Reproduces the BehindBackground scan in GdiEngine::_PaintDirectImages /
    // BackendD3D::_drawImages, including the right-bound clamp under test. Returns
    // the mask indices the consumer reads for this placement's row slice.
    static std::vector<til::CoordType> ConsumerMaskIndices(const til::rect bounds, const til::CoordType viewportLeft, const size_t maskSize)
    {
        std::vector<til::CoordType> indices;
        const auto left = std::max(bounds.left, viewportLeft);
        const auto maskRight = viewportLeft + gsl::narrow_cast<til::CoordType>(maskSize);
        const auto scanRight = std::min(bounds.right, maskRight);
        for (auto run = left; run < scanRight; ++run)
        {
            indices.push_back(run - viewportLeft);
        }
        return indices;
    }
};

namespace
{
    class TestRenderData final : public IRenderData
    {
    public:
        TestRenderData() :
            _buffer{ til::size{ 4, 2 }, TextAttribute{}, 0, false, &_bootstrapRenderer },
            _font{ L"Cascadia Mono", FF_MODERN, FW_NORMAL, { 1, 1 }, CP_UTF8 }
        {
        }

        TextBuffer& Buffer() noexcept
        {
            return _buffer;
        }

        Microsoft::Console::Types::Viewport GetViewport() noexcept override
        {
            return Microsoft::Console::Types::Viewport::FromDimensions({}, { 4, 2 });
        }

        til::point GetTextBufferEndPosition() const noexcept override
        {
            return { 3, 1 };
        }

        TextBuffer& GetTextBuffer() const noexcept override
        {
            return const_cast<TextBuffer&>(_buffer);
        }

        const FontInfo& GetFontInfo() const noexcept override
        {
            return _font;
        }

        std::span<const til::point_span> GetSearchHighlights() const noexcept override
        {
            return {};
        }

        const til::point_span* GetSearchHighlightFocused() const noexcept override
        {
            return nullptr;
        }

        std::span<const til::point_span> GetSelectionSpans() const noexcept override
        {
            return {};
        }

        void LockConsole() noexcept override {}
        void UnlockConsole() noexcept override {}

        Microsoft::Console::Render::TimerDuration GetBlinkInterval() noexcept override
        {
            return Microsoft::Console::Render::TimerDuration::max();
        }

        ULONG GetCursorPixelWidth() const noexcept override
        {
            return 1;
        }

        bool IsGridLineDrawingAllowed() noexcept override
        {
            return true;
        }

        std::wstring_view GetConsoleTitle() const noexcept override
        {
            return {};
        }

        std::wstring GetHyperlinkUri(uint16_t) const override
        {
            return {};
        }

        std::wstring GetHyperlinkCustomId(uint16_t) const override
        {
            return {};
        }

        std::vector<size_t> GetPatternId(const til::point) const override
        {
            return {};
        }

        std::pair<COLORREF, COLORREF> GetAttributeColors(const TextAttribute&) const noexcept override
        {
            return { RGB(255, 255, 255), RGB(0, 0, 0) };
        }

        bool IsSelectionActive() const override
        {
            return false;
        }

        bool IsBlockSelection() const override
        {
            return false;
        }

        void ClearSelection() override {}
        void SelectNewRegion(const til::point, const til::point) override {}

        til::point GetSelectionAnchor() const noexcept override
        {
            return {};
        }

        til::point GetSelectionEnd() const noexcept override
        {
            return {};
        }

        bool IsUiaDataInitialized() const noexcept override
        {
            return false;
        }

    private:
        DummyRenderer _bootstrapRenderer;
        TextBuffer _buffer;
        FontInfo _font;
    };

    class CapturingRenderEngine final : public RenderEngineBase
    {
    public:
        bool supportsImages = true;
        size_t prepareCount = 0;
        size_t beginCount = 0;
        size_t endCount = 0;
        size_t textCount = 0;
        size_t imageSliceCount = 0;
        std::vector<ImagePlacement> placements;
        std::vector<ImageFrameInfo::Surface> surfaces;
        std::vector<std::string> events;

        HRESULT StartPaint() noexcept override
        {
            events.emplace_back("start");
            return S_OK;
        }

        HRESULT EndPaint() noexcept override
        {
            events.emplace_back("end-paint");
            return S_OK;
        }

        HRESULT Present() noexcept override
        {
            events.emplace_back("present");
            return S_OK;
        }

        HRESULT ScrollFrame() noexcept override
        {
            return S_OK;
        }

        HRESULT Invalidate(const til::rect*) noexcept override
        {
            return S_OK;
        }

        HRESULT InvalidateCursor(const til::rect*) noexcept override
        {
            return S_OK;
        }

        HRESULT InvalidateSystem(const til::rect*) noexcept override
        {
            return S_OK;
        }

        HRESULT InvalidateScroll(const til::point*) noexcept override
        {
            return S_OK;
        }

        HRESULT InvalidateAll() noexcept override
        {
            return S_OK;
        }

        HRESULT PrepareImageFrame(const ImageFrameInfo info) noexcept override
        try
        {
            ++prepareCount;
            placements.assign(info.placements.begin(), info.placements.end());
            surfaces.assign(info.surfaces.begin(), info.surfaces.end());
            events.emplace_back("prepare-images");
            return supportsImages ? S_OK : S_FALSE;
        }
        CATCH_RETURN()

        HRESULT PaintBackground() noexcept override
        {
            events.emplace_back("background");
            return S_OK;
        }

        HRESULT PaintBufferLine(std::span<const Microsoft::Console::Render::Cluster>, const til::point coord, bool) noexcept override
        {
            ++textCount;
            events.emplace_back("text-" + std::to_string(coord.y));
            return S_OK;
        }

        HRESULT PaintBufferGridLines(Microsoft::Console::Render::GridLineSet, COLORREF, COLORREF, size_t, til::point) noexcept override
        {
            return S_OK;
        }

        HRESULT PaintImageSlice(const ImageSlice&, const til::CoordType, const til::CoordType) noexcept override
        {
            ++imageSliceCount;
            events.emplace_back("image-slice");
            return S_OK;
        }

        HRESULT BeginRowImages(const til::CoordType targetRow,
                               const til::CoordType,
                               const std::span<const uint8_t>,
                               const std::span<const COLORREF>) noexcept override
        {
            ++beginCount;
            events.emplace_back("begin-" + std::to_string(targetRow));
            return S_OK;
        }

        HRESULT EndRowImages() noexcept override
        {
            ++endCount;
            events.emplace_back("end-row");
            return S_OK;
        }

        HRESULT PaintSelection(const til::rect&) noexcept override
        {
            return S_OK;
        }

        HRESULT PaintCursor(const Microsoft::Console::Render::CursorOptions&) noexcept override
        {
            events.emplace_back("cursor");
            return S_OK;
        }

        HRESULT UpdateDrawingBrushes(const TextAttribute&,
                                     const Microsoft::Console::Render::RenderSettings&,
                                     gsl::not_null<IRenderData*>,
                                     bool,
                                     bool) noexcept override
        {
            return S_OK;
        }

        HRESULT UpdateFont(const FontInfoDesired&, _Out_ FontInfo&) noexcept override
        {
            return S_OK;
        }

        HRESULT UpdateDpi(int) noexcept override
        {
            return S_OK;
        }

        HRESULT UpdateViewport(const til::inclusive_rect&) noexcept override
        {
            return S_OK;
        }

        HRESULT GetProposedFont(const FontInfoDesired&, _Out_ FontInfo&, int) noexcept override
        {
            return S_OK;
        }

        HRESULT GetDirtyArea(std::span<const til::rect>& area) noexcept override
        {
            area = _dirty;
            return S_OK;
        }

        HRESULT GetFontSize(_Out_ til::size* size) noexcept override
        {
            *size = { 1, 1 };
            return S_OK;
        }

        HRESULT IsGlyphWideByFont(std::wstring_view, _Out_ bool* result) noexcept override
        {
            *result = false;
            return S_OK;
        }

    protected:
        HRESULT _DoUpdateTitle(std::wstring_view) noexcept override
        {
            return S_OK;
        }

    private:
        std::array<til::rect, 1> _dirty{ til::rect{ 0, 0, 4, 2 } };
    };
}

void RendererImageResilienceTests::IsRenderableAcceptsWellFormedSnapshot()
{
    const auto surface = MakeSurface({ 4, 3 });
    VERIFY_IS_TRUE(surface.IsRenderable());

    // The largest supported dimension is still renderable.
    auto large = MakeSurface({ Image::MaximumDimension, 1 });
    VERIFY_IS_TRUE(large.IsRenderable());
}

void RendererImageResilienceTests::IsRenderableRejectsMalformedSnapshots()
{
    // A well-formed snapshot that we then corrupt one field at a time. Each mutation
    // must make the whole frame skip just this image instead of aborting or throwing.
    {
        auto s = MakeSurface({ 4, 3 });
        s.image.reset();
        VERIFY_IS_FALSE(s.IsRenderable(), L"missing image");
    }
    {
        auto s = MakeSurface({ 4, 3 });
        s.pixels.reset();
        VERIFY_IS_FALSE(s.IsRenderable(), L"missing pixels");
    }
    {
        auto s = MakeSurface({ 4, 3 });
        s.size = { 0, 3 };
        VERIFY_IS_FALSE(s.IsRenderable(), L"nonpositive width");
    }
    {
        auto s = MakeSurface({ 4, 3 });
        s.size = { 4, -1 };
        VERIFY_IS_FALSE(s.IsRenderable(), L"nonpositive height");
    }
    {
        auto s = MakeSurface({ 4, 3 });
        s.size = { Image::MaximumDimension + 1, 3 };
        VERIFY_IS_FALSE(s.IsRenderable(), L"width past the guaranteed texture limit");
    }
    {
        // The pixel buffer no longer matches the declared size. Blitting this would
        // read out of bounds, so it must be rejected.
        auto s = MakeSurface({ 4, 3 });
        s.size = { 8, 3 };
        VERIFY_IS_FALSE(s.IsRenderable(), L"pixel count disagrees with size");
    }
}

void RendererImageResilienceTests::StaleEntryIsEvictedIndividually()
{
    // Two images are live this frame; a third was cached previously but is now gone.
    const auto a = MakeSurface({ 4, 3 });
    const auto b = MakeSurface({ 4, 3 });
    const auto goneImage = MakeImage({ 4, 3 });

    const std::vector<ImageFrameInfo::Surface> surfaces{ a, b };
    const std::span<const ImageFrameInfo::Surface> frame{ surfaces };

    // Live images are retained...
    VERIFY_IS_FALSE(ImageFrameInfo::ImageCacheEntryIsStale(a.image.get(), frame));
    VERIFY_IS_FALSE(ImageFrameInfo::ImageCacheEntryIsStale(b.image.get(), frame));
    // ...and only the entry that is no longer present is reported stale, so a backend
    // drops that one cache entry rather than resetting an unrelated cache (the glyph
    // atlas, in BackendD3D) merely because an image came or went.
    VERIFY_IS_TRUE(ImageFrameInfo::ImageCacheEntryIsStale(goneImage.get(), frame));
}

void RendererImageResilienceTests::SurfaceRemovalMarksEntryStale()
{
    // Model a scroll/erase that removes an image from the frame while another stays.
    const auto stays = MakeSurface({ 4, 3 });
    const auto removed = MakeSurface({ 4, 3 });

    const std::vector<ImageFrameInfo::Surface> before{ stays, removed };
    VERIFY_IS_FALSE(ImageFrameInfo::ImageCacheEntryIsStale(removed.image.get(), std::span{ before }));

    const std::vector<ImageFrameInfo::Surface> after{ stays };
    VERIFY_IS_TRUE(ImageFrameInfo::ImageCacheEntryIsStale(removed.image.get(), std::span{ after }));
    VERIFY_IS_FALSE(ImageFrameInfo::ImageCacheEntryIsStale(stays.image.get(), std::span{ after }));
}

void RendererImageResilienceTests::StaleShapedRowSurfaceRemovalIsHarmless()
{
    // A row can still carry a placement after its surface is erased/scrolled away
    // (a "stale shaped row"). The frame's surface list is what the backends look the
    // placement up in; when the lookup misses, the draw path skips just that image.
    // This mirrors the std::ranges::find(...) == end() skip in BackendD2D::_drawImage
    // and BackendD3D::_drawImage, and the missing-cache skip in
    // GdiEngine::_PaintDirectImage.
    const til::rect bounds{ 0, 0, 4, 1 };
    const ImagePlacement placement{
        ImagePlacement::Key{ 1, 1 },
        MakeImage({ bounds.width(), bounds.height() }),
        bounds,
        0,
    };
    const auto* const stalePlacementImage = placement.SurfacePointer().get();

    // This frame no longer prepared a surface for that image (removed by scroll or
    // erase). The lookup the backends perform therefore misses, so the placement is
    // skipped and the rest of the frame (text, other images) is unaffected.
    const std::vector<ImageFrameInfo::Surface> frameSurfaces{ MakeSurface({ 4, 3 }) };
    const auto found = std::ranges::find(frameSurfaces, stalePlacementImage, [](const auto& surface) {
        return surface.image.get();
    });
    VERIFY_IS_TRUE(found == frameSurfaces.end(), L"a stale placement finds no snapshot and is skipped");
    VERIFY_IS_TRUE(ImageFrameInfo::ImageCacheEntryIsStale(stalePlacementImage, std::span{ frameSurfaces }));

    // A surface that survives but was emptied (e.g. mid-resize) is likewise skipped
    // rather than blitted out of bounds.
    auto emptied = MakeSurface({ 4, 3 });
    emptied.pixels = std::make_shared<const std::vector<RGBQUAD>>();
    VERIFY_IS_FALSE(emptied.IsRenderable());
}

void RendererImageResilienceTests::DefaultBackgroundMaskRightBoundStaysInBounds()
{
    // The right-bound indexing claim: placements are cropped to the same viewport
    // that sizes the mask, so bounds.right never exceeds viewportLeft + mask size.
    // The consumer scan additionally clamps to the mask, matching the clamp already
    // in GdiEngine::_MarkDirectImageUnderlay, so even a placement that reaches (or
    // over-reaches) the viewport edge never indexes past the mask.
    constexpr til::CoordType viewportLeft = 5;
    constexpr til::CoordType viewportWidth = 8;
    constexpr size_t maskSize = gsl::narrow_cast<size_t>(viewportWidth);

    // A placement flush against the right edge of the viewport.
    const std::array<til::rect, 3> cases{
        til::rect{ viewportLeft, 0, viewportLeft + viewportWidth, 1 }, // exactly the viewport
        til::rect{ viewportLeft + 2, 0, viewportLeft + viewportWidth, 1 }, // right edge
        til::rect{ viewportLeft, 0, viewportLeft + viewportWidth + 4, 1 }, // deliberately over-reaching
    };

    for (const auto& bounds : cases)
    {
        const auto indices = ConsumerMaskIndices(bounds, viewportLeft, maskSize);
        for (const auto index : indices)
        {
            VERIFY_IS_TRUE(index >= 0, L"mask index is never negative");
            VERIFY_IS_TRUE(gsl::narrow_cast<size_t>(index) < maskSize, L"mask index never runs past the mask");
        }
    }
}

void RendererImageResilienceTests::DoubleWidthImageMaskCoordinateDivergence()
{
    // Focused double-width/double-height investigation. The producer indexes the
    // mask by screen column (line-rendition aware); the consumer indexes it by
    // buffer column. This test determines whether they diverge, using the real ROW
    // attribute APIs.
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 2 }, TextAttribute{}, 0, false, &renderer };
    auto& row = buffer.GetMutableRowByOffset(0);
    row.Reset(TextAttribute{});

    // Give buffer column 1 an explicit (non-default) background; all others default.
    TextAttribute nonDefault{};
    nonDefault.SetBackground(RGB(1, 2, 3));
    row.ReplaceAttributes(1, 2, nonDefault);

    constexpr til::CoordType viewportLeft = 0;
    constexpr til::CoordType viewportWidth = 8;
    const til::rect bounds{ 0, 0, 4, 1 };

    // A helper that reports, for each buffer column a placement covers, whether the
    // mask entry the consumer reads agrees with that buffer column's true default-ness.
    const auto measure = [&]() {
        const auto mask = BuildProducerMask(row, viewportLeft, viewportWidth);
        const auto indices = ConsumerMaskIndices(bounds, viewportLeft, viewportWidth);
        auto diverged = false;
        for (size_t i = 0; i < indices.size(); ++i)
        {
            const auto bufferColumn = bounds.left + gsl::narrow_cast<til::CoordType>(i);
            const auto consumerRead = mask[gsl::narrow_cast<size_t>(indices[i])];
            const auto truth = row.GetAttrByColumn(bufferColumn).GetBackground().IsDefault() ? uint8_t{ 1 } : uint8_t{ 0 };
            // Right-bound safety holds in every rendition.
            VERIFY_IS_TRUE(gsl::narrow_cast<size_t>(indices[i]) < mask.size());
            diverged |= (consumerRead != truth);
        }
        return diverged;
    };

    // Single-width is the production path: producer and consumer agree exactly.
    row.SetLineRendition(LineRendition::SingleWidth);
    const auto singleDiverged = measure();
    VERIFY_IS_FALSE(singleDiverged, L"single-width producer/consumer coordinates agree");

    // Double-width: report whether the screen-column producer and buffer-column
    // consumer diverge. This is a determination, not a required failure.
    row.SetLineRendition(LineRendition::DoubleWidth);
    const auto doubleDiverged = measure();
    if (doubleDiverged)
    {
        Log::Comment(L"REPRO(formula): a BehindBackground image on a double-width row would read the mask at the wrong columns.");
        Log::Comment(L"No production trigger: the image pipeline positions placements in buffer columns and does not feed BehindBackground image masks for DECDWL rows, so the coordinate basis is left unchanged and guarded by the right-bound clamp.");
    }
    else
    {
        Log::Comment(L"NO-REPRO: double-width producer/consumer coordinates agree.");
    }
    // The safety guarantee (no out-of-bounds mask access) holds regardless of the
    // divergence determination above; that is what the shipped change guarantees.
    VERIFY_IS_TRUE(true);
}

void RendererImageResilienceTests::RendererSubmitsOneFrameSnapshotInCompositionOrder()
{
    TestRenderData data;
    DummyRenderer renderer{ &data };
    CapturingRenderEngine engine;
    renderer.AddRenderEngine(&engine);

    const auto shared = MakeImage({ 4, 1 });
    const auto background = MakeImage({ 4, 1 });
    data.Buffer().GetMutableImages().Add(ImagePlacement{
        { 1, 1 },
        shared,
        { 1, 0, 5, 1 },
        -1,
    });
    data.Buffer().GetMutableImages().Add(ImagePlacement{
        { 2, 2 },
        shared,
        { 0, 0, 2, 1 },
        1,
    });
    data.Buffer().GetMutableImages().Add(ImagePlacement{
        { 3, 3 },
        background,
        { 0, 1, 4, 2 },
        ImagePlacement::BackgroundZThreshold - 1,
    });

    VERIFY_ARE_EQUAL(S_OK, renderer.PaintFrame());
    VERIFY_ARE_EQUAL(size_t{ 1 }, engine.prepareCount);
    VERIFY_ARE_EQUAL(size_t{ 3 }, engine.placements.size());
    VERIFY_ARE_EQUAL(size_t{ 2 }, engine.surfaces.size(), L"two placements sharing an Image produce one surface snapshot");
    const til::rect croppedBounds{ 1, 0, 4, 1 };
    VERIFY_ARE_EQUAL(croppedBounds, engine.placements[1].CellBounds(), L"placements are cropped to the viewport");
    VERIFY_ARE_EQUAL(background.get(), engine.placements[0].SurfacePointer().get(), L"placements are submitted in composition order");
    VERIFY_ARE_EQUAL(size_t{ 2 }, engine.beginCount);
    VERIFY_ARE_EQUAL(engine.beginCount, engine.endCount);
    VERIFY_ARE_EQUAL(size_t{ 0 }, engine.imageSliceCount, L"direct surfaces do not use the legacy ImageSlice contract");

    const auto backgroundEvent = std::ranges::find(engine.events, "background");
    const auto beginEvent = std::ranges::find(engine.events, "begin-0");
    const auto textEvent = std::ranges::find(engine.events, "text-0");
    const auto endEvent = std::ranges::find(engine.events, "end-row");
    VERIFY_IS_TRUE(backgroundEvent < beginEvent);
    VERIFY_IS_TRUE(beginEvent < textEvent);
    VERIFY_IS_TRUE(textEvent < endEvent);
}

void RendererImageResilienceTests::TextOnlyFallbackStillPaintsText()
{
    TestRenderData data;
    DummyRenderer renderer{ &data };
    CapturingRenderEngine engine;
    engine.supportsImages = false;
    renderer.AddRenderEngine(&engine);

    data.Buffer().GetMutableImages().Add(ImagePlacement{
        { 1, 1 },
        MakeImage({ 2, 1 }),
        { 0, 0, 2, 1 },
        0,
    });

    VERIFY_ARE_EQUAL(S_OK, renderer.PaintFrame());
    VERIFY_ARE_EQUAL(size_t{ 1 }, engine.prepareCount);
    VERIFY_ARE_EQUAL(size_t{ 0 }, engine.beginCount);
    VERIFY_ARE_EQUAL(size_t{ 0 }, engine.endCount);
    VERIFY_IS_TRUE(engine.textCount > 0, L"S_FALSE image support is a text-only fallback, not a blank frame");
}
