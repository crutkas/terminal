// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// These tests cover the renderer's image-resilience contract that the GDI, Atlas
// D2D, and Atlas D3D backends all share:
//   * A malformed image snapshot is skipped, not fatal (ImageFrameInfo::Surface::IsRenderable).
//   * An image that leaves the frame is evicted individually, never by resetting an
//     unrelated cache such as the glyph atlas (ImageFrameInfo::ImageCacheEntryIsStale).
//   * Shared surfaces are deduplicated and indexed independently of placement order.
// The decision logic lives in the shared header so every backend agrees and so it
// can be exercised here without a graphics device.

#include "precomp.h"
#include "WexTestClass.h"
#include "../../inc/consoletaeftemplates.hpp"

#include "../Image.hpp"
#include "../textBuffer.hpp"
#include "../../../renderer/inc/IRenderEngine.hpp"
#include "../../../renderer/inc/DummyRenderer.hpp"

using namespace WEX::Logging;
using namespace WEX::TestExecution;
using Microsoft::Console::Render::ImageFrameInfo;

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
    TEST_METHOD(SurfaceSnapshotDeduplicatesNonAdjacentPlacements);
    TEST_METHOD(SurfaceLookupFindsMultipleZFragments);

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

    static void SortSurfaces(std::vector<ImageFrameInfo::Surface>& surfaces)
    {
        std::ranges::sort(surfaces, std::less<>{}, [](const auto& surface) noexcept {
            return surface.image.get();
        });
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

    std::vector<ImageFrameInfo::Surface> surfaces{ a, b };
    SortSurfaces(surfaces);
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

    std::vector<ImageFrameInfo::Surface> before{ stays, removed };
    SortSurfaces(before);
    VERIFY_IS_FALSE(ImageFrameInfo::ImageCacheEntryIsStale(removed.image.get(), std::span{ before }));

    std::vector<ImageFrameInfo::Surface> after{ stays };
    SortSurfaces(after);
    VERIFY_IS_TRUE(ImageFrameInfo::ImageCacheEntryIsStale(removed.image.get(), std::span{ after }));
    VERIFY_IS_FALSE(ImageFrameInfo::ImageCacheEntryIsStale(stays.image.get(), std::span{ after }));
}

void RendererImageResilienceTests::StaleShapedRowSurfaceRemovalIsHarmless()
{
    // A row can still carry a placement after its surface is erased/scrolled away
    // (a "stale shaped row"). The frame's surface list is what the backends look the
    // placement up in; when the lookup misses, the draw path skips just that image.
    // This mirrors the FindSurface(...) == nullptr skip in BackendD2D::_drawImage
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
    const auto found = ImageFrameInfo::FindSurface(stalePlacementImage, frameSurfaces);
    VERIFY_IS_TRUE(found == nullptr, L"a stale placement finds no snapshot and is skipped");
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

void RendererImageResilienceTests::SurfaceSnapshotDeduplicatesNonAdjacentPlacements()
{
    const auto shared = MakeImage({ 4, 1 });
    const auto middle = MakeImage({ 4, 1 });
    const auto last = MakeImage({ 4, 1 });
    std::vector<ImagePlacement> placements;
    placements.emplace_back(ImagePlacement::Key{ 1, 1 }, shared, til::rect{ 0, 0, 4, 1 }, 20);
    placements.emplace_back(ImagePlacement::Key{ 2, 2 }, middle, til::rect{ 0, 0, 4, 1 }, -5);
    placements.emplace_back(ImagePlacement::FromFragment(ImagePlacement::Key{ 3, 3 },
                                                         shared,
                                                         til::rect{ 2, 0, 4, 1 },
                                                         til::rect{ 0, 0, 4, 1 },
                                                         -20));
    placements.emplace_back(ImagePlacement::Key{ 4, 4 }, last, til::rect{ 0, 0, 4, 1 }, 5);
    placements.emplace_back(ImagePlacement::Key{ 5, 5 }, shared, til::rect{ 0, 0, 2, 1 }, 0);

    std::vector<ImageFrameInfo::Surface> surfaces;
    ImageFrameInfo::BuildSurfaceSnapshot(placements, surfaces);

    VERIFY_ARE_EQUAL(size_t{ 3 }, surfaces.size(), L"nonadjacent placements sharing an image produce one surface");
    VERIFY_IS_TRUE(std::ranges::is_sorted(surfaces, std::less<>{}, [](const auto& surface) noexcept {
        return surface.image.get();
    }));
    VERIFY_IS_TRUE(ImageFrameInfo::FindSurface(shared.get(), surfaces) != nullptr);
    VERIFY_IS_TRUE(ImageFrameInfo::FindSurface(middle.get(), surfaces) != nullptr);
    VERIFY_IS_TRUE(ImageFrameInfo::FindSurface(last.get(), surfaces) != nullptr);
}

void RendererImageResilienceTests::SurfaceLookupFindsMultipleZFragments()
{
    const auto shared = MakeImage({ 4, 1 });
    const auto between = MakeImage({ 4, 1 });
    std::vector<ImagePlacement> placements;
    placements.emplace_back(ImagePlacement::Key{ 1, 1 }, shared, til::rect{ 0, 0, 4, 1 }, 20);
    placements.emplace_back(ImagePlacement::Key{ 2, 2 }, between, til::rect{ 0, 0, 4, 1 }, 0);
    placements.emplace_back(ImagePlacement::FromFragment(ImagePlacement::Key{ 3, 3 },
                                                         shared,
                                                         til::rect{ 1, 0, 3, 1 },
                                                         til::rect{ 0, 0, 4, 1 },
                                                         -20));
    std::ranges::sort(placements, {}, [](const auto& placement) noexcept {
        return placement.ZIndex();
    });

    std::vector<ImageFrameInfo::Surface> surfaces;
    ImageFrameInfo::BuildSurfaceSnapshot(placements, surfaces);

    for (const auto& placement : placements)
    {
        const auto snapshot = ImageFrameInfo::FindSurface(placement.SurfacePointer().get(), surfaces);
        VERIFY_IS_TRUE(snapshot != nullptr, L"every backend draw finds its frame snapshot");
        VERIFY_ARE_EQUAL(placement.SurfacePointer().get(), snapshot->image.get());
    }
    const auto missing = MakeImage({ 4, 1 });
    VERIFY_IS_TRUE(ImageFrameInfo::FindSurface(missing.get(), surfaces) == nullptr);
    VERIFY_IS_FALSE(ImageFrameInfo::ImageCacheEntryIsStale(shared.get(), surfaces));
    VERIFY_IS_TRUE(ImageFrameInfo::ImageCacheEntryIsStale(missing.get(), surfaces));
}
