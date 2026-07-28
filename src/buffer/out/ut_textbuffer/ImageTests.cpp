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
    TEST_METHOD(CopyClipsAndTranslates);
    TEST_METHOD(SurfaceUpdateReachesEveryFragment);
    TEST_METHOD(TextBufferOwnsImagesOutsideRows);

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
        return ImagePlacement{ key, MakeSurface(bounds), bounds, zIndex };
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

void ImageTests::TextBufferOwnsImagesOutsideRows()
{
    DummyRenderer renderer;
    TextBuffer buffer{ til::size{ 8, 4 }, TextAttribute{}, 0, false, &renderer };
    buffer.GetMutableImages().Add(MakePlacement({ 1, 1 }, { 0, 0, 2, 2 }));

    buffer.GetMutableRowByOffset(0).Reset(TextAttribute{});

    VERIFY_ARE_EQUAL(size_t{ 1 }, buffer.GetImages().Size());
    VERIFY_IS_NULL(buffer.GetRowByOffset(0).GetImageSlice());
}
