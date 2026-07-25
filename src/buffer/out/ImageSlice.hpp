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

    // Where content composites relative to the text and the cell background.
    // Content written without an identity always composites as AboveText,
    // which is what image content in a row has always done: on main the
    // renderer paints a row's images after its text.
    enum class RenderPosition : uint8_t
    {
        BehindBackground, // only visible where the cell background is default
        BehindText,
        AboveText,
    };
    static constexpr size_t RenderPositionCount = 3;

    // Identifies one layer. A layer is a single image (imageId) placed once
    // (placementId); placing the same image twice yields two layers that can be
    // erased independently. An all-zero key denotes the untagged base plane,
    // which holds content that carries no identity at all.
    struct LayerKey
    {
        uint32_t imageId = 0;
        uint64_t placementId = 0;

        constexpr bool operator==(const LayerKey&) const noexcept = default;
        constexpr bool Untagged() const noexcept { return imageId == 0 && placementId == 0; }
    };

    // Layers below this z composite behind the cell background rather than
    // merely behind the text.
    static constexpr int32_t BackgroundZThreshold = INT32_MIN / 2;
    // A process-wide ceiling on identified layer storage, plus a per-slice
    // layer count limit, so a stream of images cannot exhaust memory.
    static constexpr size_t MaxLayerBytes = 320ull * 1024ull * 1024ull;
    static constexpr size_t MaxLayersPerSlice = 4096;
    static size_t LayerBytesAvailable() noexcept;

    // The process-wide layer budget has to be kept in step with each slice's
    // lifetime, and that bookkeeping was the only thing stopping ImageSlice from
    // using compiler-generated special members. Holding the charge in a member
    // that knows how to copy, move and release itself hands them back.
    ImageSlice(const til::size cellSize) noexcept;
    ImageSlice(const ImageSlice& rhs);
    ImageSlice& operator=(const ImageSlice&) = delete;
    ImageSlice(ImageSlice&&) = default;
    ImageSlice& operator=(ImageSlice&&) = default;
    ~ImageSlice() = default;

    void BumpRevision() noexcept;
    uint64_t Revision() const noexcept;
    // A revision unique to one render position of this slice. Renderers cache
    // uploaded pixels by revision, so each position needs its own value.
    uint64_t Revision(const RenderPosition position) const noexcept;

    til::size CellSize() const noexcept;
    til::CoordType ColumnOffset() const noexcept;
    til::CoordType PixelWidth() const noexcept;

    std::span<const RGBQUAD> Pixels() const noexcept;
    const RGBQUAD* Pixels(const til::CoordType columnBegin) const noexcept;
    RGBQUAD* MutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd);

    // Composited output for one render position, built on demand and cached
    // until the slice's revision changes.
    std::span<const RGBQUAD> Pixels(const RenderPosition position) const;
    bool HasPixels(const RenderPosition position) const;

    // Identified-layer access. The untagged base plane is unaffected by these.
    RGBQUAD* MutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd, const LayerKey key, const int32_t zIndex);
    RGBQUAD* TryMutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd, const LayerKey key, const int32_t zIndex);
    bool PlacementCoversColumn(const uint64_t placementId, const til::CoordType column) const noexcept;
    // An upper bound on the bytes a MutablePixels write of this range would add,
    // so a caller can refuse a placement before allocating any of it.
    size_t WriteMemoryUpperBound(const til::CoordType columnBegin, const til::CoordType columnEnd, const LayerKey key, const int32_t zIndex) const noexcept;
    bool Contains(const uint32_t imageId) const noexcept;
    bool Contains(const LayerKey key) const noexcept;
    bool LayerCoversColumn(const LayerKey key, const til::CoordType column) const noexcept;
    uint32_t ColumnOwner(const til::CoordType column) const noexcept;
    bool ContainsPlacement(const uint64_t placementId) const noexcept;
    std::vector<LayerKey> LayersAtColumn(const til::CoordType column) const;
    std::vector<LayerKey> LayersAtZ(const int32_t zIndex) const;
    std::vector<LayerKey> LayersAtZ(const int32_t zIndex, const til::CoordType column) const;
    bool EraseLayer(const uint32_t imageId);
    bool EraseLayer(const LayerKey key);
    bool EraseLayer(const uint32_t imageId, const til::CoordType columnBegin, const til::CoordType columnEnd);
    bool EraseLayer(const uint32_t imageId, const int32_t zIndex);
    // Drops every identified layer covering these columns, leaving the untagged
    // base plane alone. Used when untagged content claims cells an identified
    // layer was covering.
    void ClearLayers(const til::CoordType columnBegin, const til::CoordType columnEnd);

    static void CopyBlock(const TextBuffer& srcBuffer, const til::rect srcRect, TextBuffer& dstBuffer, const til::rect dstRect);
    static void CopyRow(const ROW& srcRow, ROW& dstRow);
    static void CopyCells(const ROW& srcRow, const til::CoordType srcColumn, ROW& dstRow, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd);
    static void CopyLayerCells(const ROW& srcRow, const til::CoordType srcColumn, ROW& dstRow, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd, const LayerKey key);
    // Folds a slice set aside before a destructive copy back into a row, filling
    // only cells the row does not already cover.
    static void MergePreservedCells(Pointer srcSlice, ROW& dstRow);
    static void EraseBlock(TextBuffer& buffer, const til::rect rect);
    static void EraseCells(TextBuffer& buffer, const til::point at, const til::CoordType distance);
    static void EraseCells(ROW& row, const til::CoordType columnBegin, const til::CoordType columnEnd);
    static void EraseLayerCells(ROW& row, const til::CoordType columnBegin, const til::CoordType columnEnd, const LayerKey key);

