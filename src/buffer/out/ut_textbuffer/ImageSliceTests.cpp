// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "WexTestClass.h"
#include "../../inc/consoletaeftemplates.hpp"

#include "../textBuffer.hpp"
#include "../ImageSlice.hpp"
#include "../../../renderer/inc/DummyRenderer.hpp"

#include <set>

using namespace WEX::Common;
using namespace WEX::Logging;
using namespace WEX::TestExecution;

class ImageSliceTests
{
    TEST_CLASS(ImageSliceTests);

    TEST_METHOD(UntaggedContentAllocatesNoLayers);
    TEST_METHOD(LayerPixelsSurviveRangeGrowth);
    TEST_METHOD(HigherZIndexCompositesOnTop);
    TEST_METHOD(TransparentPixelsDoNotOccludeLowerLayers);
    TEST_METHOD(ZIndexSelectsRenderPosition);
    TEST_METHOD(ErasingOneLayerLeavesTheOther);
    TEST_METHOD(PlacementsOfOneImageEraseIndependently);
    TEST_METHOD(ErasingAColumnRangeSparesTheRest);
    TEST_METHOD(LayerCountIsCapped);
    TEST_METHOD(LayerBudgetIsReturnedOnDestruction);
    TEST_METHOD(MovingASliceCarriesItsBudgetShareExactlyOnce);
    TEST_METHOD(CopyingASliceChargesTheBudgetSeparately);
    TEST_METHOD(EqualZOrdersByImageIdNotWriteOrder);
    TEST_METHOD(LayersCompositeSourceOver);
    TEST_METHOD(RenderPositionRevisionsNeverCollide);
    TEST_METHOD(UnlayeredContentIsNotCopiedToComposite);
    TEST_METHOD(CopyingReplacesDestinationLayers);
    TEST_METHOD(CopyingFromAnUnlayeredSourceClearsDestinationLayers);
    TEST_METHOD(CopyingFromALayerOnlySourceClearsDestinationBasePlane);
    TEST_METHOD(CopyingACellOutsideTheSourceLayerCopiesNothing);
    TEST_METHOD(CopyingBetweenRowsOfDifferentCellSizesErases);

    static constexpr til::size CellSize{ 4, 2 };
    static constexpr RGBQUAD Red{ 0, 0, 255, 255 };
    static constexpr RGBQUAD Blue{ 255, 0, 0, 255 };
    static constexpr RGBQUAD Clear{ 0, 0, 0, 0 };

    static uint32_t Packed(const RGBQUAD& pixel) noexcept
    {
        return static_cast<uint32_t>(pixel.rgbBlue) |
               (static_cast<uint32_t>(pixel.rgbGreen) << 8) |
               (static_cast<uint32_t>(pixel.rgbRed) << 16) |
               (static_cast<uint32_t>(pixel.rgbReserved) << 24);
    }

    // Fills `columns` worth of cells starting at the given plane pointer. The
    // pointer must be re-fetched after anything that can widen the slice.
    static void Fill(ImageSlice& slice, RGBQUAD* start, const til::CoordType columns, const RGBQUAD color)
    {
        const auto width = columns * slice.CellSize().width;
        auto row = start;
        for (auto y = 0; y < slice.CellSize().height; y++)
        {
            for (auto x = 0; x < width; x++)
            {
                row[x] = color;
            }
            row += slice.PixelWidth();
        }
    }

    static uint32_t PixelAt(const std::span<const RGBQUAD> plane, const ImageSlice& slice, const til::CoordType column)
    {
        const auto offset = (column - slice.ColumnOffset()) * slice.CellSize().width;
        return Packed(plane[offset]);
    }

    static ImageSlice& SliceFor(ROW& row, const til::size cellSize = CellSize)
    {
        auto slice = row.GetMutableImageSlice();
        if (!slice)
        {
            slice = row.SetImageSlice(std::make_unique<ImageSlice>(cellSize));
        }
        return *slice;
    }

    static void FillLayer(ROW& row, const ImageSlice::LayerKey key, const til::CoordType columns, const RGBQUAD color, const til::size cellSize = CellSize)
    {
        auto& slice = SliceFor(row, cellSize);
        const auto pixels = slice.MutablePixels(0, columns, key, 0);
        VERIFY_IS_NOT_NULL(pixels);
        Fill(slice, pixels, columns, color);
    }

