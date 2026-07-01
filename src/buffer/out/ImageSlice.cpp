// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "ImageSlice.hpp"
#include "Row.hpp"
#include "textBuffer.hpp"

static std::atomic<uint64_t> s_revision{ 0 };

ImageSlice::ImageSlice(const til::size cellSize) noexcept :
    _cellSize{ cellSize }
{
}

void ImageSlice::BumpRevision() noexcept
{
    // Avoid setting the revision to 0. This allows the renderer to use 0 as a sentinel value.
    do
    {
        _revision = s_revision.fetch_add(1, std::memory_order_relaxed);
    } while (_revision == 0);
}

uint64_t ImageSlice::Revision() const noexcept
{
    return _revision;
}

til::size ImageSlice::CellSize() const noexcept
{
    return _cellSize;
}

uint32_t ImageSlice::ColumnOwner(const til::CoordType column) const noexcept
{
    if (column < _columnBegin || column >= _columnEnd)
    {
        return 0;
    }
    const auto index = static_cast<size_t>(column - _columnBegin);
    return index < _columnOwners.size() ? til::at(_columnOwners, index) : 0;
}

void ImageSlice::SetColumnOwner(const til::CoordType columnBegin, const til::CoordType columnEnd, const uint32_t id)
{
    const auto setBegin = std::max(columnBegin, _columnBegin);
    const auto setEnd = std::min(columnEnd, _columnEnd);
    for (auto column = setBegin; column < setEnd; ++column)
    {
        const auto index = static_cast<size_t>(column - _columnBegin);
        if (index < _columnOwners.size())
        {
            til::at(_columnOwners, index) = id;
        }
    }
}

bool ImageSlice::HasOwner(const uint32_t id) const noexcept
{
    return std::find(_columnOwners.begin(), _columnOwners.end(), id) != _columnOwners.end();
}

// Clears pixels and ownership for any column in [columnBegin,columnEnd) owned by
// another image (nonzero id), so content claiming the cells leaves no ownerless
// pixels behind. Untagged columns (owner 0, e.g. existing Sixel) are left alone.
void ImageSlice::ClearForeignColumns(const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    const auto clearBegin = std::max(columnBegin, _columnBegin);
    const auto clearEnd = std::min(columnEnd, _columnEnd);
    for (auto column = clearBegin; column < clearEnd; ++column)
    {
        const auto index = static_cast<size_t>(column - _columnBegin);
        if (index >= _columnOwners.size() || til::at(_columnOwners, index) == 0)
        {
            continue;
        }
        til::at(_columnOwners, index) = 0;
        auto iterator = std::next(_pixelBuffer.data(), static_cast<til::CoordType>(index) * _cellSize.width);
        for (auto y = 0; y < _cellSize.height; y++)
        {
            std::memset(iterator, 0, _cellSize.width * sizeof(RGBQUAD));
            std::advance(iterator, _pixelWidth);
        }
    }
}

// Clears the pixels and owner tag for every column owned by id. Returns true if
// nothing drawable remains (no owned columns and no opaque pixels), so the caller
// can drop the slice. Co-resident content (e.g. Sixel, owner 0) keeps the slice.
bool ImageSlice::EraseByOwner(const uint32_t id)
{
    return EraseByOwner(id, _columnBegin, _columnEnd);
}

