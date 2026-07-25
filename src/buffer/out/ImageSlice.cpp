// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "ImageSlice.hpp"
#include "Row.hpp"
#include "textBuffer.hpp"

#include <algorithm>

static std::atomic<uint64_t> s_revision{ 0 };

// A process-wide accounting of identified layer storage. Untagged content is
// not accounted, because its footprint is already bounded by the dimensions of
// the text buffer that owns it.
static std::atomic<size_t> s_layerBytes{ 0 };

namespace
{
    // Maps a layer's z-index onto the plane it composites into.
    constexpr ImageSlice::RenderPosition PositionOf(const int32_t zIndex) noexcept
    {
        if (zIndex < ImageSlice::BackgroundZThreshold)
        {
            return ImageSlice::RenderPosition::BehindBackground;
        }
        return zIndex < 0 ? ImageSlice::RenderPosition::BehindText : ImageSlice::RenderPosition::AboveText;
    }

    // Moves a plane into a wider plane, aligned to the new origin. Cells that
    // the old plane did not cover are left at `fill`.
    template<typename T>
    void RelocatePlane(std::vector<T>& plane, const til::CoordType oldPixelWidth, const til::CoordType newPixelWidth, const til::CoordType newPixelOffset, const til::CoordType cellHeight, const T fill = T{})
    {
        if (plane.empty())
        {
            return;
        }
        auto relocated = std::vector<T>(gsl::narrow_cast<size_t>(newPixelWidth) * cellHeight, fill);
        auto srcIterator = plane.data();
        auto dstIterator = std::next(relocated.data(), newPixelOffset);
        // Because widths are rounded up to multiples of 4, it's possible that
        // the old width will extend past the right border of the new plane, so
        // the range that we copy must be clamped to fit.
        const auto range = std::min(oldPixelWidth, newPixelWidth - newPixelOffset);
        for (auto y = 0; y < cellHeight; y++)
        {
            std::memcpy(dstIterator, srcIterator, gsl::narrow_cast<size_t>(range) * sizeof(T));
            std::advance(srcIterator, oldPixelWidth);
            std::advance(dstIterator, newPixelWidth);
        }
        plane = std::move(relocated);
    }

    // Moves per-column coverage flags into a wider range, aligned to the new origin.
    void RelocateColumns(std::vector<uint8_t>& columns, const size_t newCount, const size_t newOffset)
    {
        if (columns.empty())
        {
            return;
        }
        auto relocated = std::vector<uint8_t>(newCount, 0);
        const auto count = std::min(columns.size(), newCount - newOffset);
        std::copy_n(columns.begin(), count, std::next(relocated.begin(), newOffset));
        columns = std::move(relocated);
    }
}

size_t ImageSlice::LayerBytesAvailable() noexcept
{
    const auto used = s_layerBytes.load(std::memory_order_relaxed);
    return used >= MaxLayerBytes ? 0 : MaxLayerBytes - used;
}

ImageSlice::BudgetCharge::~BudgetCharge() noexcept
{
    Release(_bytes);
}

ImageSlice::BudgetCharge::BudgetCharge(const BudgetCharge& other) noexcept
{
    // The copying slice carries the same layers, so it owes the same amount.
    Reserve(other._bytes);
}

ImageSlice::BudgetCharge::BudgetCharge(BudgetCharge&& other) noexcept :
    _bytes{ std::exchange(other._bytes, 0) }
{
}

ImageSlice::BudgetCharge& ImageSlice::BudgetCharge::operator=(BudgetCharge&& other) noexcept
{
    if (this != &other)
    {
        Release(_bytes);
        _bytes = std::exchange(other._bytes, 0);
    }
    return *this;
}

void ImageSlice::BudgetCharge::Reserve(const size_t bytes) noexcept
{
    if (bytes != 0)
    {
        _bytes += bytes;
        s_layerBytes.fetch_add(bytes, std::memory_order_relaxed);
    }
}