    static void FillBase(ROW& row, const til::CoordType columns, const RGBQUAD color, const til::size cellSize = CellSize)
    {
        auto& slice = SliceFor(row, cellSize);
        const auto pixels = slice.MutablePixels(0, columns);
        VERIFY_IS_NOT_NULL(pixels);
        Fill(slice, pixels, columns, color);
    }
};

// Content without an identity has to keep costing exactly what it did before
// layers existed, so nothing here should allocate a layer.
void ImageSliceTests::UntaggedContentAllocatesNoLayers()
{
    ImageSlice slice{ CellSize };
    Fill(slice, slice.MutablePixels(2, 6), 4, Red);
    slice.BumpRevision();

    VERIFY_IS_FALSE(slice.Contains(ImageSlice::LayerKey{}));
    VERIFY_ARE_EQUAL(0u, slice.LayersAtColumn(3).size());
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(slice.Pixels(), slice, 2));
    // Untagged content has always drawn above the text.
    VERIFY_IS_TRUE(slice.HasPixels(ImageSlice::RenderPosition::AboveText));
    VERIFY_IS_FALSE(slice.HasPixels(ImageSlice::RenderPosition::BehindText));
}

// Writing to a column left of an existing layer widens the slice, which moves
// every plane. The layer's pixels must move with it.
void ImageSliceTests::LayerPixelsSurviveRangeGrowth()
{
    ImageSlice slice{ CellSize };
    constexpr ImageSlice::LayerKey key{ 7, 0 };

    Fill(slice, slice.MutablePixels(10, 14, key, 0), 4, Red);
    slice.BumpRevision();
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(slice.Pixels(ImageSlice::RenderPosition::AboveText), slice, 10));

    // Grow the slice to the left, which relocates the layer's plane.
    Fill(slice, slice.MutablePixels(0, 2), 2, Blue);
    slice.BumpRevision();

    VERIFY_ARE_EQUAL(0, slice.ColumnOffset());
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(slice.Pixels(ImageSlice::RenderPosition::AboveText), slice, 10));
    VERIFY_IS_TRUE(slice.LayerCoversColumn(key, 10));
    VERIFY_IS_FALSE(slice.LayerCoversColumn(key, 0));
}

void ImageSliceTests::HigherZIndexCompositesOnTop()
{
    ImageSlice slice{ CellSize };
    constexpr ImageSlice::LayerKey lower{ 1, 0 };
    constexpr ImageSlice::LayerKey upper{ 2, 0 };

    Fill(slice, slice.MutablePixels(0, 4, lower, 1), 4, Red);
    Fill(slice, slice.MutablePixels(0, 4, upper, 5), 4, Blue);
    slice.BumpRevision();

    VERIFY_ARE_EQUAL(Packed(Blue), PixelAt(slice.Pixels(ImageSlice::RenderPosition::AboveText), slice, 0));

    // Erasing the upper layer must reveal the lower one again.
    slice.EraseLayer(upper);
    slice.BumpRevision();
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(slice.Pixels(ImageSlice::RenderPosition::AboveText), slice, 0));
}

void ImageSliceTests::TransparentPixelsDoNotOccludeLowerLayers()
{
    ImageSlice slice{ CellSize };
    constexpr ImageSlice::LayerKey lower{ 1, 0 };
    constexpr ImageSlice::LayerKey upper{ 2, 0 };

    Fill(slice, slice.MutablePixels(0, 4, lower, 1), 4, Red);
    Fill(slice, slice.MutablePixels(0, 4, upper, 5), 4, Clear);
    slice.BumpRevision();

    // The upper layer is entirely transparent, so the lower one shows through.
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(slice.Pixels(ImageSlice::RenderPosition::AboveText), slice, 0));
}

void ImageSliceTests::ZIndexSelectsRenderPosition()
{
    ImageSlice slice{ CellSize };
    Fill(slice, slice.MutablePixels(0, 2, ImageSlice::LayerKey{ 1, 0 }, 0), 2, Red);
    Fill(slice, slice.MutablePixels(0, 2, ImageSlice::LayerKey{ 2, 0 }, -1), 2, Red);
    Fill(slice, slice.MutablePixels(0, 2, ImageSlice::LayerKey{ 3, 0 }, ImageSlice::BackgroundZThreshold - 1), 2, Red);
    slice.BumpRevision();

    VERIFY_IS_TRUE(slice.HasPixels(ImageSlice::RenderPosition::AboveText));
    VERIFY_IS_TRUE(slice.HasPixels(ImageSlice::RenderPosition::BehindText));
    VERIFY_IS_TRUE(slice.HasPixels(ImageSlice::RenderPosition::BehindBackground));
}