// Column-bounded variant of EraseByOwner: clears the pixels and owner tag only for
// columns in [columnBegin, columnEnd) that are owned by id, leaving columns outside
// that range (and columns owned by a different image) untouched. This lets a single
// Kitty placement be erased without clobbering a co-resident image that overlaps the
// same row but different columns. Returns true if nothing drawable remains in the
// whole slice (no owned columns and no opaque untagged pixels), so the caller can
// drop the slice.
bool ImageSlice::EraseByOwner(const uint32_t id, const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    if (id == 0 || _pixelBuffer.empty())
    {
        return false;
    }
    const auto eraseBegin = std::max(_columnBegin, columnBegin);
    const auto eraseEnd = std::min(_columnEnd, columnEnd);
    for (auto column = eraseBegin; column < eraseEnd; ++column)
    {
        const auto index = static_cast<size_t>(column - _columnBegin);
        if (index >= _columnOwners.size() || til::at(_columnOwners, index) != id)
        {
            continue;
        }
        til::at(_columnOwners, index) = 0;
        const auto eraseOffset = static_cast<til::CoordType>(index) * _cellSize.width;
        auto eraseIterator = std::next(_pixelBuffer.data(), eraseOffset);
        for (auto y = 0; y < _cellSize.height; y++)
        {
            std::memset(eraseIterator, 0, _cellSize.width * sizeof(RGBQUAD));
            std::advance(eraseIterator, _pixelWidth);
        }
    }
    return !_hasContent();
}

til::CoordType ImageSlice::ColumnOffset() const noexcept
{
    return _columnBegin;
}

til::CoordType ImageSlice::PixelWidth() const noexcept
{
    return _pixelWidth;
}

std::span<const RGBQUAD> ImageSlice::Pixels() const noexcept
{
    return _pixelBuffer;
}

const RGBQUAD* ImageSlice::Pixels(const til::CoordType columnBegin) const noexcept
{
    const auto pixelOffset = (columnBegin - _columnBegin) * _cellSize.width;
    return &til::at(_pixelBuffer, pixelOffset);
}

RGBQUAD* ImageSlice::MutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    // IF the buffer is empty or isn't large enough for the requested range, we'll need to resize it.
    if (_pixelBuffer.empty() || columnBegin < _columnBegin || columnEnd > _columnEnd)
    {
        const auto oldColumnBegin = _columnBegin;
        const auto oldPixelWidth = _pixelWidth;
        const auto existingData = !_pixelBuffer.empty();
        _columnBegin = existingData ? std::min(_columnBegin, columnBegin) : columnBegin;
        _columnEnd = existingData ? std::max(_columnEnd, columnEnd) : columnEnd;
        _pixelWidth = (_columnEnd - _columnBegin) * _cellSize.width;
        const auto bufferSize = _pixelWidth * _cellSize.height;
        const auto columnCount = _columnEnd - _columnBegin;
        if (existingData)
        {
            // If there is existing data in the buffer, we need to copy it
            // across to the appropriate position in the new buffer.
            auto newPixelBuffer = std::vector<RGBQUAD>(bufferSize);
            const auto newPixelOffset = (oldColumnBegin - _columnBegin) * _cellSize.width;
            auto newIterator = std::next(newPixelBuffer.data(), newPixelOffset);
            auto oldIterator = _pixelBuffer.data();
            // Because widths are rounded up to multiples of 4, it's possible
            // that the old width will extend past the right border of the new
            // buffer, so the range that we copy must be clamped to fit.
            const auto newPixelRange = std::min(oldPixelWidth, _pixelWidth - newPixelOffset);
            for (auto i = 0; i < _cellSize.height; i++)
            {
                std::memcpy(newIterator, oldIterator, newPixelRange * sizeof(RGBQUAD));
                std::advance(oldIterator, oldPixelWidth);
                std::advance(newIterator, _pixelWidth);
            }
            _pixelBuffer = std::move(newPixelBuffer);
            // Keep the per-column owners aligned to the new column range.
            auto newColumnOwners = std::vector<uint32_t>(columnCount);
            const auto ownerOffset = oldColumnBegin - _columnBegin;
            const auto ownerRange = std::min<til::CoordType>(static_cast<til::CoordType>(_columnOwners.size()), columnCount - ownerOffset);
            for (auto i = 0; i < ownerRange; i++)
            {
                til::at(newColumnOwners, ownerOffset + i) = til::at(_columnOwners, i);
            }
            _columnOwners = std::move(newColumnOwners);
        }
        else
        {
            // Otherwise we just initialize the buffer to the correct size.
            _pixelBuffer.resize(bufferSize);
            _columnOwners.assign(columnCount, 0);
        }
    }
    const auto pixelOffset = (columnBegin - _columnBegin) * _cellSize.width;
    return &til::at(_pixelBuffer, pixelOffset);
}