void ImageSlice::BudgetCharge::Release(const size_t bytes) noexcept
{
    if (bytes != 0)
    {
        _bytes -= bytes;
        s_layerBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
}

size_t ImageSlice::BudgetCharge::Bytes() const noexcept
{
    return _bytes;
}

// A copy is made whenever a row is cloned, which happens during scrolling and
// reflow. That must not be allowed to fail, so the budget is charged for the
// copy but is deliberately not enforced here. This is the one special member
// that cannot be defaulted: the composite caches are per-slice scratch and are
// deliberately not carried across, so the copy starts with them empty.
// Everything but `_composites`, which is a scratch cache of flattened planes.
// A copy starts with them empty rather than inheriting the original's, so this
// one member is why the copy constructor is written out rather than defaulted.
ImageSlice::ImageSlice(const ImageSlice& rhs) :
    _revision{ rhs._revision },
    _cellSize{ rhs._cellSize },
    _pixelBuffer{ rhs._pixelBuffer },
    _columnBegin{ rhs._columnBegin },
    _columnEnd{ rhs._columnEnd },
    _pixelWidth{ rhs._pixelWidth },
    _layers{ rhs._layers },
    _charge{ rhs._charge }
{
}

ImageSlice::ImageSlice(const til::size cellSize) noexcept :
    _cellSize{ cellSize }
{
}

void ImageSlice::BumpRevision() noexcept
{
    // Reserve one value per render position, so each of a slice's composited
    // planes has a revision no other slice can also hand out. Renderers cache
    // uploaded pixels by revision, and would otherwise draw one plane's pixels
    // when asked for another's.
    do
    {
        _revision = s_revision.fetch_add(RenderPositionCount, std::memory_order_relaxed);
    } while (_revision == 0);
}

uint64_t ImageSlice::Revision() const noexcept
{
    return _revision;
}

uint64_t ImageSlice::Revision(const RenderPosition position) const noexcept
{
    return _revision == 0 ? 0 : _revision + static_cast<uint64_t>(position);
}

til::size ImageSlice::CellSize() const noexcept
{
    return _cellSize;
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

// Widens the slice to cover the requested range, relocating the base plane and
// every layer so they stay aligned with the new origin.
void ImageSlice::_ensureRange(const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    const auto hasRange = _columnEnd > _columnBegin;
    if (hasRange && columnBegin >= _columnBegin && columnEnd <= _columnEnd)
    {
        return;
    }

    const auto oldColumnBegin = _columnBegin;
    const auto oldPixelWidth = _pixelWidth;
    _columnBegin = hasRange ? std::min(_columnBegin, columnBegin) : columnBegin;
    _columnEnd = hasRange ? std::max(_columnEnd, columnEnd) : columnEnd;
    _pixelWidth = (_columnEnd - _columnBegin) * _cellSize.width;

    if (!hasRange)
    {
        return;
    }

    const auto newPixelOffset = (oldColumnBegin - _columnBegin) * _cellSize.width;
    const auto newColumnOffset = gsl::narrow_cast<size_t>(oldColumnBegin - _columnBegin);
    const auto newColumnCount = gsl::narrow_cast<size_t>(_columnEnd - _columnBegin);

    RelocatePlane(_pixelBuffer, oldPixelWidth, _pixelWidth, newPixelOffset, _cellSize.height);
    for (auto& layer : _layers)
    {
        const auto before = _layerBytes(layer);
        RelocatePlane(layer.pixels, oldPixelWidth, _pixelWidth, newPixelOffset, _cellSize.height);
        RelocateColumns(layer.columns, newColumnCount, newColumnOffset);
        // This growth is forced by geometry rather than requested by a write,
        // so it is charged to the budget but not refused.
        _charge.Reserve(_layerBytes(layer) - before);
    }
}

RGBQUAD* ImageSlice::MutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    // If the buffer is empty or isn't large enough for the requested range, we'll need to resize it.
    if (_pixelBuffer.empty() || columnBegin < _columnBegin || columnEnd > _columnEnd)
    {
        _ensureRange(columnBegin, columnEnd);
        _pixelBuffer.resize(gsl::narrow_cast<size_t>(_pixelWidth) * _cellSize.height);
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
        // A slice can hold layers without ever holding untagged content, in
        // which case there is no base plane to read from. The copy still has to
        // replace whatever the destination had in that range.
        if (!srcSlice._pixelBuffer.empty())
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
        }
        else
        {
            // No base plane in the source means the destination's untagged
            // pixels in this range are what the copy replaces them with:
            // nothing. (A self-copy can't reach here with anything to erase.)
            _eraseBasePlane(dstWriteBegin, dstWriteEnd);
        }
        _copyLayers(srcSlice, srcUsedBegin, dstWriteBegin, dstWriteEnd);
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
            _eraseBasePlane(eraseBegin, eraseEnd);
            for (auto& layer : _layers)
            {
                _clearLayerColumns(layer, eraseBegin, eraseEnd);
            }
            _removeEmptyLayers();
            _invalidateComposites();
        }
        // Erasing a range can retire the last layer, which leaves the slice
        // with nothing in it at all.
        return !_hasContent();
    }
}

// Zeroes the untagged base plane over a column range, leaving every identified
// layer untouched. `_eraseCells` erases both; a copy only replaces this part.
void ImageSlice::_eraseBasePlane(const til::CoordType columnBegin, const til::CoordType columnEnd) noexcept
{
    if (_pixelBuffer.empty())
    {
        return;
    }

    const auto eraseBegin = std::max(columnBegin, _columnBegin);
    const auto eraseEnd = std::min(columnEnd, _columnEnd);
    if (eraseBegin >= eraseEnd)
    {
        return;
    }

    const auto eraseOffset = (eraseBegin - _columnBegin) * _cellSize.width;
    const auto eraseLength = (eraseEnd - eraseBegin) * _cellSize.width;
    auto eraseIterator = std::next(_pixelBuffer.data(), eraseOffset);
    for (auto y = 0; y < _cellSize.height; y++)
    {
        std::memset(eraseIterator, 0, static_cast<size_t>(eraseLength) * sizeof(RGBQUAD));
        std::advance(eraseIterator, _pixelWidth);
    }
    _invalidateComposites();
}