// The whole point of giving layers an identity: two images can share a row and
// be deleted independently.
void ImageSliceTests::ErasingOneLayerLeavesTheOther()
{
    ImageSlice slice{ CellSize };
    constexpr ImageSlice::LayerKey first{ 1, 0 };
    constexpr ImageSlice::LayerKey second{ 2, 0 };

    Fill(slice, slice.MutablePixels(0, 4, first, 0), 4, Red);
    Fill(slice, slice.MutablePixels(4, 8, second, 0), 4, Blue);
    slice.BumpRevision();

    const auto empty = slice.EraseLayer(1u);
    slice.BumpRevision();

    VERIFY_IS_FALSE(empty, L"the second layer is still present");
    VERIFY_IS_FALSE(slice.Contains(1u));
    VERIFY_IS_TRUE(slice.Contains(2u));
    VERIFY_ARE_EQUAL(Packed(Blue), PixelAt(slice.Pixels(ImageSlice::RenderPosition::AboveText), slice, 4));
    VERIFY_ARE_EQUAL(Packed(Clear), PixelAt(slice.Pixels(ImageSlice::RenderPosition::AboveText), slice, 0));
}

void ImageSliceTests::PlacementsOfOneImageEraseIndependently()
{
    ImageSlice slice{ CellSize };
    constexpr ImageSlice::LayerKey placementA{ 9, 100 };
    constexpr ImageSlice::LayerKey placementB{ 9, 200 };

    Fill(slice, slice.MutablePixels(0, 4, placementA, 0), 4, Red);
    Fill(slice, slice.MutablePixels(4, 8, placementB, 0), 4, Blue);
    slice.BumpRevision();

    slice.EraseLayer(placementA);
    slice.BumpRevision();

    VERIFY_IS_FALSE(slice.Contains(placementA));
    VERIFY_IS_TRUE(slice.Contains(placementB));
    // The image itself is still present, just not that placement of it.
    VERIFY_IS_TRUE(slice.Contains(9u));
}

void ImageSliceTests::ErasingAColumnRangeSparesTheRest()
{
    ImageSlice slice{ CellSize };
    constexpr ImageSlice::LayerKey key{ 4, 0 };

    Fill(slice, slice.MutablePixels(0, 8, key, 0), 8, Red);
    slice.BumpRevision();

    slice.EraseLayer(4u, 0, 4);
    slice.BumpRevision();

    VERIFY_IS_FALSE(slice.LayerCoversColumn(key, 0));
    VERIFY_IS_TRUE(slice.LayerCoversColumn(key, 4));
    VERIFY_ARE_EQUAL(Packed(Clear), PixelAt(slice.Pixels(ImageSlice::RenderPosition::AboveText), slice, 0));
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(slice.Pixels(ImageSlice::RenderPosition::AboveText), slice, 4));
}

// A stream of images must not be able to grow a single row without bound.
void ImageSliceTests::LayerCountIsCapped()
{
    ImageSlice slice{ CellSize };
    for (auto i = 0u; i < ImageSlice::MaxLayersPerSlice; i++)
    {
        const auto pixels = slice.MutablePixels(0, 1, ImageSlice::LayerKey{ i + 1, 0 }, 0);
        VERIFY_IS_NOT_NULL(pixels);
    }
    // One past the cap is refused rather than accepted. A caller that asked for a
    // layer it needs gets an exception, because carrying on without the pixels it
    // was handed is never right; a caller that only wanted the layer if it fit
    // asks for that explicitly and gets a null it is obliged to check.
    VERIFY_THROWS(slice.MutablePixels(0, 1, ImageSlice::LayerKey{ 0xF0000000, 0 }, 0), std::bad_alloc);
    VERIFY_IS_NULL(slice.TryMutablePixels(0, 1, ImageSlice::LayerKey{ 0xF0000000, 0 }, 0));
}