private:
    // One identified plane of pixels. Layers composite in ascending zIndex
    // order, with ties broken by insertion order.
    struct Layer
    {
        LayerKey key;
        int32_t zIndex = 0;
        std::vector<RGBQUAD> pixels;
        // One flag per column in [_columnBegin, _columnEnd) marking the cells
        // this layer actually covers, so an erase can target only those cells
        // without disturbing another layer that overlaps the same range.
        std::vector<uint8_t> columns;
    };

    // A lazily built composite for one render position, valid for as long as
    // its revision matches the slice's.
    struct Composite
    {
        uint64_t revision = 0;
        std::vector<RGBQUAD> pixels;
        bool hasPixels = false;
    };

    void _ensureRange(const til::CoordType columnBegin, const til::CoordType columnEnd);
    Layer& _getLayer(const LayerKey key, const int32_t zIndex);
    const Composite& _composite(const RenderPosition position) const;
    // This slice's share of the process-wide layer budget, released automatically
    // when the slice goes away. A copy owes what the original owed, because it
    // carries the same layers; a moved-from slice owes nothing.
    class BudgetCharge
    {
    public:
        BudgetCharge() = default;
        ~BudgetCharge() noexcept;
        BudgetCharge(const BudgetCharge& other) noexcept;
        BudgetCharge& operator=(const BudgetCharge&) = delete;
        BudgetCharge(BudgetCharge&& other) noexcept;
        BudgetCharge& operator=(BudgetCharge&& other) noexcept;

        void Reserve(const size_t bytes) noexcept;
        void Release(const size_t bytes) noexcept;
        size_t Bytes() const noexcept;

    private:
        size_t _bytes = 0;
    };

    static size_t _layerBytes(const Layer& layer) noexcept;
    void _invalidateComposites() noexcept;
    void _clearLayerColumns(Layer& layer, const til::CoordType columnBegin, const til::CoordType columnEnd) noexcept;
    bool _removeEmptyLayers();
    bool _hasContent() const noexcept;
    bool _copyCells(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd);
    void _copyLayers(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd);
    bool _copyLayerCells(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd, const LayerKey key);
    bool _eraseLayerCells(const til::CoordType columnBegin, const til::CoordType columnEnd, const LayerKey key);
    void _mergePreservedCells(const ImageSlice& srcSlice);
    bool _baseCellHasPixels(const til::CoordType column) const noexcept;
    bool _eraseCells(const til::CoordType columnBegin, const til::CoordType columnEnd);
    void _eraseBasePlane(const til::CoordType columnBegin, const til::CoordType columnEnd) noexcept;

    uint64_t _revision = 0;
    til::size _cellSize;
    std::vector<RGBQUAD> _pixelBuffer;
    til::CoordType _columnBegin = 0;
    til::CoordType _columnEnd = 0;
    til::CoordType _pixelWidth = 0;
    // Only allocated once an identified layer is written, so content without
    // identity costs exactly what it did before layers existed.
    std::vector<Layer> _layers;
    mutable std::array<Composite, 3> _composites;
    BudgetCharge _charge;
};
