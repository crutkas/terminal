/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- ImageSlice.hpp

Abstract:
- This serves as a structure to represent a slice of an image covering one textbuffer row.
--*/

#pragma once

#include "til.h"
#include <array>
#include <span>
#include <vector>

class ROW;
class TextBuffer;

class ImageSlice
{
public:
    using Pointer = std::unique_ptr<ImageSlice>;

    enum class RenderPosition : uint8_t
    {
        BehindBackground,
        BehindText,
        AboveText,
    };

    static constexpr int32_t BackgroundZThreshold = INT32_MIN / 2;
    static constexpr size_t MaxKittyLayerBytes = 320ull * 1024ull * 1024ull;
    static constexpr size_t MaxKittyLayersPerSlice = 4096;
    static size_t KittyLayerBytesAvailable() noexcept;

    ImageSlice(const ImageSlice& rhs);
    ImageSlice(ImageSlice&& rhs) noexcept;
    ImageSlice(const til::size cellSize) noexcept;
    ImageSlice& operator=(const ImageSlice&) = delete;
    ImageSlice& operator=(ImageSlice&&) = delete;
    ~ImageSlice() noexcept;

    void BumpRevision() noexcept;
    uint64_t Revision() const noexcept;
    uint64_t Revision(RenderPosition position) const noexcept;

    til::size CellSize() const noexcept;
    til::CoordType ColumnOffset() const noexcept;
    til::CoordType PixelWidth() const noexcept;

    // Per-column owner tags (0 = none/Sixel). Lets a protocol (e.g. Kitty) target
    // only the cells it owns for deletion without disturbing other images that
    // share the same row. Owners are kept aligned to [_columnBegin, _columnEnd).
    uint32_t ColumnOwner(const til::CoordType column) const noexcept;
    void SetColumnOwner(const til::CoordType columnBegin, const til::CoordType columnEnd, const uint32_t id);
    bool HasOwner(const uint32_t id) const noexcept;
    std::vector<uint32_t> ColumnOwners(const til::CoordType column) const;
    bool EraseByOwner(const uint32_t id);
    bool EraseByOwner(const uint32_t id, const til::CoordType columnBegin, const til::CoordType columnEnd);
    void ClearForeignColumns(const til::CoordType columnBegin, const til::CoordType columnEnd);

    std::span<const RGBQUAD> Pixels() const noexcept;
    std::span<const RGBQUAD> Pixels(RenderPosition position) const noexcept;
    bool HasPixels(RenderPosition position) const noexcept;
    size_t KittyWriteMemoryUpperBound(til::CoordType columnBegin, til::CoordType columnEnd, int32_t zIndex, uint32_t imageId) const noexcept;
    const RGBQUAD* Pixels(const til::CoordType columnBegin) const noexcept;
    RGBQUAD* MutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd);
    RGBQUAD* MutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd, int32_t zIndex, uint32_t imageId);

    static void CopyBlock(const TextBuffer& srcBuffer, const til::rect srcRect, TextBuffer& dstBuffer, const til::rect dstRect);
    static void CopyRow(const ROW& srcRow, ROW& dstRow);
    static void CopyCells(const ROW& srcRow, const til::CoordType srcColumn, ROW& dstRow, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd);
    static void EraseBlock(TextBuffer& buffer, const til::rect rect);
    static void EraseCells(TextBuffer& buffer, const til::point at, const til::CoordType distance);
    static void EraseCells(ROW& row, const til::CoordType columnBegin, const til::CoordType columnEnd);

private:
    struct KittyLayer
    {
        int32_t zIndex = 0;
        uint32_t imageId = 0;
        std::vector<RGBQUAD> pixels;
        std::vector<uint8_t> columns;
    };

    struct Composite
    {
        uint64_t revision = 0;
        std::vector<RGBQUAD> pixels;
        bool hasPixels = false;
    };

    void _ensureRange(til::CoordType columnBegin, til::CoordType columnEnd);
    KittyLayer& _getKittyLayer(int32_t zIndex, uint32_t imageId);
    const Composite& _composite(RenderPosition position) const noexcept;
    static size_t _layerStorageBytes(const KittyLayer& layer) noexcept;
    void _reserveKittyBytes(size_t bytes) const;
    void _releaseKittyBytes(size_t bytes) const noexcept;
    void _clearCompositeCaches() const noexcept;
    void _clearLayerColumns(KittyLayer& layer, til::CoordType columnBegin, til::CoordType columnEnd) noexcept;
    void _removeEmptyKittyLayers();
    bool _hasContent() const noexcept;
    bool _copyCells(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd);
    bool _copyCellsInPlace(const ImageSlice& srcSlice, til::CoordType srcColumn, til::CoordType dstColumnBegin, til::CoordType dstColumnEnd);
    bool _eraseCells(const til::CoordType columnBegin, const til::CoordType columnEnd);
    void _swap(ImageSlice& other) noexcept;

    uint64_t _revision = 0;
    til::size _cellSize;
    std::vector<RGBQUAD> _pixelBuffer;
    std::vector<uint32_t> _columnOwners;
    til::CoordType _columnBegin = 0;
    til::CoordType _columnEnd = 0;
    til::CoordType _pixelWidth = 0;
    std::vector<KittyLayer> _kittyLayers;
    mutable std::array<Composite, 3> _composites;
    mutable size_t _accountedKittyBytes = 0;
};
