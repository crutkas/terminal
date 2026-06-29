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
#include <span>
#include <vector>

class ROW;
class TextBuffer;

class ImageSlice
{
public:
    using Pointer = std::unique_ptr<ImageSlice>;

    ImageSlice(const ImageSlice& rhs) = default;
    ImageSlice(const til::size cellSize) noexcept;

    void BumpRevision() noexcept;
    uint64_t Revision() const noexcept;

    til::size CellSize() const noexcept;
    til::CoordType ColumnOffset() const noexcept;
    til::CoordType PixelWidth() const noexcept;

    // Per-column owner tags (0 = none/Sixel). Lets a protocol (e.g. Kitty) target
    // only the cells it owns for deletion without disturbing other images that
    // share the same row. Owners are kept aligned to [_columnBegin, _columnEnd).
    uint32_t ColumnOwner(const til::CoordType column) const noexcept;
    void SetColumnOwner(const til::CoordType columnBegin, const til::CoordType columnEnd, const uint32_t id);
    bool HasOwner(const uint32_t id) const noexcept;
    bool EraseByOwner(const uint32_t id);

    std::span<const RGBQUAD> Pixels() const noexcept;
    const RGBQUAD* Pixels(const til::CoordType columnBegin) const noexcept;
    RGBQUAD* MutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd);

    static void CopyBlock(const TextBuffer& srcBuffer, const til::rect srcRect, TextBuffer& dstBuffer, const til::rect dstRect);
    static void CopyRow(const ROW& srcRow, ROW& dstRow);
    static void CopyCells(const ROW& srcRow, const til::CoordType srcColumn, ROW& dstRow, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd);
    static void EraseBlock(TextBuffer& buffer, const til::rect rect);
    static void EraseCells(TextBuffer& buffer, const til::point at, const til::CoordType distance);
    static void EraseCells(ROW& row, const til::CoordType columnBegin, const til::CoordType columnEnd);

private:
    bool _copyCells(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd);
    bool _eraseCells(const til::CoordType columnBegin, const til::CoordType columnEnd);

    uint64_t _revision = 0;
    til::size _cellSize;
    std::vector<RGBQUAD> _pixelBuffer;
    std::vector<uint32_t> _columnOwners;
    til::CoordType _columnBegin = 0;
    til::CoordType _columnEnd = 0;
    til::CoordType _pixelWidth = 0;
};
