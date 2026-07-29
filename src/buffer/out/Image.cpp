// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "Image.hpp"

namespace
{
    std::atomic<uint64_t> s_revision{ 1 };

    uint64_t nextRevision() noexcept
    {
        uint64_t revision = 0;
        do
        {
            revision = s_revision.fetch_add(1, std::memory_order_relaxed);
        } while (revision == 0);
        return revision;
    }

    size_t checkedPixelCount(const til::size size)
    {
        THROW_HR_IF(E_INVALIDARG, size.width <= 0 || size.height <= 0);
        THROW_HR_IF(E_NOTIMPL, size.width > Image::MaximumDimension || size.height > Image::MaximumDimension);
        const auto width = static_cast<size_t>(size.width);
        const auto height = static_cast<size_t>(size.height);
        THROW_HR_IF(E_INVALIDARG, width > SIZE_MAX / height);
        return width * height;
    }

    template<typename T>
    bool imageLess(const T& lhs, const T& rhs) noexcept
    {
        if (lhs->Position() != rhs->Position())
        {
            return lhs->Position() < rhs->Position();
        }
        if (lhs->ZIndex() != rhs->ZIndex())
        {
            return lhs->ZIndex() < rhs->ZIndex();
        }
        if (lhs->Identity().protocol != rhs->Identity().protocol)
        {
            return lhs->Identity().protocol < rhs->Identity().protocol;
        }
        if (lhs->Identity().imageId != rhs->Identity().imageId)
        {
            return lhs->Identity().imageId < rhs->Identity().imageId;
        }
        if (lhs->Identity().layerId != rhs->Identity().layerId)
        {
            return lhs->Identity().layerId < rhs->Identity().layerId;
        }
        const auto lhsBounds = lhs->CellBounds();
        const auto rhsBounds = rhs->CellBounds();
        return std::tie(lhsBounds.top, lhsBounds.left, lhsBounds.bottom, lhsBounds.right) <
               std::tie(rhsBounds.top, rhsBounds.left, rhsBounds.bottom, rhsBounds.right);
    }
}

Image::Image(const til::size pixelSize, std::vector<RGBQUAD> pixels) :
    Image{ pixelSize, std::make_shared<const std::vector<RGBQUAD>>(std::move(pixels)) }
{
}

Image::Image(const til::size pixelSize, PixelStorage pixels) :
    _pixelSize{ pixelSize },
    _pixels{ std::move(pixels) },
    _revision{ nextRevision() }
{
    THROW_HR_IF(E_INVALIDARG, !_pixels || _pixels->size() != checkedPixelCount(pixelSize));
}

til::size Image::PixelSize() const noexcept
{
    return _pixelSize;
}

uint64_t Image::Revision() const noexcept
{
    return _revision;
}

std::span<const RGBQUAD> Image::Pixels() const noexcept
{
    return *_pixels;
}

const Image::PixelStorage& Image::Storage() const noexcept
{
    return _pixels;
}

void Image::Resize(const til::size pixelSize)
{
    const auto pixelCount = checkedPixelCount(pixelSize);
    if (pixelSize == _pixelSize)
    {
        return;
    }

    auto pixels = std::make_shared<std::vector<RGBQUAD>>(pixelCount);
    const auto copyWidth = std::min(pixelSize.width, _pixelSize.width);
    const auto copyHeight = std::min(pixelSize.height, _pixelSize.height);
    for (auto y = 0; y < copyHeight; ++y)
    {
        const auto source = _pixels->begin() + gsl::narrow<size_t>(y) * gsl::narrow<size_t>(_pixelSize.width);
        const auto target = pixels->begin() + gsl::narrow<size_t>(y) * gsl::narrow<size_t>(pixelSize.width);
        std::copy_n(source, copyWidth, target);
    }
    UpdatePixels(pixelSize, std::move(pixels));
}

void Image::UpdatePixels(const std::span<const RGBQUAD> pixels)
{
    UpdatePixels(_pixelSize, pixels);
}