void ImageSliceTests::EqualZOrdersByImageIdNotWriteOrder()
{
    ImageSlice slice{ CellSize };
    // Written highest-id first, so insertion order and identity order disagree.
    Fill(slice, slice.MutablePixels(0, 1, ImageSlice::LayerKey{ 2, 20 }, 7), 1, Blue);
    Fill(slice, slice.MutablePixels(0, 1, ImageSlice::LayerKey{ 1, 10 }, 7), 1, Red);

    const auto pixels = slice.Pixels(ImageSlice::RenderPosition::AboveText);
    VERIFY_ARE_EQUAL(Packed(Blue), Packed(pixels[0]), L"the higher image id must win at equal z");
}

void ImageSliceTests::LayersCompositeSourceOver()
{
    static constexpr RGBQUAD HalfRed{ 0, 0, 128, 128 };
    static constexpr RGBQUAD HalfGreen{ 0, 128, 0, 128 };

    ImageSlice slice{ CellSize };
    Fill(slice, slice.MutablePixels(0, 1, ImageSlice::LayerKey{ 1, 10 }, 0), 1, HalfRed);
    Fill(slice, slice.MutablePixels(0, 1, ImageSlice::LayerKey{ 2, 20 }, 0), 1, HalfGreen);

    // Premultiplied source-over: dst = src + dst * (1 - srcAlpha). Half-green over
    // half-red keeps all of the green and just under half of the red, rather than
    // the top layer simply replacing what is beneath it.
    const auto pixel = slice.Pixels(ImageSlice::RenderPosition::AboveText)[0];
    VERIFY_ARE_EQUAL(64, static_cast<int>(pixel.rgbRed));
    VERIFY_ARE_EQUAL(128, static_cast<int>(pixel.rgbGreen));
    VERIFY_ARE_EQUAL(0, static_cast<int>(pixel.rgbBlue));
    VERIFY_ARE_EQUAL(192, static_cast<int>(pixel.rgbReserved));
}

void ImageSliceTests::LayerBudgetIsReturnedOnDestruction()
{
    const auto before = ImageSlice::LayerBytesAvailable();
    {
        ImageSlice slice{ CellSize };
        Fill(slice, slice.MutablePixels(0, 16, ImageSlice::LayerKey{ 1, 0 }, 0), 16, Red);
        VERIFY_IS_LESS_THAN(ImageSlice::LayerBytesAvailable(), before, L"the layer is charged to the budget");
    }
    VERIFY_ARE_EQUAL(before, ImageSlice::LayerBytesAvailable(), L"and returned when the slice dies");
}

// A slice owns a share of a process-wide budget, which is the only reason its
// special members are not all compiler-generated. Moving one must hand that
// share over rather than duplicating it or dropping it on the floor -- a vector
// of slices reallocating is enough to exercise this.
void ImageSliceTests::MovingASliceCarriesItsBudgetShareExactlyOnce()
{
    const auto before = ImageSlice::LayerBytesAvailable();
    {
        ImageSlice source{ CellSize };
        Fill(source, source.MutablePixels(0, 16, ImageSlice::LayerKey{ 1, 0 }, 0), 16, Red);
        const auto afterWrite = ImageSlice::LayerBytesAvailable();
        VERIFY_IS_LESS_THAN(afterWrite, before, L"sanity: the layer is charged");

        ImageSlice moved{ std::move(source) };
        VERIFY_ARE_EQUAL(afterWrite, ImageSlice::LayerBytesAvailable(), L"a move must not charge the budget a second time");

        // The moved-from slice still has to be destroyed, and must not refund a
        // share it no longer owns.
    }
    VERIFY_ARE_EQUAL(before, ImageSlice::LayerBytesAvailable(), L"the share is refunded exactly once");

    {
        ImageSlice source{ CellSize };
        Fill(source, source.MutablePixels(0, 16, ImageSlice::LayerKey{ 1, 0 }, 0), 16, Red);
        const auto charged = before - ImageSlice::LayerBytesAvailable();

        ImageSlice target{ CellSize };
        target = std::move(source);
        VERIFY_ARE_EQUAL(before - charged, ImageSlice::LayerBytesAvailable(), L"move-assignment must not double-charge either");
    }
    VERIFY_ARE_EQUAL(before, ImageSlice::LayerBytesAvailable(), L"and move-assignment refunds exactly once");
}

