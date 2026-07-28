// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "WexTestClass.h"
#include "../../inc/consoletaeftemplates.hpp"

#include "../Image.hpp"
#include "../textBuffer.hpp"
#include "../../../renderer/inc/DummyRenderer.hpp"

using namespace WEX::Logging;
using namespace WEX::TestExecution;

class ImageTests
{
    TEST_CLASS(ImageTests);

    TEST_METHOD(RowQueryIsHalfOpenAndZOrdered);
    TEST_METHOD(RowIndexRebuildsAfterMutation);
    TEST_METHOD(EraseOutsideImageIsNoOp);
    TEST_METHOD(EraseSplitsWithoutCopyingTheSurface);
    TEST_METHOD(AddOrReplaceCollapsesFragments);
    TEST_METHOD(CopyClipsAndTranslates);
    TEST_METHOD(SurfaceUpdateReachesEveryFragment);
    TEST_METHOD(BatchSurfaceUpdateResamplesMultipleImages);
    TEST_METHOD(RasterizeUsesCropScaleAndOffset);
    TEST_METHOD(TextBufferOwnsImagesOutsideRows);
    TEST_METHOD(CircularAndRegionalScrollUseLogicalRows);
    TEST_METHOD(RectangularCopyAndErasePreserveSampling);
    TEST_METHOD(CrossBufferBlankCopyErasesPlacement);
    TEST_METHOD(TraditionalResizeClipsAndRetainsImages);
    TEST_METHOD(ResizeRebuildHandlesMixedCellSizes);
    TEST_METHOD(ReflowAppliesDirectPlacementPolicy);

    static constexpr til::size CellSize{ 2, 3 };

    static Image::Pointer MakeSurface(const til::rect bounds)
    {
        const til::size pixelSize{
            bounds.width() * CellSize.width,
            bounds.height() * CellSize.height,
        };
        const auto pixelCount = static_cast<size_t>(pixelSize.width) * pixelSize.height;
        return std::make_shared<Image>(pixelSize, std::vector<RGBQUAD>(pixelCount));
    }

    static ImagePlacement MakePlacement(const ImagePlacement::Key key, const til::rect bounds, const int32_t zIndex = 0)
    {
        return ImagePlacement{
            key,
            MakeSurface(bounds),
            bounds,
            zIndex,
            {},
            {
                .cellSize = CellSize,
                .targetWidth = gsl::narrow_cast<uint64_t>(bounds.width() * CellSize.width),
                .targetHeight = gsl::narrow_cast<uint64_t>(bounds.height() * CellSize.height),
            },
        };
    }

    static void AddPlacement(TextBuffer& buffer, ImagePlacement placement)
    {
        const auto bounds = placement.CellBounds() & buffer.GetSize().ToExclusive();
        for (auto rowIndex = bounds.top; rowIndex < bounds.bottom; ++rowIndex)
        {
            auto& row = buffer.GetMutableRowByOffset(rowIndex);
            auto slice = row.GetMutableImageSlice();
            if (!slice)
            {
                slice = row.SetImageSlice(std::make_unique<ImageSlice>(placement.Geometry().cellSize));
            }
            THROW_HR_IF(E_UNEXPECTED, !placement.RasterizeRow(rowIndex, bounds.left, bounds.right, *slice));
        }
        buffer.GetMutableImages().Add(std::move(placement));
    }
};

void ImageTests::RowQueryIsHalfOpenAndZOrdered()
{
    ImageCollection images;
    images.Add(MakePlacement({ 1, 1 }, { 0, 0, 4, 10 }));
    images.Add(MakePlacement({ 2, 2 }, { 0, 10, 4, 14 }, 2));
    images.Add(MakePlacement({ 3, 3 }, { 0, 12, 4, 20 }, -1));
    images.Add(MakePlacement({ 4, 4 }, { 0, 15, 4, 16 }, ImagePlacement::BackgroundZThreshold - 1));
    images.Add(MakePlacement({ 5, 5 }, { 0, 16, 4, 18 }));

    const auto visible = images.IntersectingRows(10, 16);

    VERIFY_ARE_EQUAL(size_t{ 3 }, visible.size());
    VERIFY_ARE_EQUAL(4u, visible[0]->Identity().imageId);
    VERIFY_ARE_EQUAL(3u, visible[1]->Identity().imageId);
    VERIFY_ARE_EQUAL(2u, visible[2]->Identity().imageId);
}