void ImageSlice::CopyBlock(const TextBuffer& srcBuffer, const til::rect srcRect, TextBuffer& dstBuffer, const til::rect dstRect)
{
    // If the top of the source is less than the top of the destination, we copy
    // the rows from the bottom upwards, to avoid the possibility of the source
    // being overwritten if it were to overlap the destination range.
    if (srcRect.top < dstRect.top)
    {
        for (auto y = srcRect.height(); y-- > 0;)
        {
            const auto& srcRow = srcBuffer.GetRowByOffset(srcRect.top + y);
            auto& dstRow = dstBuffer.GetMutableRowByOffset(dstRect.top + y);
            CopyCells(srcRow, srcRect.left, dstRow, dstRect.left, dstRect.right);
        }
    }
    else
    {
        for (auto y = 0; y < srcRect.height(); y++)
        {
            const auto& srcRow = srcBuffer.GetRowByOffset(srcRect.top + y);
            auto& dstRow = dstBuffer.GetMutableRowByOffset(dstRect.top + y);
            CopyCells(srcRow, srcRect.left, dstRow, dstRect.left, dstRect.right);
        }
    }
}

void ImageSlice::CopyRow(const ROW& srcRow, ROW& dstRow)
{
    const auto srcSlice = srcRow.GetImageSlice();
    dstRow.SetImageSlice(srcSlice ? std::make_unique<ImageSlice>(*srcSlice) : nullptr);
}

void ImageSlice::CopyCells(const ROW& srcRow, const til::CoordType srcColumn, ROW& dstRow, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd)
{
    // If there's no image content in the source row, we're essentially copying
    // a blank image into the destination, which is the same thing as an erase.
    // Also if the line renditions are different, there's no meaningful way to
    // copy the image content, so we also just treat that as an erase.
    const auto srcSlice = srcRow.GetImageSlice();
    if (!srcSlice || srcRow.GetLineRendition() != dstRow.GetLineRendition()) [[likely]]
    {
        ImageSlice::EraseCells(dstRow, dstColumnBegin, dstColumnEnd);
    }
    else
    {
        auto dstSlice = dstRow.GetMutableImageSlice();
        if (!dstSlice)
        {
            dstSlice = dstRow.SetImageSlice(std::make_unique<ImageSlice>(srcSlice->CellSize()));
            __assume(dstSlice != nullptr);
        }
        const auto scale = srcRow.GetLineRendition() != LineRendition::SingleWidth ? 1 : 0;
        if (dstSlice->_copyCells(*srcSlice, srcColumn << scale, dstColumnBegin << scale, dstColumnEnd << scale))
        {
            // If _copyCells returns true, that means the destination was
            // completely erased, so we can delete this slice.
            dstRow.SetImageSlice(nullptr);
        }
    }
}

void ImageSlice::CopyKittyCells(const ROW& srcRow, const til::CoordType srcColumn, ROW& dstRow, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd)
{
    const auto srcSlice = srcRow.GetImageSlice();
    if (!srcSlice || srcRow.GetLineRendition() != dstRow.GetLineRendition()) [[unlikely]]
    {
        EraseKittyCells(dstRow, dstColumnBegin, dstColumnEnd);
        return;
    }

    auto dstSlice = dstRow.GetMutableImageSlice();
    if (dstSlice && dstSlice->CellSize() != srcSlice->CellSize()) [[unlikely]]
    {
        EraseKittyCells(dstRow, dstColumnBegin, dstColumnEnd);
        dstSlice = dstRow.GetMutableImageSlice();
        if (dstSlice)
        {
            return;
        }
    }
    if (!dstSlice)
    {
        dstSlice = dstRow.SetImageSlice(std::make_unique<ImageSlice>(srcSlice->CellSize()));
        __assume(dstSlice != nullptr);
    }

    const auto scale = srcRow.GetLineRendition() != LineRendition::SingleWidth ? 1 : 0;
    if (dstSlice->_copyKittyCells(*srcSlice, srcColumn << scale, dstColumnBegin << scale, dstColumnEnd << scale))
    {
        dstRow.SetImageSlice(nullptr);
    }
}