size_t ImageSlice::_layerBytes(const Layer& layer) noexcept
{
    return layer.pixels.size() * sizeof(RGBQUAD) + layer.columns.size();
}

// A revision of 0 is never handed out by BumpRevision, so it doubles as the
// "this composite is stale" marker.
void ImageSlice::_invalidateComposites() noexcept
{
    for (auto& composite : _composites)
    {
        composite.revision = 0;
    }
}

bool ImageSlice::_hasContent() const noexcept
{
    return !_pixelBuffer.empty() || !_layers.empty();
}

void ImageSlice::_clearLayerColumns(Layer& layer, const til::CoordType columnBegin, const til::CoordType columnEnd) noexcept
{
    const auto clearBegin = std::max(columnBegin, _columnBegin);
    const auto clearEnd = std::min(columnEnd, _columnEnd);
    if (clearBegin >= clearEnd)
    {
        return;
    }

    for (auto column = clearBegin; column < clearEnd; column++)
    {
        const auto index = static_cast<size_t>(column - _columnBegin);
        if (index < layer.columns.size())
        {
            til::at(layer.columns, index) = 0;
        }
    }

    // The pixels have to go too, or the next composite would resurrect them.
    if (!layer.pixels.empty())
    {
        const auto offset = (clearBegin - _columnBegin) * _cellSize.width;
        const auto length = (clearEnd - clearBegin) * _cellSize.width;
        auto iterator = std::next(layer.pixels.data(), offset);
        for (auto y = 0; y < _cellSize.height; y++)
        {
            std::memset(iterator, 0, static_cast<size_t>(length) * sizeof(RGBQUAD));
            std::advance(iterator, _pixelWidth);
        }
    }
}

// Drops any layer that no longer covers a single cell, returning true if the
// set of layers changed.
bool ImageSlice::_removeEmptyLayers()
{
    auto removed = false;
    for (auto it = _layers.begin(); it != _layers.end();)
    {
        const auto covered = std::any_of(it->columns.begin(), it->columns.end(), [](const uint8_t flag) { return flag != 0; });
        if (covered)
        {
            ++it;
        }
        else
        {
            _charge.Release(_layerBytes(*it));
            it = _layers.erase(it);
            removed = true;
        }
    }
    return removed;
}

ImageSlice::Layer& ImageSlice::_getLayer(const LayerKey key, const int32_t zIndex)
{
    const auto existing = std::find_if(_layers.begin(), _layers.end(), [&](const Layer& layer) {
        return layer.key == key && layer.zIndex == zIndex;
    });
    if (existing != _layers.end())
    {
        return *existing;
    }
    _layers.push_back(Layer{ .key = key, .zIndex = zIndex });
    return _layers.back();
}

const ImageSlice::Composite& ImageSlice::_composite(const RenderPosition position) const
{
    auto& composite = til::at(_composites, static_cast<size_t>(position));
    if (composite.revision == _revision && _revision != 0)
    {
        return composite;
    }

    const auto planeSize = static_cast<size_t>(_pixelWidth) * _cellSize.height;
    composite.pixels.assign(planeSize, RGBQUAD{});
    composite.hasPixels = false;

    // Untagged content composites above the text, which is where image content
    // in a row has always been drawn.
    if (position == RenderPosition::AboveText && !_pixelBuffer.empty())
    {
        const auto count = std::min(planeSize, _pixelBuffer.size());
        std::copy_n(_pixelBuffer.begin(), count, composite.pixels.begin());
        composite.hasPixels = true;
    }

    // Layers composite in ascending z. At equal z the higher image identifier
    // wins, so what ends up on top is a property of the content itself and not
    // of the order it happened to arrive in -- which means a row that is copied,
    // scrolled or reflowed always composites the same way.
    auto ordered = std::vector<const Layer*>{};
    ordered.reserve(_layers.size());
    for (const auto& layer : _layers)
    {
        if (PositionOf(layer.zIndex) == position && !layer.pixels.empty())
        {
            ordered.push_back(&layer);
        }
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const Layer* lhs, const Layer* rhs) {
        return std::tie(lhs->zIndex, lhs->key.imageId) < std::tie(rhs->zIndex, rhs->key.imageId);
    });

    for (const auto layer : ordered)
    {
        const auto count = std::min(planeSize, layer->pixels.size());
        for (size_t i = 0; i < count; i++)
        {
            // Source-over on premultiplied pixels: dst = src + dst * (1 - srcAlpha).
            // A fully transparent source leaves what is beneath it untouched, and
            // a fully opaque one replaces it, without either being a special case.
            const auto& src = til::at(layer->pixels, i);
            auto& dst = til::at(composite.pixels, i);
            const auto inverseAlpha = 255u - src.rgbReserved;
            const auto over = [inverseAlpha](const BYTE s, const BYTE d) {
                return static_cast<BYTE>(std::min(255u, static_cast<uint32_t>(s) + (static_cast<uint32_t>(d) * inverseAlpha + 127u) / 255u));
            };
            dst.rgbRed = over(src.rgbRed, dst.rgbRed);
            dst.rgbGreen = over(src.rgbGreen, dst.rgbGreen);
            dst.rgbBlue = over(src.rgbBlue, dst.rgbBlue);
            dst.rgbReserved = over(src.rgbReserved, dst.rgbReserved);
            // Any pixel the compositor produced is content. Whether a colour with
            // no alpha ends up visible is for the engine that draws it to decide;
            // treating it as nothing here would drop the row before it gets asked.
            composite.hasPixels = composite.hasPixels || dst.rgbRed != 0 || dst.rgbGreen != 0 || dst.rgbBlue != 0 || dst.rgbReserved != 0;
        }
    }

    composite.revision = _revision;
    return composite;
}