void ImageTests::RowIndexRebuildsAfterMutation()
{
    ImageCollection images;
    images.Add(MakePlacement({ 1, 1 }, { 0, 0, 2, 2 }));
    images.PrepareRowIndex();
    VERIFY_ARE_EQUAL(size_t{ 0 }, images.IntersectingRows(5, 6).size());

    images.Add(MakePlacement({ 2, 2 }, { 0, 5, 2, 6 }));
    images.PrepareRowIndex();
    auto visible = images.IntersectingRows(5, 6);
    VERIFY_ARE_EQUAL(size_t{ 1 }, visible.size());
    VERIFY_ARE_EQUAL(2u, visible[0]->Identity().imageId);

    VERIFY_IS_TRUE(images.Erase({ 2, 2 }));
    VERIFY_ARE_EQUAL(size_t{ 0 }, images.IntersectingRows(5, 6).size());
}

void ImageTests::EraseOutsideImageIsNoOp()
{
    ImageCollection images;
    images.Add(MakePlacement({ 1, 1 }, { 0, 0, 2, 2 }));
    const auto surface = images.All()[0].SurfacePointer();

    images.EraseArea({ 4, 4, 6, 6 });

    VERIFY_ARE_EQUAL(size_t{ 1 }, images.Size());
    VERIFY_ARE_EQUAL(surface.get(), images.All()[0].SurfacePointer().get());
    const auto visible = images.IntersectingRows(0, 2);
    VERIFY_ARE_EQUAL(size_t{ 1 }, visible.size());
    VERIFY_ARE_EQUAL(1u, visible[0]->Identity().imageId);
}

void ImageTests::EraseSplitsWithoutCopyingTheSurface()
{
    ImageCollection images;
    images.Add(MakePlacement({ 1, 1 }, { 0, 0, 6, 6 }));
    const auto surface = images.All()[0].SurfacePointer();

    images.EraseArea({ 2, 2, 4, 4 });

    VERIFY_ARE_EQUAL(size_t{ 4 }, images.Size());
    size_t area = 0;
    for (const auto& image : images.All())
    {
        VERIFY_ARE_EQUAL(surface.get(), image.SurfacePointer().get());
        area += static_cast<size_t>(image.CellBounds().width()) * image.CellBounds().height();
    }
    VERIFY_ARE_EQUAL(size_t{ 32 }, area);
}

void ImageTests::AddOrReplaceCollapsesFragments()
{
    ImageCollection images;
    images.Add(MakePlacement({ 1, 1 }, { 0, 0, 6, 6 }));
    images.EraseArea({ 2, 2, 4, 4 });
    VERIFY_ARE_EQUAL(size_t{ 4 }, images.Size());

    auto replacement = MakePlacement({ 1, 1 }, { 10, 10, 12, 12 });
    const auto replacementSurface = replacement.SurfacePointer();
    images.AddOrReplace(std::move(replacement));

    VERIFY_ARE_EQUAL(size_t{ 1 }, images.Size());
    const til::rect expected{ 10, 10, 12, 12 };
    VERIFY_ARE_EQUAL(expected, images.All()[0].CellBounds());
    VERIFY_ARE_EQUAL(replacementSurface.get(), images.All()[0].SurfacePointer().get());
}

void ImageTests::CopyClipsAndTranslates()
{
    ImageCollection source;
    source.Add(MakePlacement({ 1, 1 }, { 1, 1, 5, 5 }));

    ImageCollection destination;
    destination.Add(MakePlacement({ 9, 9 }, { 10, 20, 12, 22 }));
    source.CopyArea({ 2, 2, 4, 4 }, { 10, 20 }, destination);

    VERIFY_ARE_EQUAL(size_t{ 1 }, source.Size());
    VERIFY_ARE_EQUAL(size_t{ 1 }, destination.Size());
    const auto& copy = destination.All()[0];
    const til::rect expectedBounds{ 10, 20, 12, 22 };
    const til::rect expectedOriginalBounds{ 9, 19, 13, 23 };
    VERIFY_ARE_EQUAL(1u, copy.Identity().imageId);
    VERIFY_ARE_EQUAL(expectedBounds, copy.CellBounds());
    VERIFY_ARE_EQUAL(expectedOriginalBounds, copy.OriginalCellBounds());
    VERIFY_ARE_EQUAL(source.All()[0].SurfacePointer().get(), copy.SurfacePointer().get());
}