// A copy carries the same layers, so it must owe the same amount independently.
void ImageSliceTests::CopyingASliceChargesTheBudgetSeparately()
{
    const auto before = ImageSlice::LayerBytesAvailable();
    {
        ImageSlice source{ CellSize };
        Fill(source, source.MutablePixels(0, 16, ImageSlice::LayerKey{ 1, 0 }, 0), 16, Red);
        const auto charged = before - ImageSlice::LayerBytesAvailable();
        VERIFY_IS_GREATER_THAN(charged, size_t{ 0 }, L"sanity: the layer costs something");

        ImageSlice copy{ source };
        VERIFY_ARE_EQUAL(before - charged * 2, ImageSlice::LayerBytesAvailable(), L"a copy owes its own share");
    }
    VERIFY_ARE_EQUAL(before, ImageSlice::LayerBytesAvailable(), L"both shares come back");
}

// A renderer caches uploaded pixels keyed by revision. Each of a slice's three
// composited planes is a different image, so every plane of every slice needs a
// revision no other plane anywhere can also hand out.
void ImageSliceTests::RenderPositionRevisionsNeverCollide()
{
    static constexpr std::array positions{
        ImageSlice::RenderPosition::BehindBackground,
        ImageSlice::RenderPosition::BehindText,
        ImageSlice::RenderPosition::AboveText,
    };

    std::vector<ImageSlice> slices;
    std::set<uint64_t> seen;
    for (auto i = 0; i < 16; i++)
    {
        ImageSlice slice{ CellSize };
        slice.BumpRevision();
        for (const auto position : positions)
        {
            const auto revision = slice.Revision(position);
            VERIFY_IS_TRUE(revision != 0, L"0 is reserved to mean 'nothing uploaded'");
            VERIFY_IS_TRUE(seen.insert(revision).second, L"revision was already handed out");
        }
    }
}
// Content with no identity is the overwhelmingly common case, and it composites
// to exactly itself. Handing back a copy of it would double both the memory and
// the work for every image that never uses layers at all.
void ImageSliceTests::UnlayeredContentIsNotCopiedToComposite()
{
    ImageSlice slice{ CellSize };
    const auto pixels = slice.MutablePixels(0, 4);
    VERIFY_IS_NOT_NULL(pixels);
    Fill(slice, pixels, 4, Red);

    const auto base = slice.Pixels();
    const auto composited = slice.Pixels(ImageSlice::RenderPosition::AboveText);
    VERIFY_ARE_EQUAL(base.data(), composited.data(), L"the base plane itself should be handed back");
    VERIFY_IS_TRUE(slice.HasPixels(ImageSlice::RenderPosition::AboveText));
    VERIFY_IS_FALSE(slice.HasPixels(ImageSlice::RenderPosition::BehindText));
    VERIFY_IS_FALSE(slice.HasPixels(ImageSlice::RenderPosition::BehindBackground));

    // Once a layer exists there is something to actually composite, so a
    // separate buffer is expected.
    const auto layerPixels = slice.MutablePixels(0, 4, ImageSlice::LayerKey{ .imageId = 1, .placementId = 1 }, 0);
    VERIFY_IS_NOT_NULL(layerPixels);
    Fill(slice, layerPixels, 4, Blue);
    VERIFY_ARE_NOT_EQUAL(slice.Pixels().data(), slice.Pixels(ImageSlice::RenderPosition::AboveText).data());
}
// A copy has always meant "the destination range now equals the source range".
// A destination layer with no counterpart in the source must not survive it.
void ImageSliceTests::CopyingReplacesDestinationLayers()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 2 }, TextAttribute{}, 0, false, &renderer };
    constexpr ImageSlice::LayerKey source{ .imageId = 1, .placementId = 1 };
    constexpr ImageSlice::LayerKey destination{ .imageId = 2, .placementId = 2 };

    FillLayer(buffer.GetMutableRowByOffset(0), source, 4, Red);
    FillLayer(buffer.GetMutableRowByOffset(1), destination, 4, Blue);

    ImageSlice::CopyCells(buffer.GetRowByOffset(0), 0, buffer.GetMutableRowByOffset(1), 0, 4);

    const auto slice = buffer.GetRowByOffset(1).GetImageSlice();
    VERIFY_IS_NOT_NULL(slice);
    VERIFY_IS_TRUE(slice->Contains(source), L"the source layer should have arrived");
    VERIFY_IS_FALSE(slice->Contains(destination), L"the overwritten layer should be gone");
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(slice->Pixels(ImageSlice::RenderPosition::AboveText), *slice, 0));
}