std::span<const RGBQUAD> ImageSlice::Pixels(const RenderPosition position) const
{
    // Most slices carry no identified layers at all. Hand back the base plane
    // itself rather than compositing a copy of it every time it changes.
    if (_layers.empty())
    {
        return position == RenderPosition::AboveText ? std::span<const RGBQUAD>{ _pixelBuffer } : std::span<const RGBQUAD>{};
    }
    return _composite(position).pixels;
}

bool ImageSlice::HasPixels(const RenderPosition position) const
{
    if (_layers.empty())
    {
        return position == RenderPosition::AboveText && !_pixelBuffer.empty();
    }
    return _composite(position).hasPixels;
}

// Throws std::bad_alloc if the slice cannot take another layer, either because
// this one would exceed the per-slice count or the process-wide byte budget.
// Callers for whom "it did not fit" is an ordinary outcome -- copying a row that
// is being scrolled, say -- want TryMutablePixels instead.
RGBQUAD* ImageSlice::MutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd, const LayerKey key, const int32_t zIndex)
{
    const auto pixels = TryMutablePixels(columnBegin, columnEnd, key, zIndex);
    if (!pixels)
    {
        throw std::bad_alloc{};
    }
    return pixels;
}

RGBQUAD* ImageSlice::TryMutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd, const LayerKey key, const int32_t zIndex)
{
    // Content that carries no identity belongs in the base plane.
    if (key.Untagged())
    {
        return MutablePixels(columnBegin, columnEnd);
    }

    const auto isNewLayer = std::none_of(_layers.begin(), _layers.end(), [&](const Layer& layer) {
        return layer.key == key && layer.zIndex == zIndex;
    });
    if (isNewLayer && _layers.size() >= MaxLayersPerSlice)
    {
        return nullptr;
    }

    _ensureRange(columnBegin, columnEnd);

    const auto planeSize = static_cast<size_t>(_pixelWidth) * _cellSize.height;
    const auto columnCount = static_cast<size_t>(_columnEnd - _columnBegin);
    if (isNewLayer && planeSize * (sizeof(RGBQUAD) + sizeof(uint32_t)) + columnCount > LayerBytesAvailable())
    {
        return nullptr;
    }

    auto& layer = _getLayer(key, zIndex);
    const auto before = _layerBytes(layer);
    layer.pixels.resize(planeSize);
    layer.columns.resize(columnCount, 0);
    for (auto column = columnBegin; column < columnEnd; column++)
    {
        til::at(layer.columns, static_cast<size_t>(column - _columnBegin)) = 1;
    }
    _charge.Reserve(_layerBytes(layer) - before);
    _invalidateComposites();

    const auto pixelOffset = (columnBegin - _columnBegin) * _cellSize.width;
    return &til::at(layer.pixels, pixelOffset);
}

bool ImageSlice::Contains(const uint32_t imageId) const noexcept
{
    return std::any_of(_layers.begin(), _layers.end(), [=](const Layer& layer) { return layer.key.imageId == imageId; });
}

bool ImageSlice::Contains(const LayerKey key) const noexcept
{
    return std::any_of(_layers.begin(), _layers.end(), [=](const Layer& layer) { return layer.key == key; });
}