void ImageTests::SurfaceUpdateReachesEveryFragment()
{
    ImageCollection images;
    images.Add(MakePlacement({ 1, 1 }, { 0, 0, 6, 6 }));
    const auto surface = images.All()[0].SurfacePointer();
    const auto oldRevision = surface->Revision();
    images.EraseArea({ 2, 2, 4, 4 });

    auto pixels = std::vector<RGBQUAD>(surface->Pixels().size(), RGBQUAD{ 1, 2, 3, 4 });
    surface->UpdatePixels(pixels);

    VERIFY_ARE_NOT_EQUAL(oldRevision, surface->Revision());
    for (const auto& fragment : images.All())
    {
        VERIFY_ARE_EQUAL(surface->Revision(), fragment.Surface().Revision());
        VERIFY_ARE_EQUAL(1, static_cast<int>(fragment.Surface().Pixels()[0].rgbBlue));
    }
}

void ImageTests::BatchSurfaceUpdateResamplesMultipleImages()
{
    ImageSlice row{ { 1, 1 } };
    *row.MutablePixels(0, 1, { 1, 1 }, 0) = {};
    *row.MutableSourceIndices(0, 1, { 1, 1 }, 0) = 0;
    *row.MutablePixels(1, 2, { 2, 2 }, 0) = {};
    *row.MutableSourceIndices(1, 2, { 2, 2 }, 0) = 0;

    const std::array first{ RGBQUAD{ 1, 0, 0, 255 } };
    const std::array second{ RGBQUAD{ 2, 0, 0, 255 } };
    const std::array updates{
        ImageSlice::ImageUpdate{ 1, first, 10 },
        ImageSlice::ImageUpdate{ 2, second, 11 },
    };

    VERIFY_IS_TRUE(row.UpdateImages(updates));
    const auto rendered = row.Pixels(ImageSlice::RenderPosition::AboveText);
    VERIFY_ARE_EQUAL(1, static_cast<int>(rendered[0].rgbBlue));
    VERIFY_ARE_EQUAL(2, static_cast<int>(rendered[1].rgbBlue));
    VERIFY_IS_FALSE(row.UpdateImages(updates));
}

void ImageTests::RasterizeUsesCropScaleAndOffset()
{
    const std::vector<RGBQUAD> pixels{
        RGBQUAD{ 1, 0, 0, 255 },
        RGBQUAD{ 2, 0, 0, 255 },
        RGBQUAD{ 3, 0, 0, 255 },
        RGBQUAD{ 4, 0, 0, 255 },
    };
    const auto surface = std::make_shared<Image>(til::size{ 4, 1 }, pixels);
    const ImagePlacement placement{
        { 1, 1 },
        surface,
        { 2, 3, 4, 4 },
        0,
        { 1, 0, 4, 1 },
        {
            .cellSize = { 2, 1 },
            .targetWidth = 3,
            .targetHeight = 1,
            .offset = { 1, 0 },
        },
    };
    ImageSlice row{ { 2, 1 } };

    VERIFY_IS_TRUE(placement.RasterizeRow(3, 2, 4, row));

    const auto rendered = row.Pixels(ImageSlice::RenderPosition::AboveText);
    VERIFY_ARE_EQUAL(size_t{ 4 }, rendered.size());
    VERIFY_ARE_EQUAL(0, static_cast<int>(rendered[0].rgbBlue));
    VERIFY_ARE_EQUAL(2, static_cast<int>(rendered[1].rgbBlue));
    VERIFY_ARE_EQUAL(3, static_cast<int>(rendered[2].rgbBlue));
    VERIFY_ARE_EQUAL(4, static_cast<int>(rendered[3].rgbBlue));
}

