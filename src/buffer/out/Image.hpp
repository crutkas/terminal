// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "til.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

// Pixel storage that maps directly to one renderer/GPU surface. Placements and
// fragments share this object, so splitting an image never duplicates pixels.
class Image final
{
public:
    using Pointer = std::shared_ptr<Image>;

    Image(til::size pixelSize, std::vector<RGBQUAD> pixels);

    til::size PixelSize() const noexcept;
    uint64_t Revision() const noexcept;
    std::span<const RGBQUAD> Pixels() const noexcept;
    void UpdatePixels(std::span<const RGBQUAD> pixels);

private:
    til::size _pixelSize;
    std::vector<RGBQUAD> _pixels;
    uint64_t _revision = 0;
};

// A rectangular draw of an Image in buffer-cell coordinates. `_cellBounds`
// may be a clipped fragment of `_originalCellBounds`; the renderer uses the
// original bounds for sampling and the fragment bounds as its clip.
class ImagePlacement final
{
public:
    struct Key
    {
        uint32_t imageId = 0;
        uint64_t layerId = 0;

        constexpr bool operator==(const Key&) const noexcept = default;
    };

    enum class RenderPosition : uint8_t
    {
        BehindBackground,
        BehindText,
        AboveText,
    };

    static constexpr int32_t BackgroundZThreshold = INT32_MIN / 2;

    ImagePlacement(Key key,
                   Image::Pointer image,
                   til::rect cellBounds,
                   int32_t zIndex,
                   til::rect sourceInPixels = {});

    Key Identity() const noexcept;
    const Image& Surface() const noexcept;
    const Image::Pointer& SurfacePointer() const noexcept;
    til::rect CellBounds() const noexcept;
    til::rect OriginalCellBounds() const noexcept;
    til::rect SourceInPixels() const noexcept;
    int32_t ZIndex() const noexcept;
    RenderPosition Position() const noexcept;

    std::optional<ImagePlacement> Crop(til::rect cellBounds) const;
    ImagePlacement Translated(til::point delta) const;

private:
    ImagePlacement(Key key,
                   Image::Pointer image,
                   til::rect cellBounds,
                   til::rect originalCellBounds,
                   int32_t zIndex,
                   til::rect sourceInPixels) noexcept;

    Key _key;
    Image::Pointer _image;
    til::rect _cellBounds;
    til::rect _originalCellBounds;
    int32_t _zIndex = 0;
    til::rect _sourceInPixels;
};

class ImageCollection final
{
public:
    using RowQuery = til::small_vector<const ImagePlacement*, 16>;

    ImageCollection();
    ~ImageCollection();
    ImageCollection(const ImageCollection&) = delete;
    ImageCollection& operator=(const ImageCollection&) = delete;
    ImageCollection(ImageCollection&&) noexcept;
    ImageCollection& operator=(ImageCollection&&) noexcept;

    void Add(ImagePlacement image);
    bool Erase(ImagePlacement::Key key);
    size_t EraseImage(uint32_t imageId);
    void EraseArea(til::rect area);
    void CopyArea(til::rect source, til::point target, ImageCollection& destination) const;
    void Translate(til::point delta);
    void ClipArea(til::rect area);
    void PrepareRowIndex() const;

    // Preparing the index after a batch of mutations keeps its allocations out
    // of the render query. Returned pointers remain valid until the next mutation.
    RowQuery IntersectingRows(til::CoordType rowBegin, til::CoordType rowEnd) const;
    std::span<const ImagePlacement> All() const noexcept;
    size_t Size() const noexcept;
    bool Empty() const noexcept;

private:
    struct RowIndex;

    void _markIndexDirty() noexcept;
    void _ensureRowIndex() const;

    std::vector<ImagePlacement> _images;
    mutable std::unique_ptr<RowIndex> _rowIndex;
    mutable bool _rowIndexDirty = true;
};