bool ImageSlice::LayerCoversColumn(const LayerKey key, const til::CoordType column) const noexcept
{
    if (column < _columnBegin || column >= _columnEnd)
    {
        return false;
    }
    const auto index = static_cast<size_t>(column - _columnBegin);
    return std::any_of(_layers.begin(), _layers.end(), [=](const Layer& layer) {
        return layer.key == key && index < layer.columns.size() && til::at(layer.columns, index) != 0;
    });
}

// The placement-only form, for callers that hold an identifier they know is
// unique and have no reason to also carry the image it came from.
bool ImageSlice::PlacementCoversColumn(const uint64_t placementId, const til::CoordType column) const noexcept
{
    if (column < _columnBegin || column >= _columnEnd)
    {
        return false;
    }
    const auto index = static_cast<size_t>(column - _columnBegin);
    return std::any_of(_layers.begin(), _layers.end(), [=](const Layer& layer) {
        return layer.key.placementId == placementId && index < layer.columns.size() && til::at(layer.columns, index) != 0;
    });
}

std::vector<ImageSlice::LayerKey> ImageSlice::LayersAtColumn(const til::CoordType column) const
{
    auto keys = std::vector<LayerKey>{};
    if (column < _columnBegin || column >= _columnEnd)
    {
        return keys;
    }
    const auto index = static_cast<size_t>(column - _columnBegin);
    for (const auto& layer : _layers)
    {
        if (index < layer.columns.size() && til::at(layer.columns, index) != 0)
        {
            keys.push_back(layer.key);
        }
    }
    return keys;
}

// The image that visually owns a column: the topmost layer covering it. Zero
// when no layer does. Callers use this to ask "whose pixels are here?" without
// having to know how many layers happen to overlap.
uint32_t ImageSlice::ColumnOwner(const til::CoordType column) const noexcept
{
    if (column < _columnBegin || column >= _columnEnd)
    {
        return 0;
    }
    const auto index = static_cast<size_t>(column - _columnBegin);
    for (auto it = _layers.rbegin(); it != _layers.rend(); ++it)
    {
        if (index < it->columns.size() && til::at(it->columns, index) != 0)
        {
            return it->key.imageId;
        }
    }
    return 0;
}

// A placement identifier is unique across every image, so it alone identifies a
// layer. Contains(LayerKey) is the stricter form for callers that know both.
bool ImageSlice::ContainsPlacement(const uint64_t placementId) const noexcept
{
    return std::ranges::any_of(_layers, [&](const auto& layer) {
        return layer.key.placementId == placementId;
    });
}

std::vector<ImageSlice::LayerKey> ImageSlice::LayersAtZ(const int32_t zIndex) const
{
    auto keys = std::vector<LayerKey>{};
    for (const auto& layer : _layers)
    {
        if (layer.zIndex == zIndex)
        {
            keys.push_back(layer.key);
        }
    }
    return keys;
}

// The erase overloads all report true when the slice has been left with no
// content at all, which tells the caller to drop the slice entirely.
bool ImageSlice::EraseLayer(const uint32_t imageId)
{
    auto erased = false;
    for (auto it = _layers.begin(); it != _layers.end();)
    {
        if (it->key.imageId == imageId)
        {
            _charge.Release(_layerBytes(*it));
            it = _layers.erase(it);
            erased = true;
        }
        else
        {
            ++it;
        }
    }
    if (erased)
    {
        _invalidateComposites();
    }
    return !_hasContent();
}

bool ImageSlice::EraseLayer(const LayerKey key)
{
    auto erased = false;
    for (auto it = _layers.begin(); it != _layers.end();)
    {
        if (it->key == key)
        {
            _charge.Release(_layerBytes(*it));
            it = _layers.erase(it);
            erased = true;
        }
        else
        {
            ++it;
        }
    }
    if (erased)
    {
        _invalidateComposites();
    }
    return !_hasContent();
}

bool ImageSlice::EraseLayer(const uint32_t imageId, const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    auto changed = false;
    for (auto& layer : _layers)
    {
        if (layer.key.imageId == imageId)
        {
            _clearLayerColumns(layer, columnBegin, columnEnd);
            changed = true;
        }
    }
    if (changed)
    {
        _removeEmptyLayers();
        _invalidateComposites();
    }
    return !_hasContent();
}