void ImageTests::TextBufferOwnsImagesOutsideRows()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 4 }, TextAttribute{}, 0, false, &renderer };
    buffer.GetMutableImages().Add(MakePlacement({ 1, 1 }, { 0, 0, 2, 2 }));

    buffer.GetMutableRowByOffset(0).Reset(TextAttribute{});

    VERIFY_ARE_EQUAL(size_t{ 1 }, buffer.GetImages().Size());
    VERIFY_IS_NULL(buffer.GetRowByOffset(0).GetImageSlice());
}

void ImageTests::CircularAndRegionalScrollUseLogicalRows()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 4 }, TextAttribute{}, 0, false, &renderer };
    auto placement = MakePlacement({ 1, 1 }, { 1, 0, 5, 3 });
    const auto surface = placement.SurfacePointer();
    AddPlacement(buffer, std::move(placement));

    buffer.IncrementCircularBuffer(TextAttribute{});

    VERIFY_ARE_EQUAL(uint64_t{ 1 }, buffer.GetImages().RowEpoch());
    VERIFY_ARE_EQUAL(size_t{ 1 }, buffer.GetImages().Size());
    const til::rect afterCircularBounds{ 1, 0, 5, 2 };
    const til::rect afterCircularOriginal{ 1, -1, 5, 2 };
    VERIFY_ARE_EQUAL(afterCircularBounds, buffer.GetImages().All()[0].CellBounds());
    VERIFY_ARE_EQUAL(afterCircularOriginal, buffer.GetImages().All()[0].OriginalCellBounds());

    buffer.ScrollRows(0, 1, 1);

    VERIFY_ARE_EQUAL(size_t{ 2 }, buffer.GetImages().Size());
    for (const auto& fragment : buffer.GetImages().All())
    {
        VERIFY_ARE_EQUAL(surface.get(), fragment.SurfacePointer().get());
    }
    const auto* copiedRow = buffer.GetRowByOffset(1).GetImageSlice();
    VERIFY_IS_NOT_NULL(copiedRow);
    VERIFY_ARE_EQUAL(1u, copiedRow->ColumnOwner(2));

    buffer.IncrementCircularBuffer(TextAttribute{});

    VERIFY_ARE_EQUAL(uint64_t{ 2 }, buffer.GetImages().RowEpoch());
    VERIFY_ARE_EQUAL(size_t{ 1 }, buffer.GetImages().Size());
    const til::rect finalBounds{ 1, 0, 5, 1 };
    const til::rect finalOriginal{ 1, -1, 5, 2 };
    VERIFY_ARE_EQUAL(finalBounds, buffer.GetImages().All()[0].CellBounds());
    VERIFY_ARE_EQUAL(finalOriginal, buffer.GetImages().All()[0].OriginalCellBounds());
    VERIFY_ARE_EQUAL(surface.get(), buffer.GetImages().All()[0].SurfacePointer().get());

    buffer.IncrementCircularBuffer(TextAttribute{});
    VERIFY_IS_TRUE(buffer.GetImages().Empty());
}

void ImageTests::RectangularCopyAndErasePreserveSampling()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 10, 4 }, TextAttribute{}, 0, false, &renderer };
    auto placement = MakePlacement({ 1, 1 }, { 1, 0, 6, 3 });
    const auto surface = placement.SurfacePointer();
    AddPlacement(buffer, std::move(placement));

    ImageSlice::CopyBlock(buffer, { 2, 1, 5, 3 }, buffer, { 6, 0, 9, 2 });

    VERIFY_ARE_EQUAL(size_t{ 2 }, buffer.GetImages().Size());
    const til::rect copiedBounds{ 6, 0, 9, 2 };
    const til::rect copiedOriginal{ 5, -1, 10, 2 };
    auto foundCopy = false;
    for (const auto& fragment : buffer.GetImages().All())
    {
        if (fragment.CellBounds() == copiedBounds)
        {
            foundCopy = true;
            VERIFY_ARE_EQUAL(copiedOriginal, fragment.OriginalCellBounds());
            VERIFY_ARE_EQUAL(surface.get(), fragment.SurfacePointer().get());
        }
    }
    VERIFY_IS_TRUE(foundCopy);
    VERIFY_ARE_EQUAL(1u, buffer.GetRowByOffset(0).GetImageSlice()->ColumnOwner(7));
    VERIFY_ARE_EQUAL(1u, buffer.GetRowByOffset(1).GetImageSlice()->ColumnOwner(7));

    ImageSlice::EraseBlock(buffer, { 7, 0, 8, 2 });

    VERIFY_ARE_EQUAL(size_t{ 3 }, buffer.GetImages().Size());
    for (const auto& fragment : buffer.GetImages().All())
    {
        VERIFY_ARE_EQUAL(surface.get(), fragment.SurfacePointer().get());
        if (fragment.CellBounds().left >= 6)
        {
            VERIFY_ARE_EQUAL(copiedOriginal, fragment.OriginalCellBounds());
        }
        VERIFY_IS_TRUE((fragment.CellBounds() & til::rect{ 7, 0, 8, 2 }).empty());
    }
    for (auto rowIndex = 0; rowIndex < 2; ++rowIndex)
    {
        const auto* slice = buffer.GetRowByOffset(rowIndex).GetImageSlice();
        VERIFY_IS_NOT_NULL(slice);
        VERIFY_ARE_EQUAL(1u, slice->ColumnOwner(6));
        VERIFY_ARE_EQUAL(0u, slice->ColumnOwner(7));
        VERIFY_ARE_EQUAL(1u, slice->ColumnOwner(8));
    }
}