void Image::UpdatePixels(PixelStorage pixels)
{
    UpdatePixels(_pixelSize, std::move(pixels));
}

void Image::UpdatePixels(const til::size pixelSize, const std::span<const RGBQUAD> pixels)
{
    THROW_HR_IF(E_INVALIDARG, pixels.size() != checkedPixelCount(pixelSize));
    UpdatePixels(pixelSize, std::make_shared<const std::vector<RGBQUAD>>(pixels.begin(), pixels.end()));
}

void Image::UpdatePixels(const til::size pixelSize, PixelStorage pixels)
{
    const auto pixelCount = checkedPixelCount(pixelSize);
    THROW_HR_IF(E_INVALIDARG, !pixels || pixels->size() != pixelCount);

    // All validation and allocation happens before changing either field, so a
    // rejected resize leaves every placement with its prior valid surface.
    _updatePixels(pixelSize, std::move(pixels));
}

void Image::_updatePixels(const til::size pixelSize, PixelStorage pixels) noexcept
{
    _pixelSize = pixelSize;
    _pixels = std::move(pixels);
    _revision = nextRevision();
}

ImagePlacement::ImagePlacement(const Key key,
                               Image::Pointer image,
                               const til::rect cellBounds,
                               const int32_t zIndex,
                               til::rect sourceInPixels,
                               PixelGeometry geometry) :
    _key{ key },
    _image{ std::move(image) },
    _cellBounds{ cellBounds },
    _originalCellBounds{ cellBounds },
    _zIndex{ zIndex },
    _sourceInPixels{ sourceInPixels },
    _geometry{ geometry }
{
    THROW_HR_IF(E_INVALIDARG, !_image || cellBounds.empty());
    if (_sourceInPixels.empty())
    {
        const auto size = _image->PixelSize();
        _sourceInPixels = til::rect{ 0, 0, size.width, size.height };
    }

    const auto imageSize = _image->PixelSize();
    const til::rect imageBounds{ 0, 0, imageSize.width, imageSize.height };
    THROW_HR_IF(E_INVALIDARG, _sourceInPixels.empty() || (_sourceInPixels & imageBounds) != _sourceInPixels);
    THROW_HR_IF(E_INVALIDARG, _geometry.cellSize.width <= 0 || _geometry.cellSize.height <= 0);
    THROW_HR_IF(E_INVALIDARG, _geometry.offset.x < 0 || _geometry.offset.x >= _geometry.cellSize.width || _geometry.offset.y < 0 || _geometry.offset.y >= _geometry.cellSize.height);
    if (_geometry.targetWidth == 0)
    {
        _geometry.targetWidth = gsl::narrow_cast<uint64_t>(_sourceInPixels.width());
    }
    if (_geometry.targetHeight == 0)
    {
        _geometry.targetHeight = gsl::narrow_cast<uint64_t>(_sourceInPixels.height());
    }
    THROW_HR_IF(E_INVALIDARG, _geometry.targetWidth > INT64_MAX || _geometry.targetHeight > INT64_MAX);
}

ImagePlacement ImagePlacement::FromFragment(const Key key,
                                            Image::Pointer image,
                                            const til::rect cellBounds,
                                            const til::rect originalCellBounds,
                                            const int32_t zIndex,
                                            const til::rect sourceInPixels,
                                            const PixelGeometry geometry)
{
    THROW_HR_IF(E_INVALIDARG, originalCellBounds.right <= originalCellBounds.left || originalCellBounds.bottom <= originalCellBounds.top || cellBounds.left < originalCellBounds.left || cellBounds.top < originalCellBounds.top || cellBounds.right > originalCellBounds.right || cellBounds.bottom > originalCellBounds.bottom);
    auto placement = ImagePlacement{ key, std::move(image), cellBounds, zIndex, sourceInPixels, geometry };
    placement._originalCellBounds = originalCellBounds;
    return placement;
}