void ImageSlice::_copyLayers(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd)
{
    if (srcSlice._layers.empty() && _layers.empty())
    {
        return;
    }

    // A row can be copied onto itself, and writing to our own layers would
    // invalidate the source we're reading from, so take a snapshot first.
    const auto snapshot = &srcSlice == this ? _layers : std::vector<Layer>{};
    const auto& srcLayers = &srcSlice == this ? snapshot : srcSlice._layers;
    const auto srcColumnBegin = srcSlice._columnBegin;
    const auto srcPixelWidth = srcSlice._pixelWidth;
    const auto columnCount = dstColumnEnd - dstColumnBegin;

    // A copy replaces the destination range outright, so every layer already
    // there has to give up those columns before the source is written in.
    // Otherwise a destination layer with no counterpart in the source would
    // survive a copy that was meant to overwrite it.
    for (auto& dstLayer : _layers)
    {
        _clearLayerColumns(dstLayer, dstColumnBegin, dstColumnEnd);
    }

    for (const auto& srcLayer : srcLayers)
    {
        if (srcLayer.pixels.empty())
        {
            continue;
        }

        const auto dstPixels = TryMutablePixels(dstColumnBegin, dstColumnEnd, srcLayer.key, srcLayer.zIndex);
        if (!dstPixels)
        {
            // The budget is exhausted, so this layer simply doesn't survive the copy.
            continue;
        }

        auto srcIterator = std::next(srcLayer.pixels.data(), (srcColumn - srcColumnBegin) * _cellSize.width);
        auto dstIterator = dstPixels;
        const auto byteCount = sizeof(RGBQUAD) * columnCount * _cellSize.width;
        for (auto y = 0; y < _cellSize.height; y++)
        {
            std::memmove(dstIterator, srcIterator, byteCount);
            std::advance(srcIterator, srcPixelWidth);
            std::advance(dstIterator, _pixelWidth);
        }

        // Coverage has to follow the pixels, otherwise a later targeted erase
        // would skip the cells we just wrote.
        auto& dstLayer = _getLayer(srcLayer.key, srcLayer.zIndex);
        for (auto column = 0; column < columnCount; column++)
        {
            const auto srcIndex = static_cast<size_t>(srcColumn - srcColumnBegin + column);
            const auto dstIndex = static_cast<size_t>(dstColumnBegin - _columnBegin + column);
            const auto covered = srcIndex < srcLayer.columns.size() && til::at(srcLayer.columns, srcIndex) != 0;
            if (dstIndex < dstLayer.columns.size())
            {
                til::at(dstLayer.columns, dstIndex) = covered ? uint8_t{ 1 } : uint8_t{ 0 };
            }
        }
    }

    _removeEmptyLayers();
    _invalidateComposites();
}


size_t ImageSlice::WriteMemoryUpperBound(const til::CoordType columnBegin, const til::CoordType columnEnd, const LayerKey key, const int32_t zIndex) const noexcept
{
    const auto hasRange = _columnEnd > _columnBegin;
    const auto newColumnBegin = hasRange ? std::min(_columnBegin, columnBegin) : columnBegin;
    const auto newColumnEnd = hasRange ? std::max(_columnEnd, columnEnd) : columnEnd;
    const auto rangeExpands = !hasRange || newColumnBegin != _columnBegin || newColumnEnd != _columnEnd;
    const auto columnCount = static_cast<size_t>(std::max<til::CoordType>(0, newColumnEnd - newColumnBegin));
    const auto pixelCount = columnCount * static_cast<size_t>(_cellSize.width) * static_cast<size_t>(_cellSize.height);
    const auto layerBytes = pixelCount * (sizeof(RGBQUAD) + sizeof(uint32_t)) + columnCount;

    const auto hasLayer = std::any_of(_layers.begin(), _layers.end(), [&](const Layer& layer) {
        return layer.key == key && layer.zIndex == zIndex;
    });
    if (!hasLayer && _layers.size() >= MaxLayersPerSlice)
    {
        // Report more than could ever be available, so the caller refuses.
        return MaxLayerBytes + 1;
    }

    // Growing the range reallocates every existing layer as well as adding the
    // new one, and both have to be affordable at the same moment.
    return (rangeExpands ? _layers.size() * layerBytes : 0) + (hasLayer ? 0 : layerBytes);
}

std::vector<ImageSlice::LayerKey> ImageSlice::LayersAtZ(const int32_t zIndex, const til::CoordType column) const
{
    auto keys = std::vector<LayerKey>{};
    if (column < _columnBegin || column >= _columnEnd)
    {
        return keys;
    }
    const auto index = static_cast<size_t>(column - _columnBegin);
    for (const auto& layer : _layers)
    {
        if (layer.zIndex == zIndex && index < layer.columns.size() && til::at(layer.columns, index) != 0)
        {
            keys.push_back(layer.key);
        }
    }
    return keys;
}

bool ImageSlice::EraseLayer(const uint32_t imageId, const int32_t zIndex)
{
    auto erased = false;
    for (auto it = _layers.begin(); it != _layers.end();)
    {
        if (it->key.imageId == imageId && it->zIndex == zIndex)
        {
            _releaseLayerBytes(_layerBytes(*it));
            it = _layers.erase(it);
            erased = true;
        }
        else
        {
            ++it;
        }
    }
    if (erased)
    {
        BumpRevision();
        _invalidateComposites();
    }
    return !_hasContent();
}

void ImageSlice::ClearLayers(const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    if (_layers.empty())
    {
        return;
    }
    for (auto& layer : _layers)
    {
        _clearLayerColumns(layer, columnBegin, columnEnd);
    }
    _removeEmptyLayers();
    BumpRevision();
    _invalidateComposites();
}