void ImageTests::CrossBufferBlankCopyErasesPlacement()
{
    DummyRenderer renderer;
    TextBuffer source{ til::size{ 8, 2 }, TextAttribute{}, 0, false, &renderer };
    TextBuffer destination{ til::size{ 8, 2 }, TextAttribute{}, 0, false, &renderer };
    AddPlacement(destination, MakePlacement({ 1, 1 }, { 1, 0, 4, 1 }));

    source.CopyRow(0, 0, destination);

    VERIFY_IS_TRUE(destination.GetImages().Empty());
    VERIFY_IS_NULL(destination.GetRowByOffset(0).GetImageSlice());
}

void ImageTests::TraditionalResizeClipsAndRetainsImages()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 4 }, TextAttribute{}, 0, false, &renderer };
    auto placement = MakePlacement({ 1, 1 }, { 4, 1, 8, 4 });
    const auto surface = placement.SurfacePointer();
    AddPlacement(buffer, std::move(placement));

    buffer.ResizeTraditional({ 6, 3 });

    VERIFY_ARE_EQUAL(size_t{ 2 }, buffer.GetImages().Size());
    const til::rect originalBounds{ 4, 1, 8, 4 };
    size_t coveredArea = 0;
    for (const auto& fragment : buffer.GetImages().All())
    {
        VERIFY_ARE_EQUAL(originalBounds, fragment.OriginalCellBounds());
        VERIFY_ARE_EQUAL(surface.get(), fragment.SurfacePointer().get());
        coveredArea += static_cast<size_t>(fragment.CellBounds().width()) * fragment.CellBounds().height();
    }
    VERIFY_ARE_EQUAL(size_t{ 4 }, coveredArea);
    VERIFY_IS_NULL(buffer.GetRowByOffset(0).GetImageSlice());
    for (auto rowIndex = 1; rowIndex < 3; ++rowIndex)
    {
        const auto* slice = buffer.GetRowByOffset(rowIndex).GetImageSlice();
        VERIFY_IS_NOT_NULL(slice);
        VERIFY_ARE_EQUAL(1u, slice->ColumnOwner(4));
        VERIFY_ARE_EQUAL(1u, slice->ColumnOwner(5));
        VERIFY_ARE_EQUAL(0u, slice->ColumnOwner(6));
    }
}

void ImageTests::ResizeRebuildHandlesMixedCellSizes()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 2 }, TextAttribute{}, 0, false, &renderer };
    buffer.GetMutableImages().Add(MakePlacement({ 1, 1 }, { 0, 0, 2, 1 }, -1));

    const til::size smallCell{ 1, 1 };
    auto smallSurface = std::make_shared<Image>(til::size{ 2, 1 }, std::vector<RGBQUAD>(2));
    buffer.GetMutableImages().Add(ImagePlacement{
        { 2, 2 },
        std::move(smallSurface),
        { 0, 0, 2, 1 },
        1,
        {},
        {
            .cellSize = smallCell,
            .targetWidth = 2,
            .targetHeight = 1,
        },
    });

    buffer.ResizeTraditional({ 6, 2 });

    VERIFY_ARE_EQUAL(size_t{ 2 }, buffer.GetImages().Size());
    const auto* slice = buffer.GetRowByOffset(0).GetImageSlice();
    VERIFY_IS_NOT_NULL(slice);
    VERIFY_ARE_EQUAL(smallCell, slice->CellSize());
    VERIFY_IS_FALSE(slice->Contains({ 1, 1 }));
    VERIFY_IS_TRUE(slice->Contains({ 2, 2 }));
}