ImagePlacement::ImagePlacement(const Key key,
                               Image::Pointer image,
                               const til::rect cellBounds,
                               const til::rect originalCellBounds,
                               const int32_t zIndex,
                               const til::rect sourceInPixels,
                               const PixelGeometry geometry,
                               const uint64_t rowEpoch) noexcept :
    _key{ key },
    _image{ std::move(image) },
    _cellBounds{ cellBounds },
    _originalCellBounds{ originalCellBounds },
    _zIndex{ zIndex },
    _sourceInPixels{ sourceInPixels },
    _geometry{ geometry },
    _rowEpoch{ rowEpoch }
{
}

ImagePlacement::Key ImagePlacement::Identity() const noexcept
{
    return _key;
}

const Image& ImagePlacement::Surface() const noexcept
{
    return *_image;
}

const Image::Pointer& ImagePlacement::SurfacePointer() const noexcept
{
    return _image;
}

til::rect ImagePlacement::CellBounds() const noexcept
{
    return _cellBounds;
}

til::rect ImagePlacement::OriginalCellBounds() const noexcept
{
    return _originalCellBounds;
}

til::rect ImagePlacement::SourceInPixels() const noexcept
{
    return _sourceInPixels;
}

const ImagePlacement::PixelGeometry& ImagePlacement::Geometry() const noexcept
{
    return _geometry;
}

int32_t ImagePlacement::ZIndex() const noexcept
{
    return _zIndex;
}

ImagePlacement::RenderPosition ImagePlacement::Position() const noexcept
{
    if (_zIndex < BackgroundZThreshold)
    {
        return RenderPosition::BehindBackground;
    }
    return _zIndex < 0 ? RenderPosition::BehindText : RenderPosition::AboveText;
}

std::optional<ImagePlacement> ImagePlacement::Crop(const til::rect cellBounds) const
{
    const auto clipped = _cellBounds & cellBounds;
    if (clipped.empty())
    {
        return std::nullopt;
    }
    return ImagePlacement{ _key, _image, clipped, _originalCellBounds, _zIndex, _sourceInPixels, _geometry, _rowEpoch };
}

ImagePlacement ImagePlacement::Translated(const til::point delta) const
{
    return ImagePlacement{
        _key,
        _image,
        _cellBounds + delta,
        _originalCellBounds + delta,
        _zIndex,
        _sourceInPixels,
        _geometry,
        _rowEpoch,
    };
}

ImageCollection::LogicalPlacement::LogicalPlacement(const ImagePlacement* const placement,
                                                    const til::CoordType rowOffset,
                                                    const til::CoordType bufferHeight) noexcept :
    _placement{ placement },
    _rowOffset{ rowOffset },
    _bufferHeight{ bufferHeight }
{
}

const ImageCollection::LogicalPlacement* ImageCollection::LogicalPlacement::operator->() const noexcept
{
    return this;
}

ImagePlacement::Key ImageCollection::LogicalPlacement::Identity() const noexcept
{
    return _placement->Identity();
}

const Image& ImageCollection::LogicalPlacement::Surface() const noexcept
{
    return _placement->Surface();
}

til::rect ImageCollection::LogicalPlacement::CellBounds() const noexcept
{
    auto bounds = _placement->CellBounds() + til::point{ 0, _rowOffset };
    bounds.top = std::max(bounds.top, til::CoordType{ 0 });
    bounds.bottom = std::min(bounds.bottom, _bufferHeight);
    return bounds;
}

til::rect ImageCollection::LogicalPlacement::OriginalCellBounds() const noexcept
{
    return _placement->OriginalCellBounds() + til::point{ 0, _rowOffset };
}

const ImagePlacement::PixelGeometry& ImageCollection::LogicalPlacement::Geometry() const noexcept
{
    return _placement->Geometry();
}

int32_t ImageCollection::LogicalPlacement::ZIndex() const noexcept
{
    return _placement->ZIndex();
}

ImagePlacement::RenderPosition ImageCollection::LogicalPlacement::Position() const noexcept
{
    return _placement->Position();
}

