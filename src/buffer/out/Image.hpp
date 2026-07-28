// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "til.h"

#include <memory>
#include <optional>
#include <span>
#include <vector>

class ImageSlice;

// Pixel storage that maps directly to one renderer/GPU surface. Placements and
// fragments share this object, so splitting an image never duplicates pixels.
class Image final
{
public:
    using Pointer = std::shared_ptr<Image>;
    using PixelStorage = std::shared_ptr<std::vector<RGBQUAD>>;

    Image(til::size pixelSize, std::vector<RGBQUAD> pixels);
    Image(til::size pixelSize, PixelStorage pixels);

    til::size PixelSize() const noexcept;
    uint64_t Revision() const noexcept;
    std::span<const RGBQUAD> Pixels() const noexcept;
    const PixelStorage& Storage() const noexcept;
    void UpdatePixels(std::span<const RGBQUAD> pixels);
    void UpdatePixels(PixelStorage pixels);

private:
    til::size _pixelSize;
    PixelStorage _pixels;
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

    struct PixelGeometry
    {
        til::size cellSize{ 1, 1 };
        uint64_t targetWidth = 0;
        uint64_t targetHeight = 0;
        til::point offset{};
    };

    ImagePlacement(Key key,
                   Image::Pointer image,
                   til::rect cellBounds,
                   int32_t zIndex,
                   til::rect sourceInPixels = {},
                   PixelGeometry geometry = {});
    static ImagePlacement FromFragment(Key key,
                                       Image::Pointer image,
                                       til::rect cellBounds,
                                       til::rect originalCellBounds,
                                       int32_t zIndex,
                                       til::rect sourceInPixels = {},
                                       PixelGeometry geometry = {});

    Key Identity() const noexcept;
    const Image& Surface() const noexcept;
    const Image::Pointer& SurfacePointer() const noexcept;
    til::rect CellBounds() const noexcept;
    til::rect OriginalCellBounds() const noexcept;
    til::rect SourceInPixels() const noexcept;
    const PixelGeometry& Geometry() const noexcept;
    int32_t ZIndex() const noexcept;
    RenderPosition Position() const noexcept;

    std::optional<ImagePlacement> Crop(til::rect cellBounds) const;
    ImagePlacement Translated(til::point delta) const;
    bool RasterizeRow(til::CoordType row, til::CoordType columnBegin, til::CoordType columnEnd, ImageSlice& destination) const;

private:
    ImagePlacement(Key key,
                   Image::Pointer image,
                   til::rect cellBounds,
                   til::rect originalCellBounds,
                   int32_t zIndex,
                   til::rect sourceInPixels,
                   PixelGeometry geometry,
                   uint64_t rowEpoch) noexcept;

    friend class ImageCollection;

    Key _key;
    Image::Pointer _image;
    til::rect _cellBounds;
    til::rect _originalCellBounds;
    int32_t _zIndex = 0;
    til::rect _sourceInPixels;
    PixelGeometry _geometry;
    uint64_t _rowEpoch = 0;
};

class ImageCollection final
{
public:
    class LogicalPlacement final
    {
    public:
        const LogicalPlacement* operator->() const noexcept;

        ImagePlacement::Key Identity() const noexcept;
        const Image& Surface() const noexcept;
        til::rect CellBounds() const noexcept;
        til::rect OriginalCellBounds() const noexcept;
        const ImagePlacement::PixelGeometry& Geometry() const noexcept;
        int32_t ZIndex() const noexcept;
        ImagePlacement::RenderPosition Position() const noexcept;
        std::optional<ImagePlacement> Crop(til::rect cellBounds) const;
        bool RasterizeRow(til::CoordType row, til::CoordType columnBegin, til::CoordType columnEnd, ImageSlice& destination) const;

    private:
        friend class ImageCollection;

        LogicalPlacement(const ImagePlacement* placement, til::CoordType rowOffset, til::CoordType bufferHeight) noexcept;

        const ImagePlacement* _placement = nullptr;
        til::CoordType _rowOffset = 0;
        til::CoordType _bufferHeight = til::CoordTypeMax;
    };

    using RowQuery = til::small_vector<LogicalPlacement, 16>;

    ImageCollection();
    ~ImageCollection();
    ImageCollection(const ImageCollection&) = delete;
    ImageCollection& operator=(const ImageCollection&) = delete;
    ImageCollection(ImageCollection&&) noexcept;
    ImageCollection& operator=(ImageCollection&&) noexcept;

    void Add(ImagePlacement image);
    void AddOrReplace(ImagePlacement image);
    void AddOrReplaceArea(ImagePlacement image);
    void Clear() noexcept;
    bool Erase(ImagePlacement::Key key);
    size_t EraseImage(uint32_t imageId);
    void EraseArea(til::rect area);
    void EraseAreas(std::span<const til::rect> areas);
    void CopyArea(til::rect source, til::point target, ImageCollection& destination) const;
    void Translate(til::point delta);
    void ClipArea(til::rect area);
    void AdvanceRows(til::CoordType rowCount, til::CoordType bufferHeight);
    void PrepareRowIndex() const;

    // Preparing the index after a batch of mutations keeps its allocations out
    // of the render query. RowQuery entries and All() spans are frame-local and
    // remain valid only until the next collection mutation.
    RowQuery IntersectingRows(til::CoordType rowBegin, til::CoordType rowEnd) const;
    std::span<const ImagePlacement> All() const;
    size_t Size() const noexcept;
    bool Empty() const noexcept;
    uint64_t RowEpoch() const noexcept;

private:
    struct RowIndex;

    static std::vector<ImagePlacement> _eraseAreas(std::span<const ImagePlacement> images, std::span<const til::rect> areas);
    void _replace(std::vector<ImagePlacement> images) noexcept;
    std::optional<til::CoordType> _logicalRowOffset(const ImagePlacement& image) const noexcept;
    void _markLogicalImagesDirty() noexcept;
    void _markIndexDirty() noexcept;
    void _ensureLogicalImages() const;
    void _ensureRowIndex() const;

    std::vector<ImagePlacement> _images;
    uint64_t _rowEpoch = 0;
    uint64_t _lastPurgeEpoch = 0;
    til::CoordType _bufferHeight = til::CoordTypeMax;
    mutable std::vector<ImagePlacement> _logicalImages;
    mutable std::unique_ptr<RowIndex> _rowIndex;
    mutable bool _logicalImagesDirty = true;
    mutable bool _rowIndexDirty = true;
};