void ImageTests::ReflowAppliesDirectPlacementPolicy()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 6 }, TextAttribute{}, 0, false, &renderer };
    RowWriteState firstRow{ .text = L"ABCDEFGH" };
    buffer.Replace(0, TextAttribute{}, firstRow);
    buffer.SetWrapForced(0, true);
    RowWriteState secondRow{ .text = L"IJ" };
    buffer.Replace(1, TextAttribute{}, secondRow);
    buffer.GetCursor().SetPosition({ 1, 1 });

    auto wrappedPlacement = MakePlacement({ 1, 1 }, { 2, 0, 6, 2 });
    const auto wrappedSurface = wrappedPlacement.SurfacePointer();
    AddPlacement(buffer, std::move(wrappedPlacement));
    auto imageOnlyPlacement = MakePlacement({ 2, 2 }, { 0, 4, 1, 5 });
    const auto imageOnlySurface = imageOnlyPlacement.SurfacePointer();
    AddPlacement(buffer, std::move(imageOnlyPlacement));

    TextBuffer reflowed{ til::size{ 4, 8 }, TextAttribute{}, 0, false, &renderer };
    TextBuffer::Reflow(buffer, reflowed);

    VERIFY_ARE_EQUAL(size_t{ 5 }, reflowed.GetImages().Size());
    auto foundFirstSegment = false;
    auto foundFirstWrappedSegment = false;
    auto foundSecondSegment = false;
    auto foundSecondWrappedSegment = false;
    auto foundImageOnlyRow = false;
    for (const auto& fragment : reflowed.GetImages().All())
    {
        if (fragment.Identity() == ImagePlacement::Key{ 1, 1 })
        {
            VERIFY_ARE_EQUAL(wrappedSurface.get(), fragment.SurfacePointer().get());
            foundFirstSegment |= fragment.CellBounds() == til::rect{ 2, 0, 4, 1 };
            foundFirstWrappedSegment |= fragment.CellBounds() == til::rect{ 0, 1, 2, 2 };
            foundSecondSegment |= fragment.CellBounds() == til::rect{ 2, 2, 4, 3 };
            foundSecondWrappedSegment |= fragment.CellBounds() == til::rect{ 0, 3, 2, 4 };
        }
        else if (fragment.Identity() == ImagePlacement::Key{ 2, 2 })
        {
            VERIFY_ARE_EQUAL(imageOnlySurface.get(), fragment.SurfacePointer().get());
            foundImageOnlyRow |= fragment.CellBounds() == til::rect{ 0, 6, 1, 7 };
        }
    }

    VERIFY_IS_TRUE(foundFirstSegment, L"the first source segment follows its exact destination cells");
    VERIFY_IS_TRUE(foundFirstWrappedSegment, L"the wrapped part of the first source row becomes a distinct fragment");
    VERIFY_IS_TRUE(foundSecondSegment, L"the joined source row follows its first destination segment");
    VERIFY_IS_TRUE(foundSecondWrappedSegment, L"the joined source row also fragments when it wraps");
    VERIFY_IS_TRUE(foundImageOnlyRow, L"an image-only source row participates in reflow");
    VERIFY_ARE_EQUAL(1u, reflowed.GetRowByOffset(0).GetImageSlice()->ColumnOwner(2));
    VERIFY_ARE_EQUAL(1u, reflowed.GetRowByOffset(1).GetImageSlice()->ColumnOwner(0));
    VERIFY_ARE_EQUAL(1u, reflowed.GetRowByOffset(2).GetImageSlice()->ColumnOwner(2));
    VERIFY_ARE_EQUAL(1u, reflowed.GetRowByOffset(3).GetImageSlice()->ColumnOwner(0));
    VERIFY_ARE_EQUAL(2u, reflowed.GetRowByOffset(6).GetImageSlice()->ColumnOwner(0));
}