std::optional<ImagePlacement> ImageCollection::LogicalPlacement::Crop(const til::rect cellBounds) const
{
    const auto logical = _placement->Translated({ 0, _rowOffset });
    return logical.Crop(cellBounds & CellBounds());
}

struct ImageCollection::RowIndex
{
    using Tree = interval_tree::IntervalTree<uint64_t, size_t>;

    explicit RowIndex(Tree::interval_vector intervals) :
        tree{ std::move(intervals) }
    {
    }

    Tree tree;
};

ImageCollection::ImageCollection() = default;
ImageCollection::~ImageCollection() = default;
ImageCollection::ImageCollection(ImageCollection&&) noexcept = default;
ImageCollection& ImageCollection::operator=(ImageCollection&&) noexcept = default;

ImageCollection ImageCollection::Snapshot() const
{
    ImageCollection snapshot;
    snapshot._images = _images;
    snapshot._rowEpoch = _rowEpoch;
    snapshot._lastPurgeEpoch = _lastPurgeEpoch;
    snapshot._bufferHeight = _bufferHeight;
    return snapshot;
}

std::vector<ImagePlacement> ImageCollection::_eraseAreas(const std::span<const ImagePlacement> images, const std::span<const til::rect> areas)
{
    std::vector<ImagePlacement> remaining{ images.begin(), images.end() };
    for (const auto area : areas)
    {
        if (area.empty())
        {
            continue;
        }

        const auto intersects = [&](const auto& image) {
            return !(image.CellBounds() & area).empty();
        };
        if (std::none_of(remaining.begin(), remaining.end(), intersects))
        {
            continue;
        }

        std::vector<ImagePlacement> next;
        next.reserve(remaining.size());
        for (const auto& image : remaining)
        {
            const auto bounds = image.CellBounds();
            if ((bounds & area).empty())
            {
                next.emplace_back(image);
                continue;
            }

            for (const auto fragment : bounds - area)
            {
                if (auto cropped = image.Crop(fragment))
                {
                    next.emplace_back(std::move(*cropped));
                }
            }
        }
        remaining = std::move(next);
    }
    return remaining;
}

void ImageCollection::_replace(std::vector<ImagePlacement> images) noexcept
{
    for (auto& image : images)
    {
        image._rowEpoch = _rowEpoch;
    }
    _images = std::move(images);
    _markIndexDirty();
}

void ImageCollection::Add(ImagePlacement image)
{
    image._rowEpoch = _rowEpoch;
    _images.emplace_back(std::move(image));
    _markIndexDirty();
}

void ImageCollection::AddOrReplace(ImagePlacement image)
{
    const auto key = image.Identity();
    image._rowEpoch = _rowEpoch;
    const auto current = All();
    std::vector<ImagePlacement> images{ current.begin(), current.end() };
    std::erase_if(images, [&](const auto& candidate) {
        return candidate.Identity() == key;
    });
    images.emplace_back(std::move(image));
    _replace(std::move(images));
}

void ImageCollection::AddOrReplace(ImagePlacement image,
                                   const Image::Pointer& surface,
                                   const til::size pixelSize,
                                   Image::PixelStorage pixels)
{
    THROW_HR_IF(E_INVALIDARG, !surface);
    image._image = surface;
    image._rowEpoch = _rowEpoch;

    const auto key = image.Identity();
    const auto current = All();
    std::vector<ImagePlacement> images{ current.begin(), current.end() };
    std::erase_if(images, [&](const auto& candidate) {
        return candidate.Identity() == key;
    });
    images.emplace_back(std::move(image));

    // The replacement collection has already allocated successfully. Updating
    // the surface is transactional, and the final vector swap cannot fail.
    surface->UpdatePixels(pixelSize, std::move(pixels));
    _replace(std::move(images));
}