// The source having no layers at all is the worst case: nothing iterates, so
// nothing would clear the destination unless the copy does it up front.
void ImageSliceTests::CopyingFromAnUnlayeredSourceClearsDestinationLayers()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 2 }, TextAttribute{}, 0, false, &renderer };
    constexpr ImageSlice::LayerKey destination{ .imageId = 2, .placementId = 2 };

    FillBase(buffer.GetMutableRowByOffset(0), 4, Red);
    FillLayer(buffer.GetMutableRowByOffset(1), destination, 4, Blue);

    ImageSlice::CopyCells(buffer.GetRowByOffset(0), 0, buffer.GetMutableRowByOffset(1), 0, 4);

    const auto slice = buffer.GetRowByOffset(1).GetImageSlice();
    VERIFY_IS_NOT_NULL(slice);
    VERIFY_IS_FALSE(slice->Contains(destination), L"a layer must not outlive the copy that overwrote it");
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(slice->Pixels(ImageSlice::RenderPosition::AboveText), *slice, 0));
}

// The mirror image: a slice can hold layers and no untagged content at all, so
// there is no base plane to copy from. The destination's still has to go.
void ImageSliceTests::CopyingFromALayerOnlySourceClearsDestinationBasePlane()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 2 }, TextAttribute{}, 0, false, &renderer };
    constexpr ImageSlice::LayerKey source{ .imageId = 1, .placementId = 1 };

    FillLayer(buffer.GetMutableRowByOffset(0), source, 4, Red);
    FillBase(buffer.GetMutableRowByOffset(1), 4, Blue);
    VERIFY_IS_TRUE(buffer.GetRowByOffset(0).GetImageSlice()->Pixels().empty(), L"a layer-only slice has no base plane");

    ImageSlice::CopyCells(buffer.GetRowByOffset(0), 0, buffer.GetMutableRowByOffset(1), 0, 4);

    const auto slice = buffer.GetRowByOffset(1).GetImageSlice();
    VERIFY_IS_NOT_NULL(slice);
    VERIFY_IS_TRUE(slice->Contains(source));
    VERIFY_ARE_EQUAL(Packed(Clear), PixelAt(slice->Pixels(), *slice, 0), L"the untagged pixels should have been replaced by nothing");
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(slice->Pixels(ImageSlice::RenderPosition::AboveText), *slice, 0));
}

// CopyLayerCells takes a source column from the caller, and reflow derives that
// column from per-cell metadata rather than from the layer itself. Metadata can name
// a column the layer never covered, so the copy has to treat an uncovered source as
// nothing to carry rather than trusting the caller and indexing outside the plane --
// which would splice unrelated heap into a layer that is then composited and painted.
void ImageSliceTests::CopyingACellOutsideTheSourceLayerCopiesNothing()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 2 }, TextAttribute{}, 0, false, &renderer };
    constexpr ImageSlice::LayerKey key{ .imageId = 1, .placementId = 1 };

    // The source layer covers columns 0-1 only.
    FillLayer(buffer.GetMutableRowByOffset(0), key, 2, Red);
    const auto source = buffer.GetRowByOffset(0).GetImageSlice();
    VERIFY_IS_NOT_NULL(source);
    VERIFY_IS_TRUE(source->LayerCoversColumn(key, 1), L"sanity: column 1 is covered");
    VERIFY_IS_FALSE(source->LayerCoversColumn(key, 5), L"sanity: column 5 is not");

    FillLayer(buffer.GetMutableRowByOffset(1), key, 4, Blue);

    // Column 5 is past the source layer. Like every copy here the destination range is
    // cleared first, so the observable result must be transparency -- the source had
    // nothing for this column. What must never appear is bytes: neither the layer's own
    // Red, which lives at a different column, nor whatever follows the allocation.
    ImageSlice::CopyLayerCells(buffer.GetRowByOffset(0), 5, buffer.GetMutableRowByOffset(1), 0, 1, key);

    const auto destination = buffer.GetRowByOffset(1).GetImageSlice();
    VERIFY_IS_NOT_NULL(destination);
    VERIFY_ARE_EQUAL(Packed(Clear), PixelAt(destination->Pixels(ImageSlice::RenderPosition::AboveText), *destination, 0), L"an uncovered source column must contribute no pixels");

    // A negative offset is the same hazard in the other direction.
    ImageSlice::CopyLayerCells(buffer.GetRowByOffset(0), -3, buffer.GetMutableRowByOffset(1), 1, 2, key);
    VERIFY_ARE_EQUAL(Packed(Clear), PixelAt(buffer.GetRowByOffset(1).GetImageSlice()->Pixels(ImageSlice::RenderPosition::AboveText), *buffer.GetRowByOffset(1).GetImageSlice(), 1), L"a negative source offset must not copy either");

    // And the in-range case still works, so the guard has not simply disabled the copy.
    ImageSlice::CopyLayerCells(buffer.GetRowByOffset(0), 0, buffer.GetMutableRowByOffset(1), 2, 3, key);
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(buffer.GetRowByOffset(1).GetImageSlice()->Pixels(ImageSlice::RenderPosition::AboveText), *buffer.GetRowByOffset(1).GetImageSlice(), 2), L"a covered source column still copies");
}

