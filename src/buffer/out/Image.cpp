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
        const auto width = static_cast<size_t>(size.width);
        const auto height = static_cast<size_t>(size.height);
        THROW_HR_IF(E_INVALIDARG, width > SIZE_MAX / height);
        return width * height;
    }

    bool imageLess(const ImagePlacement* lhs, const ImagePlacement* rhs) noexcept
    {
        if (lhs->Position() != rhs->Position())
        {
            return lhs->Position() < rhs->Position();
        }
        if (lhs->ZIndex() != rhs->ZIndex())
        {
            return lhs->ZIndex() < rhs->ZIndex();
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
    _pixelSize{ pixelSize },
    _pixels{ std::move(pixels) },
    _revision{ nextRevision() }
{
    THROW_HR_IF(E_INVALIDARG, _pixels.size() != checkedPixelCount(pixelSize));
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
    return _pixels;
}

void Image::UpdatePixels(const std::span<const RGBQUAD> pixels)
{
    THROW_HR_IF(E_INVALIDARG, pixels.size() != _pixels.size());
    std::copy(pixels.begin(), pixels.end(), _pixels.begin());
    _revision = nextRevision();
}

ImagePlacement::ImagePlacement(const Key key,
                               Image::Pointer image,
                               const til::rect cellBounds,
                               const int32_t zIndex,
                               til::rect sourceInPixels) :
    _key{ key },
    _image{ std::move(image) },
    _cellBounds{ cellBounds },
    _originalCellBounds{ cellBounds },
    _zIndex{ zIndex },
    _sourceInPixels{ sourceInPixels }
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
}

ImagePlacement::ImagePlacement(const Key key,
                               Image::Pointer image,
                               const til::rect cellBounds,
                               const til::rect originalCellBounds,
                               const int32_t zIndex,
                               const til::rect sourceInPixels) noexcept :
    _key{ key },
    _image{ std::move(image) },
    _cellBounds{ cellBounds },
    _originalCellBounds{ originalCellBounds },
    _zIndex{ zIndex },
    _sourceInPixels{ sourceInPixels }
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
    return ImagePlacement{ _key, _image, clipped, _originalCellBounds, _zIndex, _sourceInPixels };
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
    };
}

struct ImageCollection::RowIndex
{
    using Tree = interval_tree::IntervalTree<til::CoordType, size_t>;

    explicit RowIndex(const std::vector<ImagePlacement>& images)
    {
        Tree::interval_vector intervals;
        intervals.reserve(images.size());
        for (size_t i = 0; i < images.size(); ++i)
        {
            const auto bounds = images[i].CellBounds();
            intervals.emplace_back(bounds.top, bounds.bottom - 1, i);
        }
        tree = Tree{ std::move(intervals) };
    }

    Tree tree;
};

ImageCollection::ImageCollection() = default;
ImageCollection::~ImageCollection() = default;
ImageCollection::ImageCollection(ImageCollection&&) noexcept = default;
ImageCollection& ImageCollection::operator=(ImageCollection&&) noexcept = default;

void ImageCollection::Add(ImagePlacement image)
{
    _images.emplace_back(std::move(image));
    _markIndexDirty();
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

size_t ImageCollection::EraseImage(const uint32_t imageId)
{
    const auto oldSize = _images.size();
    std::erase_if(_images, [&](const auto& image) {
        return image.Identity().imageId == imageId;
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
    if (area.empty() || _images.empty())
    {
        return;
    }

    const auto intersects = [&](const auto& image) {
        return !(image.CellBounds() & area).empty();
    };
    if (std::none_of(_images.begin(), _images.end(), intersects))
    {
        return;
    }

    std::vector<ImagePlacement> remaining;
    remaining.reserve(_images.size());
    for (auto& image : _images)
    {
        const auto bounds = image.CellBounds();
        if ((bounds & area).empty())
        {
            remaining.emplace_back(std::move(image));
            continue;
        }

        for (const auto fragment : bounds - area)
        {
            if (auto cropped = image.Crop(fragment))
            {
                remaining.emplace_back(std::move(*cropped));
            }
        }
    }

    _images = std::move(remaining);
    _markIndexDirty();
}

void ImageCollection::CopyArea(const til::rect source, const til::point target, ImageCollection& destination) const
{
    if (source.empty())
    {
        return;
    }

    std::vector<ImagePlacement> copied;
    const til::point delta{ target.x - source.left, target.y - source.top };
    for (const auto& image : _images)
    {
        if (auto fragment = image.Crop(source))
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
    destination.EraseArea(targetArea);
    for (auto& image : copied)
    {
        destination.Add(std::move(image));
    }
}

void ImageCollection::Translate(const til::point delta)
{
    if (delta == til::point{} || _images.empty())
    {
        return;
    }

    for (auto& image : _images)
    {
        image = image.Translated(delta);
    }
    _markIndexDirty();
}

void ImageCollection::ClipArea(const til::rect area)
{
    std::vector<ImagePlacement> clipped;
    clipped.reserve(_images.size());
    for (const auto& image : _images)
    {
        if (auto fragment = image.Crop(area))
        {
            clipped.emplace_back(std::move(*fragment));
        }
    }

    if (clipped.size() != _images.size() ||
        !std::equal(clipped.begin(), clipped.end(), _images.begin(), [](const auto& lhs, const auto& rhs) {
            return lhs.CellBounds() == rhs.CellBounds();
        }))
    {
        _images = std::move(clipped);
        _markIndexDirty();
    }
}

void ImageCollection::PrepareRowIndex() const
{
    _ensureRowIndex();
}

ImageCollection::RowQuery ImageCollection::IntersectingRows(const til::CoordType rowBegin, const til::CoordType rowEnd) const
{
    RowQuery result;
    if (rowBegin >= rowEnd || _images.empty())
    {
        return result;
    }

    _ensureRowIndex();
    _rowIndex->tree.visit_overlapping(rowBegin, rowEnd - 1, [&](const auto& interval) {
        result.emplace_back(&_images[interval.value]);
    });
    std::sort(result.begin(), result.end(), imageLess);
    return result;
}

std::span<const ImagePlacement> ImageCollection::All() const noexcept
{
    return _images;
}

size_t ImageCollection::Size() const noexcept
{
    return _images.size();
}

bool ImageCollection::Empty() const noexcept
{
    return _images.empty();
}

void ImageCollection::_markIndexDirty() noexcept
{
    _rowIndexDirty = true;
}

void ImageCollection::_ensureRowIndex() const
{
    if (_rowIndexDirty)
    {
        _rowIndex = std::make_unique<RowIndex>(_images);
        _rowIndexDirty = false;
    }
}