void ImageCollection::AddOrReplaceArea(ImagePlacement image)
{
    const auto key = image.Identity();
    const auto area = image.CellBounds();
    const auto current = All();
    std::vector<ImagePlacement> remaining;
    remaining.reserve(current.size() + 1);
    for (const auto& candidate : current)
    {
        const auto bounds = candidate.CellBounds();
        if (candidate.Identity() != key || (bounds & area).empty())
        {
            remaining.emplace_back(candidate);
            continue;
        }

        for (const auto fragment : bounds - area)
        {
            if (auto cropped = candidate.Crop(fragment))
            {
                remaining.emplace_back(std::move(*cropped));
            }
        }
    }
    remaining.emplace_back(std::move(image));
    _replace(std::move(remaining));
}

void ImageCollection::ReplaceProtocolAreas(const ImagePlacement::Key::Protocol protocol,
                                           const std::span<const til::rect> areas,
                                           const std::span<const ProtocolReplacement> replacements)
{
    for (const auto& replacement : replacements)
    {
        THROW_HR_IF(E_INVALIDARG, replacement.placement.Identity().protocol != protocol);
        if (replacement.pixels)
        {
            const auto size = replacement.placement.Surface().PixelSize();
            const auto pixelCount = checkedPixelCount(size);
            THROW_HR_IF(E_INVALIDARG, replacement.pixels->size() != pixelCount);
        }
    }

    const auto current = All();
    std::vector<ImagePlacement> remaining;
    remaining.reserve(current.size() + replacements.size());
    for (const auto& candidate : current)
    {
        if (candidate.Identity().protocol != protocol)
        {
            remaining.emplace_back(candidate);
            continue;
        }

        const std::array candidateArray{ candidate };
        auto fragments = _eraseAreas(candidateArray, areas);
        std::move(fragments.begin(), fragments.end(), std::back_inserter(remaining));
    }
    for (const auto& replacement : replacements)
    {
        remaining.emplace_back(replacement.placement);
    }

    // Every allocation and validation has completed. These updates and the
    // final vector swap are nonthrowing, so the batch cannot partially commit.
    for (const auto& replacement : replacements)
    {
        if (replacement.pixels)
        {
            const auto& surface = replacement.placement.SurfacePointer();
            surface->_updatePixels(surface->PixelSize(), replacement.pixels);
        }
    }
    _replace(std::move(remaining));
}

void ImageCollection::Clear() noexcept
{
    if (!_images.empty())
    {
        _images.clear();
        _markIndexDirty();
    }
}

size_t ImageCollection::EraseProtocol(const ImagePlacement::Key::Protocol protocol) noexcept
{
    const auto removed = std::erase_if(_images, [&](const auto& image) {
        return image.Identity().protocol == protocol;
    });
    if (removed != 0)
    {
        _markIndexDirty();
    }
    return removed;
}

bool ImageCollection::Erase(const ImagePlacement::Key key)
{
    const auto oldSize = _images.size();
    std::erase_if(_images, [&](const auto& image) {
        return image.Identity() == key;
    });
    const auto erased = _images.size() != oldSize;
    if (erased)
    {
        _markIndexDirty();
    }
    return erased;
}

size_t ImageCollection::EraseImage(const ImagePlacement::Key::Protocol protocol, const uint32_t imageId)
{
    const auto oldSize = _images.size();
    std::erase_if(_images, [&](const auto& image) {
        const auto key = image.Identity();
        return key.protocol == protocol && key.imageId == imageId;
    });
    const auto erased = oldSize - _images.size();
    if (erased != 0)
    {
        _markIndexDirty();
    }
    return erased;
}

void ImageCollection::EraseArea(const til::rect area)
{
    const std::array areas{ area };
    EraseAreas(areas);
}

void ImageCollection::EraseAreas(const std::span<const til::rect> areas)
{
    if (areas.empty() || _images.empty())
    {
        return;
    }

    const auto current = All();
    const auto intersects = std::any_of(current.begin(), current.end(), [&](const auto& image) {
        return std::any_of(areas.begin(), areas.end(), [&](const auto area) {
            return !area.empty() && !(image.CellBounds() & area).empty();
        });
    });
    if (!intersects)
    {
        return;
    }

    _replace(_eraseAreas(current, areas));
}