void ImageSlice::EraseLayerCells(ROW& row, const til::CoordType columnBegin, const til::CoordType columnEnd, const LayerKey key)
{
    const auto imageSlice = row.GetMutableImageSlice();
    if (imageSlice) [[unlikely]]
    {
        const auto scale = row.GetLineRendition() != LineRendition::SingleWidth ? 1 : 0;
        if (imageSlice->_eraseLayerCells(columnBegin << scale, columnEnd << scale, key))
        {
            row.SetImageSlice(nullptr);
        }
    }
}

bool ImageSlice::_eraseLayerCells(const til::CoordType columnBegin, const til::CoordType columnEnd, const LayerKey key)
{
    auto erased = false;
    for (auto& layer : _layers)
    {
        if (layer.key == key)
        {
            _clearLayerColumns(layer, columnBegin, columnEnd);
            erased = true;
        }
    }
    if (erased)
    {
        _removeEmptyLayers();
        BumpRevision();
        _invalidateComposites();
    }
    return !_hasContent();
}

void ImageSlice::CopyLayerCells(const ROW& srcRow, const til::CoordType srcColumn, ROW& dstRow, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd, const LayerKey key)
{
    const auto srcSlice = srcRow.GetImageSlice();
    if (!srcSlice || srcRow.GetLineRendition() != dstRow.GetLineRendition()) [[unlikely]]
    {
        EraseLayerCells(dstRow, dstColumnBegin, dstColumnEnd, key);
        return;
    }

    auto dstSlice = dstRow.GetMutableImageSlice();
    if (dstSlice && dstSlice->CellSize() != srcSlice->CellSize()) [[unlikely]]
    {
        // The two rows disagree about cell geometry, so nothing can be carried
        // across; drop what was there rather than mixing two scales.
        EraseLayerCells(dstRow, dstColumnBegin, dstColumnEnd, key);
        return;
    }
    if (!dstSlice)
    {
        dstSlice = dstRow.SetImageSlice(std::make_unique<ImageSlice>(srcSlice->CellSize()));
        __assume(dstSlice != nullptr);
    }

    const auto scale = srcRow.GetLineRendition() != LineRendition::SingleWidth ? 1 : 0;
    if (dstSlice->_copyLayerCells(*srcSlice, srcColumn << scale, dstColumnBegin << scale, dstColumnEnd << scale, key))
    {
        dstRow.SetImageSlice(nullptr);
    }
}

bool ImageSlice::_copyLayerCells(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd, const LayerKey key)
{
    // A row can be copied onto itself, so read from a snapshot.
    const auto snapshot = &srcSlice == this ? _layers : std::vector<Layer>{};
    const auto& srcLayers = &srcSlice == this ? snapshot : srcSlice._layers;
    const auto srcColumnBegin = srcSlice._columnBegin;
    const auto srcPixelWidth = srcSlice._pixelWidth;
    const auto columnCount = dstColumnEnd - dstColumnBegin;

    // Only this layer's cells are being moved, so only this layer's cells are
    // cleared; anything else covering the range is left alone.
    for (auto& dstLayer : _layers)
    {
        if (dstLayer.key == key)
        {
            _clearLayerColumns(dstLayer, dstColumnBegin, dstColumnEnd);
        }
    }

    for (const auto& srcLayer : srcLayers)
    {
        if (srcLayer.key != key || srcLayer.pixels.empty())
        {
            continue;
        }

        // The caller names a source column taken from cell metadata, which is not proof
        // that this layer's pixels reach that column -- a placement can record a cell
        // reference and then decline to draw it. Reading outside the plane would copy
        // unrelated heap into a layer that is about to be composited and painted, so an
        // uncovered source is treated as "nothing to carry across".
        //
        // The bound comes from the layer's own plane rather than from the slice's column
        // range, so it stays correct for the entry-time snapshot taken when a row is
        // copied onto itself, and holds even if the destination grows this slice mid-loop.
        const auto srcOffset = srcColumn - srcColumnBegin;
        const auto lastRow = static_cast<size_t>(std::max(0, _cellSize.height - 1)) * srcPixelWidth;
        const auto rowExtent = static_cast<size_t>(srcOffset + columnCount) * _cellSize.width;
        if (srcOffset < 0 || columnCount < 0 || lastRow + rowExtent > srcLayer.pixels.size())
        {
            continue;
        }

        const auto dstPixels = TryMutablePixels(dstColumnBegin, dstColumnEnd, srcLayer.key, srcLayer.zIndex);
        if (!dstPixels)
        {
            continue;
        }

        auto srcIterator = std::next(srcLayer.pixels.data(), srcOffset * _cellSize.width);
        auto dstIterator = dstPixels;
        const auto byteCount = sizeof(RGBQUAD) * columnCount * _cellSize.width;
        for (auto y = 0; y < _cellSize.height; y++)
        {
            std::memmove(dstIterator, srcIterator, byteCount);
            std::advance(srcIterator, srcPixelWidth);
            std::advance(dstIterator, _pixelWidth);
        }

        auto& dstLayer = _getLayer(srcLayer.key, srcLayer.zIndex);
        for (auto column = 0; column < columnCount; column++)
        {
            const auto srcIndex = static_cast<size_t>(srcColumn - srcColumnBegin + column);
            const auto dstIndex = static_cast<size_t>(dstColumnBegin - _columnBegin + column);
            const auto covered = srcIndex < srcLayer.columns.size() && til::at(srcLayer.columns, srcIndex) != 0;
            if (dstIndex < dstLayer.columns.size())
            {
                til::at(dstLayer.columns, dstIndex) = covered ? uint8_t{ 1 } : uint8_t{ 0 };
            }
        }
    }

    _removeEmptyLayers();
    BumpRevision();
    _invalidateComposites();
    return !_hasContent();
}