bool ImageSlice::_copyCells(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd)
{
    const auto srcColumnEnd = srcColumn + dstColumnEnd - dstColumnBegin;

    // First we determine the portions of the copy range that are currently in use.
    const auto srcUsedBegin = std::max(srcColumn, srcSlice._columnBegin);
    const auto srcUsedEnd = std::max(std::min(srcColumnEnd, srcSlice._columnEnd), srcUsedBegin);
    const auto dstUsedBegin = std::max(dstColumnBegin, _columnBegin);
    const auto dstUsedEnd = std::max(std::min(dstColumnEnd, _columnEnd), dstUsedBegin);

    // The used source projected into the destination is the range we must overwrite.
    const auto projectedOffset = dstColumnBegin - srcColumn;
    const auto dstWriteBegin = srcUsedBegin + projectedOffset;
    const auto dstWriteEnd = srcUsedEnd + projectedOffset;

    if (dstWriteBegin < dstWriteEnd)
    {
        auto dstIterator = MutablePixels(dstWriteBegin, dstWriteEnd);
        auto srcIterator = srcSlice.Pixels(srcUsedBegin);
        const auto writeCellCount = dstWriteEnd - dstWriteBegin;
        const auto writeByteCount = sizeof(RGBQUAD) * writeCellCount * _cellSize.width;
        for (auto y = 0; y < _cellSize.height; y++)
        {
            std::memmove(dstIterator, srcIterator, writeByteCount);
            std::advance(srcIterator, srcSlice._pixelWidth);
            std::advance(dstIterator, _pixelWidth);
        }
        // Carry over per-column ownership so a moved/reflowed image keeps its tag.
        // Snapshot first because an in-place rightward move overlaps src and dst.
        std::vector<uint32_t> movedOwners(writeCellCount);
        for (auto i = 0; i < writeCellCount; i++)
        {
            til::at(movedOwners, i) = srcSlice.ColumnOwner(srcUsedBegin + i);
        }
        for (auto i = 0; i < writeCellCount; i++)
        {
            const auto dstCol = dstWriteBegin + i;
            SetColumnOwner(dstCol, dstCol + 1, til::at(movedOwners, i));
        }
    }

    // The used destination before and after the written area must be erased.
    // If this results in the entire range being erased, we return true to let
    // the caller know that the slice should be deleted.
    if (dstUsedBegin < dstWriteBegin && _eraseCells(dstUsedBegin, dstWriteBegin))
    {
        return true;
    }
    if (dstUsedEnd > dstWriteEnd && _eraseCells(dstWriteEnd, dstUsedEnd))
    {
        return true;
    }

    // At this point, if the beginning column is not less than the end, that
    // means this was an empty slice into which nothing was copied, so we can
    // again return true to let the caller know it should be deleted.
    return _columnBegin >= _columnEnd;
}