void ImageCollection::CopyArea(const til::rect source, const til::point target, ImageCollection& destination) const
{
    if (source.empty())
    {
        return;
    }

    std::vector<ImagePlacement> copied;
    const til::point delta{ target.x - source.left, target.y - source.top };
    for (const auto image : IntersectingRows(source.top, source.bottom))
    {
        if (auto fragment = image->Crop(source))
        {
            copied.emplace_back(fragment->Translated(delta));
        }
    }

    const til::rect targetArea{
        target.x,
        target.y,
        target.x + source.width(),
        target.y + source.height(),
    };
    const auto destinationCandidates = destination.IntersectingRows(targetArea.top, targetArea.bottom);
    const auto destinationIntersects = std::any_of(
        destinationCandidates.begin(),
        destinationCandidates.end(),
        [&](const auto& image) {
            return !(image->CellBounds() & targetArea).empty();
        });
    if (copied.empty() && !destinationIntersects)
    {
        return;
    }

    const std::array targetAreas{ targetArea };
    auto remaining = _eraseAreas(destination.All(), targetAreas);
    remaining.reserve(remaining.size() + copied.size());
    std::move(copied.begin(), copied.end(), std::back_inserter(remaining));
    destination._replace(std::move(remaining));
}

void ImageCollection::Translate(const til::point delta)
{
    if (delta == til::point{} || _images.empty())
    {
        return;
    }

    const auto current = All();
    std::vector<ImagePlacement> translated;
    translated.reserve(current.size());
    for (const auto& image : current)
    {
        translated.emplace_back(image.Translated(delta));
    }
    _replace(std::move(translated));
}

void ImageCollection::ClipArea(const til::rect area)
{
    const auto current = All();
    std::vector<ImagePlacement> clipped;
    clipped.reserve(current.size());
    auto changed = false;
    for (const auto& image : current)
    {
        if (auto fragment = image.Crop(area))
        {
            changed = changed || fragment->CellBounds() != image.CellBounds();
            clipped.emplace_back(std::move(*fragment));
        }
        else
        {
            changed = true;
        }
    }

    if (changed)
    {
        _replace(std::move(clipped));
    }
}

void ImageCollection::AdvanceRows(const til::CoordType rowCount, const til::CoordType bufferHeight)
{
    if (rowCount <= 0)
    {
        return;
    }
    THROW_HR_IF(E_INVALIDARG, bufferHeight <= 0);

    const auto count = static_cast<uint64_t>(rowCount);
    constexpr auto coordinateHeadroom = static_cast<uint64_t>(til::CoordTypeMax);
    if (_rowEpoch > std::numeric_limits<uint64_t>::max() - coordinateHeadroom - count)
    {
        auto rebased = std::vector<ImagePlacement>{};
        if (!_images.empty())
        {
            const auto current = All();
            rebased.assign(current.begin(), current.end());
        }
        _rowEpoch = 0;
        _lastPurgeEpoch = 0;
        _images = std::move(rebased);
        for (auto& image : _images)
        {
            image._rowEpoch = 0;
        }
        _rowIndexDirty = true;
    }
    _rowEpoch += count;
    _bufferHeight = bufferHeight;
    _markLogicalImagesDirty();

    if (_rowEpoch - _lastPurgeEpoch >= static_cast<uint64_t>(bufferHeight))
    {
        const auto oldSize = _images.size();
        std::erase_if(_images, [&](const auto& image) {
            const auto bounds = image.CellBounds();
            return image._rowEpoch > _rowEpoch ||
                   _rowEpoch - image._rowEpoch >= static_cast<uint64_t>(bounds.bottom);
        });
        _lastPurgeEpoch = _rowEpoch;
        if (_images.size() != oldSize)
        {
            _rowIndexDirty = true;
        }
    }
}

void ImageCollection::PrepareRowIndex() const
{
    _ensureRowIndex();
}

