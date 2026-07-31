// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "WexTestClass.h"
#include "../../inc/consoletaeftemplates.hpp"

#include "../Image.hpp"

using namespace WEX::Logging;
using namespace WEX::TestExecution;

class ImageTests
{
    TEST_CLASS(ImageTests);

    TEST_METHOD(RowQueryIsHalfOpenAndZOrdered);
    TEST_METHOD(RowIndexRebuildsAfterMutation);
    TEST_METHOD(EraseOutsideImageIsNoOp);
    TEST_METHOD(NonintersectingEraseDoesNotMaterializeLogicalPlacements);
    TEST_METHOD(EraseUsesClippedLogicalRowsAfterAdvance);
    TEST_METHOD(EraseSplitsWithoutCopyingTheSurface);
    TEST_METHOD(AddOrReplaceCollapsesFragments);
    TEST_METHOD(AddOrReplacePreservesScrolledPlacements);
    TEST_METHOD(BatchedAreaReplacementCommitsOnceAndPreservesGeometry);
    TEST_METHOD(CopyClipsAndTranslates);
    TEST_METHOD(SurfaceUpdateReachesEveryFragment);
    TEST_METHOD(SurfaceResizeIsTransactionalAndPreservesPixels);
    TEST_METHOD(ProtocolBatchValidationIsAtomic);
    TEST_METHOD(ProtocolIdentityPreventsCrossProtocolMutation);
    TEST_METHOD(LogicalCacheReleasesSurfacesOnInvalidation);

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

void ImageTests::NonintersectingEraseDoesNotMaterializeLogicalPlacements()
{
    auto placement = MakePlacement({ 1, 1 }, { 0, 2, 2, 4 });
    const auto surface = placement.SurfacePointer();
    ImageCollection images;
    images.Add(std::move(placement));
    images.All();
    images.PrepareRowIndex();
    images.AdvanceRows(1, 10);
    const auto revision = images.Revision();
    const auto surfaceReferences = surface.use_count();

    images.EraseArea({ 4, 1, 6, 3 });
    images.EraseArea({ 0, -2, 2, 1 });

    VERIFY_ARE_EQUAL(revision, images.Revision());
    VERIFY_ARE_EQUAL(surfaceReferences, surface.use_count(), L"a nonintersecting erase must not materialize logical placement copies");
    const auto visible = images.IntersectingRows(1, 3);
    VERIFY_ARE_EQUAL(size_t{ 1 }, visible.size());
    VERIFY_ARE_EQUAL(1u, visible[0]->Identity().imageId);
}

void ImageTests::EraseUsesClippedLogicalRowsAfterAdvance()
{
    ImageCollection images;
    images.Add(MakePlacement({ 1, 1 }, { 0, 0, 4, 2 }));
    images.AdvanceRows(1, 10);
    const auto revision = images.Revision();

    images.EraseArea({ 1, 0, 3, 1 });

    VERIFY_ARE_EQUAL(revision + 1, images.Revision());
    const auto visible = images.IntersectingRows(0, 1);
    VERIFY_ARE_EQUAL(size_t{ 2 }, visible.size());
    const til::rect expectedLeft{ 0, 0, 1, 1 };
    const til::rect expectedRight{ 3, 0, 4, 1 };
    VERIFY_ARE_EQUAL(expectedLeft, visible[0]->CellBounds());
    VERIFY_ARE_EQUAL(expectedRight, visible[1]->CellBounds());
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

void ImageTests::AddOrReplacePreservesScrolledPlacements()
{
    ImageCollection images;
    images.Add(MakePlacement({ 1, 1 }, { 0, 2, 2, 4 }));
    images.AdvanceRows(1, 10);

    images.AddOrReplace(MakePlacement({ 2, 2 }, { 4, 5, 6, 7 }));

    const auto existing = std::ranges::find(images.All(), uint32_t{ 1 }, [](const auto& image) {
        return image.Identity().imageId;
    });
    VERIFY_IS_TRUE(existing != images.All().end());
    if (existing != images.All().end())
    {
        const til::rect expected{ 0, 1, 2, 3 };
        VERIFY_ARE_EQUAL(expected, existing->CellBounds(), L"replacing another identity must not undo logical row scrolling");
    }
}

void ImageTests::BatchedAreaReplacementCommitsOnceAndPreservesGeometry()
{
    constexpr ImagePlacement::Key key{ 1, 9, ImagePlacement::Key::Protocol::Kitty };
    ImageCollection images;
    images.Add(MakePlacement(key, { 0, 2, 6, 4 }));
    images.AdvanceRows(1, 10);

    const auto logical = images.All();
    VERIFY_ARE_EQUAL(size_t{ 1 }, logical.size());
    const auto surface = logical[0].SurfacePointer();
    const auto originalBounds = logical[0].OriginalCellBounds();
    std::vector<ImagePlacement> replacements;
    replacements.emplace_back(*logical[0].Crop({ 1, 1, 2, 2 }));
    replacements.emplace_back(*logical[0].Crop({ 4, 2, 5, 3 }));
    const auto revision = images.Revision();

    images.AddOrReplaceAreas(std::move(replacements));

    VERIFY_ARE_EQUAL(revision + 1, images.Revision());
    VERIFY_ARE_EQUAL(uint64_t{ 1 }, images.RowEpoch());
    const auto result = images.All();
    VERIFY_IS_TRUE(result.size() >= 2);
    const til::rect firstReplacement{ 1, 1, 2, 2 };
    const til::rect secondReplacement{ 4, 2, 5, 3 };
    VERIFY_ARE_EQUAL(firstReplacement, result[result.size() - 2].CellBounds());
    VERIFY_ARE_EQUAL(secondReplacement, result[result.size() - 1].CellBounds());

    size_t coveredArea = 0;
    for (const auto& placement : result)
    {
        VERIFY_ARE_EQUAL(key, placement.Identity());
        VERIFY_ARE_EQUAL(originalBounds, placement.OriginalCellBounds());
        VERIFY_ARE_EQUAL(surface.get(), placement.SurfacePointer().get());
        coveredArea += static_cast<size_t>(placement.CellBounds().width()) * placement.CellBounds().height();
    }
    VERIFY_ARE_EQUAL(size_t{ 12 }, coveredArea);
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
    const auto oldStorage = surface->Storage();
    images.EraseArea({ 2, 2, 4, 4 });

    auto pixels = std::vector<RGBQUAD>(surface->Pixels().size(), RGBQUAD{ 1, 2, 3, 4 });
    surface->UpdatePixels(pixels);

    VERIFY_ARE_NOT_EQUAL(oldRevision, surface->Revision());
    VERIFY_ARE_NOT_EQUAL(oldStorage.get(), surface->Storage().get());
    VERIFY_ARE_EQUAL(0, static_cast<int>(oldStorage->front().rgbBlue), L"a frame snapshot must retain immutable pixels across revision updates");
    for (const auto& fragment : images.All())
    {
        VERIFY_ARE_EQUAL(surface->Revision(), fragment.Surface().Revision());
        VERIFY_ARE_EQUAL(1, static_cast<int>(fragment.Surface().Pixels()[0].rgbBlue));
    }
}

void ImageTests::SurfaceResizeIsTransactionalAndPreservesPixels()
{
    const std::vector<RGBQUAD> initial{
        RGBQUAD{ 1, 0, 0, 255 },
        RGBQUAD{ 2, 0, 0, 255 },
        RGBQUAD{ 3, 0, 0, 255 },
        RGBQUAD{ 4, 0, 0, 255 },
    };
    Image surface{ { 2, 2 }, initial };
    const auto initialStorage = surface.Storage();
    const auto initialRevision = surface.Revision();

    surface.Resize({ 3, 3 });

    const til::size expectedSize{ 3, 3 };
    VERIFY_ARE_EQUAL(expectedSize, surface.PixelSize());
    VERIFY_ARE_NOT_EQUAL(initialRevision, surface.Revision());
    VERIFY_ARE_NOT_EQUAL(initialStorage.get(), surface.Storage().get());
    VERIFY_ARE_EQUAL(1, static_cast<int>(surface.Pixels()[0].rgbBlue));
    VERIFY_ARE_EQUAL(2, static_cast<int>(surface.Pixels()[1].rgbBlue));
    VERIFY_ARE_EQUAL(3, static_cast<int>(surface.Pixels()[3].rgbBlue));
    VERIFY_ARE_EQUAL(4, static_cast<int>(surface.Pixels()[4].rgbBlue));
    VERIFY_ARE_EQUAL(0, static_cast<int>(surface.Pixels()[8].rgbReserved));

    const auto validStorage = surface.Storage();
    const auto validRevision = surface.Revision();
    VERIFY_THROWS_SPECIFIC(surface.Resize({ Image::MaximumDimension + 1, 1 }),
                           wil::ResultException,
                           [](wil::ResultException& e) { return e.GetErrorCode() == E_NOTIMPL; });
    VERIFY_ARE_EQUAL(expectedSize, surface.PixelSize());
    VERIFY_ARE_EQUAL(validRevision, surface.Revision());
    VERIFY_ARE_EQUAL(validStorage.get(), surface.Storage().get());

    const auto invalidPixels = std::make_shared<const std::vector<RGBQUAD>>(1);
    VERIFY_THROWS_SPECIFIC(surface.UpdatePixels({ 4, 4 }, invalidPixels),
                           wil::ResultException,
                           [](wil::ResultException& e) { return e.GetErrorCode() == E_INVALIDARG; });
    VERIFY_ARE_EQUAL(expectedSize, surface.PixelSize());
    VERIFY_ARE_EQUAL(validRevision, surface.Revision());
    VERIFY_ARE_EQUAL(validStorage.get(), surface.Storage().get());

    const auto sharedSurface = std::make_shared<Image>(til::size{ 1, 1 }, std::vector<RGBQUAD>{ RGBQUAD{ 9, 0, 0, 255 } });
    ImageCollection images;
    images.Add(ImagePlacement{
        { 8, 8, ImagePlacement::Key::Protocol::Sixel },
        sharedSurface,
        { 0, 0, 1, 1 },
        0,
    });
    const auto replacementPixels = std::make_shared<const std::vector<RGBQUAD>>(4, RGBQUAD{ 7, 0, 0, 255 });
    const auto stagedSurface = std::make_shared<Image>(til::size{ 2, 2 }, replacementPixels);
    auto replacement = ImagePlacement{
        { 8, 8, ImagePlacement::Key::Protocol::Sixel },
        stagedSurface,
        { 1, 1, 3, 3 },
        0,
    };
    const auto collectionRevision = sharedSurface->Revision();
    VERIFY_THROWS_SPECIFIC(images.AddOrReplace(std::move(replacement), sharedSurface, til::size{ 2, 2 }, invalidPixels),
                           wil::ResultException,
                           [](wil::ResultException& e) { return e.GetErrorCode() == E_INVALIDARG; });
    const til::rect originalPlacementBounds{ 0, 0, 1, 1 };
    const til::size originalSurfaceSize{ 1, 1 };
    VERIFY_ARE_EQUAL(originalPlacementBounds, images.All().front().CellBounds());
    VERIFY_ARE_EQUAL(originalSurfaceSize, sharedSurface->PixelSize());
    VERIFY_ARE_EQUAL(collectionRevision, sharedSurface->Revision());
}

void ImageTests::ProtocolBatchValidationIsAtomic()
{
    constexpr ImagePlacement::Key firstKey{ 0, 1, ImagePlacement::Key::Protocol::Sixel };
    constexpr ImagePlacement::Key secondKey{ 0, 2, ImagePlacement::Key::Protocol::Sixel };
    constexpr ImagePlacement::Key kittyKey{ 1, 1, ImagePlacement::Key::Protocol::Kitty };
    ImageCollection images;
    images.Add(MakePlacement(firstKey, { 0, 0, 2, 1 }));
    images.Add(MakePlacement(secondKey, { 0, 1, 2, 2 }));
    images.Add(MakePlacement(kittyKey, { 0, 0, 2, 2 }));

    const auto firstSurface = images.All()[0].SurfacePointer();
    const auto secondSurface = images.All()[1].SurfacePointer();
    const auto firstRevision = firstSurface->Revision();
    const auto secondRevision = secondSurface->Revision();
    const auto firstStorage = firstSurface->Storage();
    const auto secondStorage = secondSurface->Storage();
    const auto validPixels = std::make_shared<const std::vector<RGBQUAD>>(firstSurface->Pixels().size(), RGBQUAD{ 1, 2, 3, 255 });
    const auto invalidPixels = std::make_shared<const std::vector<RGBQUAD>>(1, RGBQUAD{ 4, 5, 6, 255 });
    const std::array areas{
        til::rect{ 0, 0, 2, 1 },
        til::rect{ 0, 1, 2, 2 },
    };
    const auto makeReplacement = [](const ImagePlacement::Key key, const Image::Pointer& surface, const til::rect area) {
        return ImagePlacement{
            key,
            surface,
            area,
            0,
            {},
            {
                .cellSize = CellSize,
                .targetWidth = gsl::narrow_cast<uint64_t>(surface->PixelSize().width),
                .targetHeight = gsl::narrow_cast<uint64_t>(surface->PixelSize().height),
            },
        };
    };
    const std::array invalidReplacements{
        ImageCollection::ProtocolReplacement{ makeReplacement(firstKey, firstSurface, areas[0]), validPixels },
        ImageCollection::ProtocolReplacement{ makeReplacement(secondKey, secondSurface, areas[1]), invalidPixels },
    };

    VERIFY_THROWS_SPECIFIC(images.ReplaceProtocolAreas(ImagePlacement::Key::Protocol::Sixel, areas, invalidReplacements),
                           wil::ResultException,
                           [](wil::ResultException& e) { return e.GetErrorCode() == E_INVALIDARG; });

    VERIFY_ARE_EQUAL(size_t{ 3 }, images.Size());
    VERIFY_ARE_EQUAL(firstRevision, firstSurface->Revision());
    VERIFY_ARE_EQUAL(secondRevision, secondSurface->Revision());
    VERIFY_ARE_EQUAL(firstStorage.get(), firstSurface->Storage().get());
    VERIFY_ARE_EQUAL(secondStorage.get(), secondSurface->Storage().get());
    VERIFY_IS_TRUE(std::ranges::any_of(images.All(), [&](const auto& placement) {
        return placement.Identity() == kittyKey;
    }));

    const auto secondValidPixels = std::make_shared<const std::vector<RGBQUAD>>(secondSurface->Pixels().size(), RGBQUAD{ 7, 8, 9, 255 });
    const std::array validReplacements{
        ImageCollection::ProtocolReplacement{ makeReplacement(firstKey, firstSurface, areas[0]), validPixels },
        ImageCollection::ProtocolReplacement{ makeReplacement(secondKey, secondSurface, areas[1]), secondValidPixels },
    };
    images.ReplaceProtocolAreas(ImagePlacement::Key::Protocol::Sixel, areas, validReplacements);

    VERIFY_ARE_NOT_EQUAL(firstRevision, firstSurface->Revision());
    VERIFY_ARE_NOT_EQUAL(secondRevision, secondSurface->Revision());
    VERIFY_ARE_EQUAL(1, static_cast<int>(firstSurface->Pixels().front().rgbBlue));
    VERIFY_ARE_EQUAL(7, static_cast<int>(secondSurface->Pixels().front().rgbBlue));
    VERIFY_ARE_EQUAL(size_t{ 3 }, images.Size());
    VERIFY_IS_TRUE(std::ranges::any_of(images.All(), [&](const auto& placement) {
                       return placement.Identity() == kittyKey;
                   }),
                   L"a successful Sixel batch must not erase an overlapping Kitty placement");
}

void ImageTests::ProtocolIdentityPreventsCrossProtocolMutation()
{
    constexpr ImagePlacement::Key kittyKey{ 7, 9, ImagePlacement::Key::Protocol::Kitty };
    constexpr ImagePlacement::Key sixelKey{ 7, 9, ImagePlacement::Key::Protocol::Sixel };
    VERIFY_IS_FALSE(kittyKey == sixelKey);

    ImageCollection images;
    auto kitty = MakePlacement(kittyKey, { 0, 0, 2, 1 });
    auto sixel = MakePlacement(sixelKey, { 0, 0, 2, 1 });
    const auto sixelSurface = sixel.SurfacePointer();
    images.Add(std::move(kitty));
    images.Add(std::move(sixel));

    VERIFY_ARE_EQUAL(size_t{ 2 }, images.Size());
    VERIFY_ARE_EQUAL(size_t{ 1 }, images.EraseImage(ImagePlacement::Key::Protocol::Kitty, kittyKey.imageId));
    VERIFY_ARE_EQUAL(size_t{ 1 }, images.Size());
    VERIFY_ARE_EQUAL(sixelKey, images.All()[0].Identity());
    VERIFY_ARE_EQUAL(sixelSurface.get(), images.All()[0].SurfacePointer().get());

    images.Add(MakePlacement(kittyKey, { 0, 0, 2, 1 }));
    VERIFY_ARE_EQUAL(size_t{ 1 }, images.EraseProtocol(ImagePlacement::Key::Protocol::Kitty));
    VERIFY_ARE_EQUAL(size_t{ 1 }, images.Size());
    VERIFY_ARE_EQUAL(sixelKey, images.All()[0].Identity());
}

void ImageTests::LogicalCacheReleasesSurfacesOnInvalidation()
{
    ImageCollection images;
    auto clearPlacement = MakePlacement({ 1, 1 }, { 0, 0, 2, 1 });
    auto clearSurface = clearPlacement.SurfacePointer();
    const std::weak_ptr<Image> clearWeak = clearSurface;
    images.Add(std::move(clearPlacement));
    images.All();
    images.PrepareRowIndex();
    clearSurface.reset();

    images.Clear();

    VERIFY_IS_TRUE(clearWeak.expired(), L"Clear must not leave a cached logical placement retaining the surface");
    VERIFY_IS_TRUE(images.IntersectingRows(0, 1).empty());

    auto erasedPlacement = MakePlacement({ 2, 2 }, { 0, 0, 2, 1 });
    auto erasedSurface = erasedPlacement.SurfacePointer();
    const std::weak_ptr<Image> erasedWeak = erasedSurface;
    images.Add(std::move(erasedPlacement));
    images.Add(MakePlacement({ 3, 3 }, { 0, 2, 2, 3 }));
    images.All();
    images.PrepareRowIndex();
    erasedSurface.reset();

    VERIFY_IS_TRUE(images.Erase({ 2, 2 }));

    VERIFY_IS_TRUE(erasedWeak.expired(), L"a non-Clear mutation must release stale cached logical placements immediately");
    VERIFY_IS_TRUE(images.IntersectingRows(0, 1).empty());
    VERIFY_ARE_EQUAL(size_t{ 1 }, images.IntersectingRows(2, 3).size(), L"row indexing must rebuild around the surviving placement");
}