// Image cell size comes from the font, so it is not constant for the lifetime of
// a buffer: a placement made before the renderer reported the real font, or before
// the user changed its size, keeps the cell it was created with. Two rows can
// therefore hold slices at different scales, and an ordinary scroll will copy one
// onto the other. The copy walks the source planes with the destination's stride,
// so a larger destination cell would read past the end of the source and splice
// unrelated heap into pixels that are about to be composited and painted.
void ImageSliceTests::CopyingBetweenRowsOfDifferentCellSizesErases()
{
    static constexpr til::size LargeCell{ CellSize.width * 2, CellSize.height * 2 };
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 3 }, TextAttribute{}, 0, false, &renderer };

    // Row 0 is the small-cell source. Its base plane is only
    // (2 * 4) * 2 == 16 pixels, and the destination below wants to read 4 rows of
    // 16 from it.
    FillBase(buffer.GetMutableRowByOffset(0), 2, Red);
    // Row 1 is the large-cell destination, with content past the copy range so we
    // can tell an erase of the range apart from an erase of the row.
    FillBase(buffer.GetMutableRowByOffset(1), 4, Blue, LargeCell);
    VERIFY_ARE_EQUAL(CellSize, buffer.GetRowByOffset(0).GetImageSlice()->CellSize());
    VERIFY_ARE_EQUAL(LargeCell, buffer.GetRowByOffset(1).GetImageSlice()->CellSize());

    ImageSlice::CopyCells(buffer.GetRowByOffset(0), 0, buffer.GetMutableRowByOffset(1), 0, 2);

    const auto destination = buffer.GetRowByOffset(1).GetImageSlice();
    VERIFY_IS_NOT_NULL(destination);
    VERIFY_ARE_EQUAL(LargeCell, destination->CellSize(), L"the destination keeps its own geometry");
    // Without the guard the first source row is copied verbatim, so column 0 would
    // read back Red -- and the last would be read from past the source allocation.
    VERIFY_ARE_EQUAL(Packed(Clear), PixelAt(destination->Pixels(), *destination, 0), L"a mismatched copy must erase, not rescale");
    VERIFY_ARE_EQUAL(Packed(Blue), PixelAt(destination->Pixels(), *destination, 2), L"only the copied range should have been erased");

    // The other direction stays in bounds but is just as meaningless, and is
    // handled the same way.
    FillBase(buffer.GetMutableRowByOffset(2), 4, Red);
    ImageSlice::CopyCells(buffer.GetRowByOffset(1), 0, buffer.GetMutableRowByOffset(2), 0, 2);
    const auto narrowSlice = buffer.GetRowByOffset(2).GetImageSlice();
    VERIFY_IS_NOT_NULL(narrowSlice);
    VERIFY_ARE_EQUAL(Packed(Clear), PixelAt(narrowSlice->Pixels(), *narrowSlice, 0), L"a smaller destination must not take a larger source's pixels either");
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(narrowSlice->Pixels(), *narrowSlice, 2), L"and again, only the copied range should have gone");

    // And a copy between rows that do agree still works, so the guard has not
    // simply disabled copying.
    ImageSlice::CopyCells(buffer.GetRowByOffset(0), 0, buffer.GetMutableRowByOffset(2), 0, 2);
    VERIFY_ARE_EQUAL(Packed(Red), PixelAt(buffer.GetRowByOffset(2).GetImageSlice()->Pixels(), *buffer.GetRowByOffset(2).GetImageSlice(), 0), L"matching cell sizes still copy");
}