ImageCollection::RowQuery ImageCollection::IntersectingRows(const til::CoordType rowBegin, const til::CoordType rowEnd) const
{
    RowQuery result;
    if (rowBegin < 0 || rowBegin >= rowEnd || _images.empty())
    {
        return result;
    }

    _ensureRowIndex();
    const auto absoluteBegin = _rowEpoch + static_cast<uint64_t>(rowBegin);
    const auto absoluteEnd = _rowEpoch + static_cast<uint64_t>(rowEnd);
    _rowIndex->tree.visit_overlapping(absoluteBegin, absoluteEnd - 1, [&](const auto& interval) {
        const auto& image = _images[interval.value];
        if (const auto rowOffset = _logicalRowOffset(image))
        {
            result.push_back(LogicalPlacement{ &image, *rowOffset, _bufferHeight });
        }
    });
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return imageLess(lhs, rhs);
    });
    return result;
}

std::span<const ImagePlacement> ImageCollection::All() const
{
    _ensureLogicalImages();
    return _logicalImages;
}

size_t ImageCollection::Size() const noexcept
{
    return std::count_if(_images.begin(), _images.end(), [&](const auto& image) {
        return _logicalRowOffset(image).has_value();
    });
}

bool ImageCollection::Empty() const noexcept
{
    return std::none_of(_images.begin(), _images.end(), [&](const auto& image) {
        return _logicalRowOffset(image).has_value();
    });
}

uint64_t ImageCollection::RowEpoch() const noexcept
{
    return _rowEpoch;
}

std::optional<til::CoordType> ImageCollection::_logicalRowOffset(const ImagePlacement& image) const noexcept
{
    if (image._rowEpoch > _rowEpoch)
    {
        return std::nullopt;
    }

    const auto elapsedRows = _rowEpoch - image._rowEpoch;
    if (elapsedRows > static_cast<uint64_t>(til::CoordTypeMax))
    {
        return std::nullopt;
    }

    const auto offset = -static_cast<til::CoordType>(elapsedRows);
    const auto bounds = image.CellBounds();
    const auto logicalTop = static_cast<int64_t>(bounds.top) + offset;
    const auto logicalBottom = static_cast<int64_t>(bounds.bottom) + offset;
    if (logicalBottom <= 0 || logicalTop >= _bufferHeight)
    {
        return std::nullopt;
    }
    return offset;
}

void ImageCollection::_markLogicalImagesDirty() noexcept
{
    _logicalImagesDirty = true;
}

void ImageCollection::_markIndexDirty() noexcept
{
    _markLogicalImagesDirty();
    _rowIndexDirty = true;
}

void ImageCollection::_ensureLogicalImages() const
{
    if (_logicalImagesDirty)
    {
        std::vector<ImagePlacement> logicalImages;
        logicalImages.reserve(_images.size());
        for (const auto& image : _images)
        {
            const auto rowOffset = _logicalRowOffset(image);
            if (!rowOffset)
            {
                continue;
            }

            auto logical = image.Translated({ 0, *rowOffset });
            const auto bounds = logical.CellBounds();
            logical = *logical.Crop({
                bounds.left,
                std::max(bounds.top, til::CoordType{ 0 }),
                bounds.right,
                std::min(bounds.bottom, _bufferHeight),
            });
            logical._rowEpoch = _rowEpoch;
            logicalImages.emplace_back(std::move(logical));
        }
        _logicalImages = std::move(logicalImages);
        _logicalImagesDirty = false;
    }
}

void ImageCollection::_ensureRowIndex() const
{
    if (_rowIndexDirty)
    {
        RowIndex::Tree::interval_vector intervals;
        intervals.reserve(_images.size());
        for (size_t i = 0; i < _images.size(); ++i)
        {
            const auto& image = _images[i];
            const auto bounds = image.CellBounds();
            THROW_HR_IF(E_UNEXPECTED, image._rowEpoch > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(bounds.bottom));
            intervals.emplace_back(
                image._rowEpoch + static_cast<uint64_t>(bounds.top),
                image._rowEpoch + static_cast<uint64_t>(bounds.bottom) - 1,
                i);
        }
        _rowIndex = std::make_unique<RowIndex>(std::move(intervals));
        _rowIndexDirty = false;
    }
}