bool ImageSlice::_copyKittyCells(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd)
{
    if (_cellSize != srcSlice._cellSize)
    {
        return _eraseKittyCells(dstColumnBegin, dstColumnEnd);
    }

    const auto cellCount = dstColumnEnd - dstColumnBegin;
    const auto pixelsPerCell = static_cast<size_t>(_cellSize.width) * _cellSize.height;
    std::vector<uint32_t> owners(cellCount);
    std::vector<RGBQUAD> pixels(static_cast<size_t>(cellCount) * pixelsPerCell);

    for (auto i = 0; i < cellCount; ++i)
    {
        const auto sourceColumn = srcColumn + i;
        const auto owner = srcSlice.ColumnOwner(sourceColumn);
        til::at(owners, i) = owner;
        if (owner == 0)
        {
            continue;
        }

        auto source = srcSlice.Pixels(sourceColumn);
        auto destination = pixels.data() + static_cast<size_t>(i) * pixelsPerCell;
        const auto rowByteCount = static_cast<size_t>(_cellSize.width) * sizeof(RGBQUAD);
        for (auto y = 0; y < _cellSize.height; ++y)
        {
            std::memcpy(destination, source, rowByteCount);
            std::advance(source, srcSlice._pixelWidth);
            std::advance(destination, _cellSize.width);
        }
    }

    _eraseKittyCells(dstColumnBegin, dstColumnEnd);
    for (auto i = 0; i < cellCount; ++i)
    {
        const auto owner = til::at(owners, i);
        if (owner == 0)
        {
            continue;
        }

        const auto destinationColumn = dstColumnBegin + i;
        auto destination = MutablePixels(destinationColumn, destinationColumn + 1);
        auto source = pixels.data() + static_cast<size_t>(i) * pixelsPerCell;
        const auto rowByteCount = static_cast<size_t>(_cellSize.width) * sizeof(RGBQUAD);
        for (auto y = 0; y < _cellSize.height; ++y)
        {
            std::memcpy(destination, source, rowByteCount);
            std::advance(source, _cellSize.width);
            std::advance(destination, _pixelWidth);
        }
        SetColumnOwner(destinationColumn, destinationColumn + 1, owner);
    }

    return !_hasContent();
}

void ImageSlice::MergeLegacyCells(const ImageSlice& srcSlice, ROW& dstRow)
{
    auto dstSlice = dstRow.GetMutableImageSlice();
    if (dstSlice && dstSlice->CellSize() != srcSlice.CellSize()) [[unlikely]]
    {
        return;
    }
    if (!dstSlice)
    {
        dstSlice = dstRow.SetImageSlice(std::make_unique<ImageSlice>(srcSlice.CellSize()));
        __assume(dstSlice != nullptr);
    }
    dstSlice->_mergeLegacyCells(srcSlice);
    if (!dstSlice->_hasContent())
    {
        dstRow.SetImageSlice(nullptr);
    }
}

void ImageSlice::_mergeLegacyCells(const ImageSlice& srcSlice)
{
    for (auto column = srcSlice._columnBegin; column < srcSlice._columnEnd; ++column)
    {
        if (srcSlice.ColumnOwner(column) != 0 ||
            !srcSlice._cellHasPixels(column) ||
            ColumnOwner(column) != 0 ||
            _cellHasPixels(column))
        {
            continue;
        }

        auto source = srcSlice.Pixels(column);
        auto destination = MutablePixels(column, column + 1);
        const auto rowByteCount = static_cast<size_t>(_cellSize.width) * sizeof(RGBQUAD);
        for (auto y = 0; y < _cellSize.height; ++y)
        {
            std::memcpy(destination, source, rowByteCount);
            std::advance(source, srcSlice._pixelWidth);
            std::advance(destination, _pixelWidth);
        }
    }
}

void ImageSlice::EraseBlock(TextBuffer& buffer, const til::rect rect)
{
    for (auto y = rect.top; y < rect.bottom; y++)
    {
        auto& row = buffer.GetMutableRowByOffset(y);
        EraseCells(row, rect.left, rect.right);
    }
}

void ImageSlice::EraseCells(TextBuffer& buffer, const til::point at, const til::CoordType distance)
{
    auto x = at.x;
    auto y = at.y;
    auto distanceRemaining = distance;
    while (distanceRemaining > 0)
    {
        auto& row = buffer.GetMutableRowByOffset(y);
        EraseCells(row, x, x + distanceRemaining);
        distanceRemaining -= (static_cast<til::CoordType>(row.size()) - x);
        x = 0;
        y++;
    }
}