// True if any pixel of this column in the untagged base plane is non-zero.
bool ImageSlice::_baseCellHasPixels(const til::CoordType column) const noexcept
{
    if (_pixelBuffer.empty() || column < _columnBegin || column >= _columnEnd)
    {
        return false;
    }
    auto iterator = std::next(_pixelBuffer.data(), (column - _columnBegin) * _cellSize.width);
    for (auto y = 0; y < _cellSize.height; y++)
    {
        for (auto x = 0; x < _cellSize.width; x++)
        {
            if (std::bit_cast<uint32_t>(iterator[x]) != 0)
            {
                return true;
            }
        }
        std::advance(iterator, _pixelWidth);
    }
    return false;
}

void ImageSlice::MergePreservedCells(Pointer srcSlice, ROW& dstRow)
{
    if (!srcSlice)
    {
        return;
    }

    auto dstSlice = dstRow.GetMutableImageSlice();
    if (dstSlice && dstSlice->CellSize() != srcSlice->CellSize()) [[unlikely]]
    {
        return;
    }
    if (!dstSlice)
    {
        dstRow.SetImageSlice(std::move(srcSlice));
        return;
    }
    dstSlice->_mergePreservedCells(*srcSlice);
    if (!dstSlice->_hasContent())
    {
        dstRow.SetImageSlice(nullptr);
    }
}

// Fills in only what the destination doesn't already have. A merge never
// overwrites, so a cell the destination has already claimed always wins.
void ImageSlice::_mergePreservedCells(const ImageSlice& srcSlice)
{
    for (auto column = srcSlice._columnBegin; column < srcSlice._columnEnd; column++)
    {
        if (!srcSlice._baseCellHasPixels(column) || _baseCellHasPixels(column))
        {
            continue;
        }

        auto source = srcSlice.Pixels(column);
        auto destination = MutablePixels(column, column + 1);
        const auto rowByteCount = static_cast<size_t>(_cellSize.width) * sizeof(RGBQUAD);
        for (auto y = 0; y < _cellSize.height; y++)
        {
            std::memcpy(destination, source, rowByteCount);
            std::advance(source, srcSlice._pixelWidth);
            std::advance(destination, _pixelWidth);
        }
    }

    for (const auto& srcLayer : srcSlice._layers)
    {
        const auto covered = std::any_of(srcLayer.columns.begin(), srcLayer.columns.end(), [](const uint8_t flag) { return flag != 0; });
        if (!covered || srcLayer.pixels.empty())
        {
            continue;
        }

        for (auto column = srcSlice._columnBegin; column < srcSlice._columnEnd; column++)
        {
            const auto srcIndex = static_cast<size_t>(column - srcSlice._columnBegin);
            if (srcIndex >= srcLayer.columns.size() || til::at(srcLayer.columns, srcIndex) == 0)
            {
                continue;
            }
            if (LayerCoversColumn(srcLayer.key, column))
            {
                continue;
            }

            const auto dstPixels = TryMutablePixels(column, column + 1, srcLayer.key, srcLayer.zIndex);
            if (!dstPixels)
            {
                // Out of budget: the rest of this layer simply isn't preserved.
                break;
            }
            auto source = std::next(srcLayer.pixels.data(), static_cast<til::CoordType>(srcIndex) * _cellSize.width);
            auto destination = dstPixels;
            const auto rowByteCount = static_cast<size_t>(_cellSize.width) * sizeof(RGBQUAD);
            for (auto y = 0; y < _cellSize.height; y++)
            {
                std::memcpy(destination, source, rowByteCount);
                std::advance(source, srcSlice._pixelWidth);
                std::advance(destination, _pixelWidth);
            }
        }
    }

    BumpRevision();
    _invalidateComposites();
}