void ImageSlice::EraseCells(ROW& row, const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    const auto imageSlice = row.GetMutableImageSlice();
    if (imageSlice) [[unlikely]]
    {
        const auto scale = row.GetLineRendition() != LineRendition::SingleWidth ? 1 : 0;
        if (imageSlice->_eraseCells(columnBegin << scale, columnEnd << scale))
        {
            // If _eraseCells returns true, that means the image was
            // completely erased, so we can delete this slice.
            row.SetImageSlice(nullptr);
        }
    }
}

void ImageSlice::EraseKittyCells(ROW& row, const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    const auto imageSlice = row.GetMutableImageSlice();
    if (imageSlice) [[unlikely]]
    {
        const auto scale = row.GetLineRendition() != LineRendition::SingleWidth ? 1 : 0;
        if (imageSlice->_eraseKittyCells(columnBegin << scale, columnEnd << scale))
        {
            row.SetImageSlice(nullptr);
        }
    }
}

bool ImageSlice::_eraseCells(const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    if (columnBegin <= _columnBegin && columnEnd >= _columnEnd)
    {
        // If we're erasing the entire range that's in use, we return true to
        // indicate that there is now nothing left. We don't bother altering
        // the buffer because the caller is now expected to delete this slice.
        return true;
    }
    else
    {
        const auto eraseBegin = std::max(columnBegin, _columnBegin);
        const auto eraseEnd = std::min(columnEnd, _columnEnd);
        if (eraseBegin < eraseEnd)
        {
            const auto eraseOffset = (eraseBegin - _columnBegin) * _cellSize.width;
            const auto eraseLength = (eraseEnd - eraseBegin) * _cellSize.width;
            auto eraseIterator = std::next(_pixelBuffer.data(), eraseOffset);
            for (auto y = 0; y < _cellSize.height; y++)
            {
                std::memset(eraseIterator, 0, eraseLength * sizeof(RGBQUAD));
                std::advance(eraseIterator, _pixelWidth);
            }
            // Clear ownership of the erased columns so it doesn't outlive the pixels.
            SetColumnOwner(eraseBegin, eraseEnd, 0);
        }
        return false;
    }
}

bool ImageSlice::_eraseKittyCells(const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    const auto eraseBegin = std::max(columnBegin, _columnBegin);
    const auto eraseEnd = std::min(columnEnd, _columnEnd);
    for (auto column = eraseBegin; column < eraseEnd; ++column)
    {
        if (ColumnOwner(column) == 0)
        {
            continue;
        }

        const auto index = static_cast<size_t>(column - _columnBegin);
        til::at(_columnOwners, index) = 0;
        auto iterator = std::next(_pixelBuffer.data(), gsl::narrow_cast<til::CoordType>(index) * _cellSize.width);
        for (auto y = 0; y < _cellSize.height; ++y)
        {
            std::memset(iterator, 0, _cellSize.width * sizeof(RGBQUAD));
            std::advance(iterator, _pixelWidth);
        }
    }
    return !_hasContent();
}

bool ImageSlice::_cellHasPixels(const til::CoordType column) const noexcept
{
    if (column < _columnBegin || column >= _columnEnd)
    {
        return false;
    }

    auto pixel = Pixels(column);
    for (auto y = 0; y < _cellSize.height; ++y)
    {
        for (auto x = 0; x < _cellSize.width; ++x)
        {
            if (pixel[x].rgbReserved != 0 || pixel[x].rgbRed != 0 || pixel[x].rgbGreen != 0 || pixel[x].rgbBlue != 0)
            {
                return true;
            }
        }
        std::advance(pixel, _pixelWidth);
    }
    return false;
}

bool ImageSlice::_hasContent() const noexcept
{
    return std::ranges::any_of(_columnOwners, [](const auto owner) { return owner != 0; }) ||
           std::ranges::any_of(_pixelBuffer, [](const auto& pixel) {
               return pixel.rgbReserved != 0 || pixel.rgbRed != 0 || pixel.rgbGreen != 0 || pixel.rgbBlue != 0;
           });
}
