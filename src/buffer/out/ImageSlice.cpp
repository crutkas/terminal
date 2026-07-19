// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "ImageSlice.hpp"
#include "Row.hpp"
#include "textBuffer.hpp"

static std::atomic<uint64_t> s_revision{ 0 };
static std::atomic<size_t> s_kittyLayerBytes{ 0 };
static constexpr size_t KittyLayerAllocationOverhead = 128;

ImageSlice::ImageSlice(const til::size cellSize) noexcept :
    _cellSize{ cellSize }
{
}

ImageSlice::ImageSlice(const ImageSlice& rhs) :
    _revision{ rhs._revision },
    _cellSize{ rhs._cellSize },
    _pixelBuffer{ rhs._pixelBuffer },
    _columnOwners{ rhs._columnOwners },
    _columnBegin{ rhs._columnBegin },
    _columnEnd{ rhs._columnEnd },
    _pixelWidth{ rhs._pixelWidth }
{
    auto layerBytes = size_t{ 0 };
    for (const auto& layer : rhs._kittyLayers)
    {
        layerBytes += _layerStorageBytes(layer);
    }
    _reserveKittyBytes(layerBytes);
    auto rollback = wil::scope_exit([&]() noexcept {
        _releaseKittyBytes(layerBytes);
    });
    _kittyLayers = rhs._kittyLayers;
    rollback.release();
}

ImageSlice::ImageSlice(ImageSlice&& rhs) noexcept :
    _revision{ rhs._revision },
    _cellSize{ rhs._cellSize },
    _pixelBuffer{ std::move(rhs._pixelBuffer) },
    _columnOwners{ std::move(rhs._columnOwners) },
    _columnBegin{ rhs._columnBegin },
    _columnEnd{ rhs._columnEnd },
    _pixelWidth{ rhs._pixelWidth },
    _kittyLayers{ std::move(rhs._kittyLayers) },
    _composites{ std::move(rhs._composites) },
    _accountedKittyBytes{ std::exchange(rhs._accountedKittyBytes, 0) }
{
}

ImageSlice::~ImageSlice() noexcept
{
    _releaseKittyBytes(_accountedKittyBytes);
}

size_t ImageSlice::_layerStorageBytes(const KittyLayer& layer) noexcept
{
    return sizeof(KittyLayer) + KittyLayerAllocationOverhead +
           layer.pixels.size() * sizeof(RGBQUAD) +
           layer.sourceIndices.size() * sizeof(uint32_t) +
           layer.columns.size() * sizeof(uint8_t);
}

size_t ImageSlice::KittyLayerBytesAvailable() noexcept
{
    const auto used = s_kittyLayerBytes.load(std::memory_order_relaxed);
    return used < MaxKittyLayerBytes ? MaxKittyLayerBytes - used : 0;
}

void ImageSlice::_reserveKittyBytes(const size_t bytes) const
{
    if (bytes == 0)
    {
        return;
    }

    auto current = s_kittyLayerBytes.load(std::memory_order_relaxed);
    do
    {
        if (current > MaxKittyLayerBytes || bytes > MaxKittyLayerBytes - current)
        {
            throw std::bad_alloc{};
        }
    } while (!s_kittyLayerBytes.compare_exchange_weak(current, current + bytes, std::memory_order_relaxed));
    _accountedKittyBytes += bytes;
}

void ImageSlice::_releaseKittyBytes(const size_t bytes) const noexcept
{
    if (bytes != 0)
    {
        _accountedKittyBytes -= bytes;
        s_kittyLayerBytes.fetch_sub(bytes, std::memory_order_relaxed);
    }
}

void ImageSlice::_clearCompositeCaches() const noexcept
{
    for (auto& composite : _composites)
    {
        composite = {};
    }
}

void ImageSlice::BumpRevision() noexcept
{
    _clearCompositeCaches();
    // Avoid setting the revision to 0. This allows the renderer to use 0 as a sentinel value.
    do
    {
        _revision = s_revision.fetch_add(1, std::memory_order_relaxed);
    } while (_revision == 0);
}

uint64_t ImageSlice::Revision() const noexcept
{
    return Revision(RenderPosition::AboveText);
}

uint64_t ImageSlice::Revision(const RenderPosition position) const noexcept
{
    return (_revision << 2) | (static_cast<uint64_t>(position) + 1);
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
    for (auto it = _kittyLayers.rbegin(); it != _kittyLayers.rend(); ++it)
    {
        if (index < it->columns.size() && til::at(it->columns, index) != 0)
        {
            return it->imageId;
        }
    }
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
    if (std::find(_columnOwners.begin(), _columnOwners.end(), id) != _columnOwners.end())
    {
        return true;
    }
    return std::any_of(_kittyLayers.begin(), _kittyLayers.end(), [&](const auto& layer) {
        return layer.imageId == id && std::find(layer.columns.begin(), layer.columns.end(), uint8_t{ 1 }) != layer.columns.end();
    });
}

std::vector<uint32_t> ImageSlice::ColumnOwners(const til::CoordType column) const
{
    std::vector<uint32_t> owners;
    if (column < _columnBegin || column >= _columnEnd)
    {
        return owners;
    }
    const auto index = static_cast<size_t>(column - _columnBegin);
    for (const auto& layer : _kittyLayers)
    {
        if (index < layer.columns.size() && til::at(layer.columns, index) != 0 &&
            std::find(owners.begin(), owners.end(), layer.imageId) == owners.end())
        {
            owners.push_back(layer.imageId);
        }
    }
    if (index < _columnOwners.size())
    {
        const auto owner = til::at(_columnOwners, index);
        if (owner != 0 && std::find(owners.begin(), owners.end(), owner) == owners.end())
        {
            owners.push_back(owner);
        }
    }
    return owners;
}

std::vector<uint32_t> ImageSlice::ImageIdsAtZ(const int32_t zIndex) const
{
    std::vector<uint32_t> imageIds;
    for (const auto& layer : _kittyLayers)
    {
        if (layer.zIndex == zIndex &&
            std::find(layer.columns.begin(), layer.columns.end(), uint8_t{ 1 }) != layer.columns.end())
        {
            imageIds.push_back(layer.imageId);
        }
    }
    return imageIds;
}

std::vector<uint32_t> ImageSlice::ImageIdsAtZ(const int32_t zIndex, const til::CoordType column) const
{
    std::vector<uint32_t> imageIds;
    if (column < _columnBegin || column >= _columnEnd)
    {
        return imageIds;
    }

    const auto index = static_cast<size_t>(column - _columnBegin);
    for (const auto& layer : _kittyLayers)
    {
        if (layer.zIndex == zIndex && index < layer.columns.size() && til::at(layer.columns, index) != 0)
        {
            imageIds.push_back(layer.imageId);
        }
    }
    return imageIds;
}

std::vector<ImageSlice::KittyLayerIdentity> ImageSlice::KittyLayersAtZ(const int32_t zIndex) const
{
    std::vector<KittyLayerIdentity> identities;
    for (const auto& layer : _kittyLayers)
    {
        if (layer.zIndex == zIndex &&
            std::find(layer.columns.begin(), layer.columns.end(), uint8_t{ 1 }) != layer.columns.end())
        {
            identities.push_back({ layer.imageId, layer.placementId });
        }
    }
    return identities;
}

std::vector<ImageSlice::KittyLayerIdentity> ImageSlice::KittyLayersAtZ(const int32_t zIndex, const til::CoordType column) const
{
    std::vector<KittyLayerIdentity> identities;
    if (column < _columnBegin || column >= _columnEnd)
    {
        return identities;
    }

    const auto index = static_cast<size_t>(column - _columnBegin);
    for (const auto& layer : _kittyLayers)
    {
        if (layer.zIndex == zIndex && index < layer.columns.size() && til::at(layer.columns, index) != 0)
        {
            identities.push_back({ layer.imageId, layer.placementId });
        }
    }
    return identities;
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
    for (auto& layer : _kittyLayers)
    {
        _clearLayerColumns(layer, columnBegin, columnEnd);
    }
    _removeEmptyKittyLayers();
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
    if (id == 0)
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
    for (auto& layer : _kittyLayers)
    {
        if (layer.imageId == id)
        {
            _clearLayerColumns(layer, columnBegin, columnEnd);
        }
    }
    _removeEmptyKittyLayers();
    return !_hasContent();
}

bool ImageSlice::EraseByZ(const int32_t zIndex, const uint32_t imageId)
{
    auto released = size_t{ 0 };
    for (const auto& layer : _kittyLayers)
    {
        if (layer.zIndex == zIndex && layer.imageId == imageId)
        {
            released += _layerStorageBytes(layer);
        }
    }
    std::erase_if(_kittyLayers, [&](const auto& layer) {
        return layer.zIndex == zIndex && layer.imageId == imageId;
    });
    if (released != 0)
    {
        _releaseKittyBytes(released);
        BumpRevision();
    }
    return !_hasContent();
}

bool ImageSlice::EraseByPlacement(const uint64_t placementId)
{
    if (placementId == 0)
    {
        return !_hasContent();
    }
    auto released = size_t{ 0 };
    for (const auto& layer : _kittyLayers)
    {
        if (layer.placementId == placementId)
        {
            released += _layerStorageBytes(layer);
        }
    }
    std::erase_if(_kittyLayers, [&](const auto& layer) {
        return layer.placementId == placementId;
    });
    if (released != 0)
    {
        _releaseKittyBytes(released);
        BumpRevision();
    }
    return !_hasContent();
}

bool ImageSlice::HasPlacement(const uint64_t placementId) const noexcept
{
    return placementId != 0 && std::any_of(_kittyLayers.begin(), _kittyLayers.end(), [&](const auto& layer) {
        return layer.placementId == placementId &&
               std::find(layer.columns.begin(), layer.columns.end(), uint8_t{ 1 }) != layer.columns.end();
    });
}

bool ImageSlice::PlacementCoversColumn(const uint64_t placementId, const til::CoordType column) const noexcept
{
    if (placementId == 0 || column < _columnBegin || column >= _columnEnd)
    {
        return false;
    }
    const auto index = static_cast<size_t>(column - _columnBegin);
    return std::any_of(_kittyLayers.begin(), _kittyLayers.end(), [&](const auto& layer) {
        return layer.placementId == placementId && index < layer.columns.size() && til::at(layer.columns, index) != 0;
    });
}

bool ImageSlice::UpdateKittyImage(const uint32_t imageId, const std::span<const RGBQUAD> pixels)
{
    auto changed = false;
    for (auto& layer : _kittyLayers)
    {
        if (layer.imageId != imageId || layer.sourceIndices.size() != layer.pixels.size())
        {
            continue;
        }
        for (size_t i = 0; i < layer.pixels.size(); ++i)
        {
            const auto sourceIndex = til::at(layer.sourceIndices, i);
            if (sourceIndex != NoSourceIndex)
            {
                til::at(layer.pixels, i) = sourceIndex < pixels.size() ? pixels[sourceIndex] : RGBQUAD{};
                changed = true;
            }
        }
    }
    if (changed)
    {
        BumpRevision();
    }
    return changed;
}

void ImageSlice::_clearLayerColumns(KittyLayer& layer, const til::CoordType columnBegin, const til::CoordType columnEnd) noexcept
{
    const auto eraseBegin = std::max(columnBegin, _columnBegin);
    const auto eraseEnd = std::min(columnEnd, _columnEnd);
    for (auto column = eraseBegin; column < eraseEnd; ++column)
    {
        const auto index = static_cast<size_t>(column - _columnBegin);
        if (index >= layer.columns.size() || til::at(layer.columns, index) == 0)
        {
            continue;
        }
        til::at(layer.columns, index) = 0;
        auto pixels = std::next(layer.pixels.data(), static_cast<til::CoordType>(index) * _cellSize.width);
        for (auto y = 0; y < _cellSize.height; ++y)
        {
            std::memset(pixels, 0, _cellSize.width * sizeof(RGBQUAD));
            std::advance(pixels, _pixelWidth);
        }
        auto sourceIndices = std::next(layer.sourceIndices.data(), static_cast<til::CoordType>(index) * _cellSize.width);
        for (auto y = 0; y < _cellSize.height; ++y)
        {
            std::fill_n(sourceIndices, _cellSize.width, NoSourceIndex);
            std::advance(sourceIndices, _pixelWidth);
        }
    }
}

void ImageSlice::_removeEmptyKittyLayers()
{
    auto released = size_t{ 0 };
    for (const auto& layer : _kittyLayers)
    {
        if (std::find(layer.columns.begin(), layer.columns.end(), uint8_t{ 1 }) == layer.columns.end())
        {
            released += _layerStorageBytes(layer);
        }
    }
    std::erase_if(_kittyLayers, [](const auto& layer) {
        return std::find(layer.columns.begin(), layer.columns.end(), uint8_t{ 1 }) == layer.columns.end();
    });
    _releaseKittyBytes(released);
}

bool ImageSlice::_hasContent() const noexcept
{
    if (std::find_if(_columnOwners.begin(), _columnOwners.end(), [](const auto owner) { return owner != 0; }) != _columnOwners.end())
    {
        return true;
    }
    for (const auto& px : _pixelBuffer)
    {
        if (px.rgbReserved != 0 || px.rgbRed != 0 || px.rgbGreen != 0 || px.rgbBlue != 0)
        {
            return true;
        }
    }
    return std::any_of(_kittyLayers.begin(), _kittyLayers.end(), [](const auto& layer) {
        return std::find(layer.columns.begin(), layer.columns.end(), uint8_t{ 1 }) != layer.columns.end();
    });
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
    return Pixels(RenderPosition::AboveText);
}

std::span<const RGBQUAD> ImageSlice::Pixels(const RenderPosition position) const noexcept
{
    return _composite(position).pixels;
}

bool ImageSlice::HasPixels(const RenderPosition position) const noexcept
{
    return _composite(position).hasPixels;
}

size_t ImageSlice::KittyWriteMemoryUpperBound(const til::CoordType columnBegin, const til::CoordType columnEnd, const int32_t zIndex, const uint32_t imageId, const uint64_t placementId) const noexcept
{
    const auto hadRange = _columnBegin < _columnEnd;
    const auto newColumnBegin = hadRange ? std::min(_columnBegin, columnBegin) : columnBegin;
    const auto newColumnEnd = hadRange ? std::max(_columnEnd, columnEnd) : columnEnd;
    const auto rangeExpands = !hadRange || newColumnBegin != _columnBegin || newColumnEnd != _columnEnd;
    const auto columnCount = static_cast<size_t>(std::max<til::CoordType>(0, newColumnEnd - newColumnBegin));
    const auto pixelCount = columnCount * static_cast<size_t>(_cellSize.width) * static_cast<size_t>(_cellSize.height);
    const auto layerBytes = sizeof(KittyLayer) + KittyLayerAllocationOverhead +
                            pixelCount * sizeof(RGBQUAD) +
                            pixelCount * sizeof(uint32_t) +
                            columnCount * sizeof(uint8_t);
    const auto hasLayer = std::any_of(_kittyLayers.begin(), _kittyLayers.end(), [&](const auto& layer) {
        return layer.zIndex == zIndex && layer.imageId == imageId && layer.placementId == placementId;
    });
    if (!hasLayer && _kittyLayers.size() >= MaxKittyLayersPerSlice)
    {
        return MaxKittyLayerBytes + 1;
    }
    return (rangeExpands ? _kittyLayers.size() * layerBytes : 0) + (hasLayer ? 0 : layerBytes);
}

const RGBQUAD* ImageSlice::Pixels(const til::CoordType columnBegin) const noexcept
{
    const auto pixelOffset = (columnBegin - _columnBegin) * _cellSize.width;
    return &til::at(_pixelBuffer, pixelOffset);
}

RGBQUAD* ImageSlice::MutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    _ensureRange(columnBegin, columnEnd);
    const auto pixelOffset = (columnBegin - _columnBegin) * _cellSize.width;
    return &til::at(_pixelBuffer, pixelOffset);
}

RGBQUAD* ImageSlice::MutablePixels(const til::CoordType columnBegin, const til::CoordType columnEnd, const int32_t zIndex, const uint32_t imageId, const uint64_t placementId)
{
    _ensureRange(columnBegin, columnEnd);
    auto& layer = _getKittyLayer(zIndex, imageId, placementId);
    const auto ownerBegin = static_cast<size_t>(columnBegin - _columnBegin);
    const auto ownerEnd = static_cast<size_t>(columnEnd - _columnBegin);
    std::fill(layer.columns.begin() + ownerBegin, layer.columns.begin() + ownerEnd, uint8_t{ 1 });
    const auto pixelOffset = (columnBegin - _columnBegin) * _cellSize.width;
    return &til::at(layer.pixels, pixelOffset);
}

uint32_t* ImageSlice::MutableSourceIndices(const til::CoordType columnBegin, const til::CoordType columnEnd, const int32_t zIndex, const uint32_t imageId, const uint64_t placementId)
{
    _ensureRange(columnBegin, columnEnd);
    auto& layer = _getKittyLayer(zIndex, imageId, placementId);
    const auto pixelOffset = (columnBegin - _columnBegin) * _cellSize.width;
    return &til::at(layer.sourceIndices, pixelOffset);
}

void ImageSlice::_ensureRange(const til::CoordType columnBegin, const til::CoordType columnEnd)
{
    if (_columnBegin < _columnEnd && columnBegin >= _columnBegin && columnEnd <= _columnEnd)
    {
        return;
    }

    const auto hadRange = _columnBegin < _columnEnd;
    const auto oldColumnBegin = _columnBegin;
    const auto oldPixelWidth = _pixelWidth;
    const auto newColumnBegin = hadRange ? std::min(_columnBegin, columnBegin) : columnBegin;
    const auto newColumnEnd = hadRange ? std::max(_columnEnd, columnEnd) : columnEnd;
    const auto newPixelWidth = (newColumnEnd - newColumnBegin) * _cellSize.width;
    const auto newBufferSize = newPixelWidth * _cellSize.height;
    const auto newColumnCount = static_cast<size_t>(newColumnEnd - newColumnBegin);
    const auto newPixelOffset = hadRange ? (oldColumnBegin - newColumnBegin) * _cellSize.width : 0;
    const auto newOwnerOffset = hadRange ? static_cast<size_t>(oldColumnBegin - newColumnBegin) : 0;

    const auto resizePixels = [&](const std::vector<RGBQUAD>& pixels) {
        auto resized = std::vector<RGBQUAD>(newBufferSize);
        if (hadRange && !pixels.empty())
        {
            auto dst = std::next(resized.data(), newPixelOffset);
            auto src = pixels.data();
            const auto copyWidth = std::min(oldPixelWidth, newPixelWidth - newPixelOffset);
            for (auto y = 0; y < _cellSize.height; ++y)
            {
                std::memcpy(dst, src, copyWidth * sizeof(RGBQUAD));
                std::advance(src, oldPixelWidth);
                std::advance(dst, newPixelWidth);
            }
        }
        return resized;
    };
    const auto resizeColumns = [&](const auto& columns) {
        using Value = typename std::remove_reference_t<decltype(columns)>::value_type;
        auto resized = std::vector<Value>(newColumnCount);
        if (hadRange && !columns.empty())
        {
            const auto copyCount = std::min(columns.size(), newColumnCount - newOwnerOffset);
            std::copy_n(columns.begin(), copyCount, resized.begin() + newOwnerOffset);
        }
        return resized;
    };
    const auto resizeSourceIndices = [&](const std::vector<uint32_t>& sourceIndices) {
        auto resized = std::vector<uint32_t>(newBufferSize, NoSourceIndex);
        if (hadRange && !sourceIndices.empty())
        {
            auto dst = std::next(resized.data(), newPixelOffset);
            auto src = sourceIndices.data();
            const auto copyWidth = std::min(oldPixelWidth, newPixelWidth - newPixelOffset);
            for (auto y = 0; y < _cellSize.height; ++y)
            {
                std::memcpy(dst, src, copyWidth * sizeof(uint32_t));
                std::advance(src, oldPixelWidth);
                std::advance(dst, newPixelWidth);
            }
        }
        return resized;
    };

    auto resizedPixelBuffer = resizePixels(_pixelBuffer);
    auto resizedColumnOwners = resizeColumns(_columnOwners);

    auto oldLayerBytes = size_t{ 0 };
    for (const auto& layer : _kittyLayers)
    {
        oldLayerBytes += _layerStorageBytes(layer);
    }
    const auto newLayerBytes = _kittyLayers.size() * (sizeof(KittyLayer) + KittyLayerAllocationOverhead +
                                                       newBufferSize * sizeof(RGBQUAD) +
                                                       newBufferSize * sizeof(uint32_t) +
                                                       newColumnCount * sizeof(uint8_t));
    _reserveKittyBytes(newLayerBytes);
    auto rollback = wil::scope_exit([&]() noexcept {
        _releaseKittyBytes(newLayerBytes);
    });

    auto resizedLayers = std::vector<KittyLayer>{};
    resizedLayers.reserve(_kittyLayers.size());
    for (const auto& layer : _kittyLayers)
    {
        KittyLayer resizedLayer;
        resizedLayer.zIndex = layer.zIndex;
        resizedLayer.imageId = layer.imageId;
        resizedLayer.placementId = layer.placementId;
        resizedLayer.pixels = resizePixels(layer.pixels);
        resizedLayer.sourceIndices = resizeSourceIndices(layer.sourceIndices);
        resizedLayer.columns = resizeColumns(layer.columns);
        resizedLayers.emplace_back(std::move(resizedLayer));
    }

    _pixelBuffer = std::move(resizedPixelBuffer);
    _columnOwners = std::move(resizedColumnOwners);
    _kittyLayers = std::move(resizedLayers);
    _columnBegin = newColumnBegin;
    _columnEnd = newColumnEnd;
    _pixelWidth = newPixelWidth;
    _releaseKittyBytes(oldLayerBytes);
    rollback.release();
}

ImageSlice::KittyLayer& ImageSlice::_getKittyLayer(const int32_t zIndex, const uint32_t imageId, const uint64_t placementId)
{
    const auto key = std::tuple{ zIndex, imageId, placementId };
    const auto it = std::lower_bound(_kittyLayers.begin(), _kittyLayers.end(), key, [](const auto& layer, const auto& value) {
        return std::tuple{ layer.zIndex, layer.imageId, layer.placementId } < value;
    });
    if (it != _kittyLayers.end() && it->zIndex == zIndex && it->imageId == imageId && it->placementId == placementId)
    {
        return *it;
    }
    if (_kittyLayers.size() >= MaxKittyLayersPerSlice)
    {
        throw std::bad_alloc{};
    }
    KittyLayer layer;
    layer.zIndex = zIndex;
    layer.imageId = imageId;
    layer.placementId = placementId;
    const auto layerBytes = sizeof(KittyLayer) + KittyLayerAllocationOverhead +
                            _pixelBuffer.size() * sizeof(RGBQUAD) +
                            _pixelBuffer.size() * sizeof(uint32_t) +
                            _columnOwners.size() * sizeof(uint8_t);
    _reserveKittyBytes(layerBytes);
    auto rollback = wil::scope_exit([&]() noexcept {
        _releaseKittyBytes(layerBytes);
    });
    layer.pixels.resize(_pixelBuffer.size());
    layer.sourceIndices.assign(_pixelBuffer.size(), NoSourceIndex);
    layer.columns.resize(_columnOwners.size());
    auto& inserted = *_kittyLayers.insert(it, std::move(layer));
    rollback.release();
    return inserted;
}

const ImageSlice::Composite& ImageSlice::_composite(const RenderPosition position) const noexcept
try
{
    const auto positionIndex = static_cast<size_t>(position);
    auto& result = til::at(_composites, positionIndex);
    const auto revision = Revision(position);
    if (result.revision == revision)
    {
        return result;
    }

    const auto belongsToPosition = [&](const KittyLayer& layer) {
        return position == RenderPosition::BehindBackground ? layer.zIndex < BackgroundZThreshold :
               position == RenderPosition::BehindText       ? layer.zIndex < 0 && layer.zIndex >= BackgroundZThreshold :
                                                              layer.zIndex >= 0;
    };
    if (position != RenderPosition::AboveText &&
        std::find_if(_kittyLayers.begin(), _kittyLayers.end(), belongsToPosition) == _kittyLayers.end())
    {
        result = {};
        result.revision = revision;
        return result;
    }

    result = {};
    result.pixels.assign(_pixelBuffer.size(), RGBQUAD{});
    if (position == RenderPosition::AboveText)
    {
        result.pixels = _pixelBuffer;
    }

    for (const auto& layer : _kittyLayers)
    {
        if (!belongsToPosition(layer) || layer.pixels.size() != result.pixels.size())
        {
            continue;
        }

        for (size_t i = 0; i < result.pixels.size(); ++i)
        {
            const auto& src = til::at(layer.pixels, i);
            auto& dst = til::at(result.pixels, i);
            const auto inverseAlpha = 255u - src.rgbReserved;
            dst.rgbRed = static_cast<BYTE>(std::min(255u, static_cast<uint32_t>(src.rgbRed) + (static_cast<uint32_t>(dst.rgbRed) * inverseAlpha + 127u) / 255u));
            dst.rgbGreen = static_cast<BYTE>(std::min(255u, static_cast<uint32_t>(src.rgbGreen) + (static_cast<uint32_t>(dst.rgbGreen) * inverseAlpha + 127u) / 255u));
            dst.rgbBlue = static_cast<BYTE>(std::min(255u, static_cast<uint32_t>(src.rgbBlue) + (static_cast<uint32_t>(dst.rgbBlue) * inverseAlpha + 127u) / 255u));
            dst.rgbReserved = static_cast<BYTE>(std::min(255u, static_cast<uint32_t>(src.rgbReserved) + (static_cast<uint32_t>(dst.rgbReserved) * inverseAlpha + 127u) / 255u));
        }
    }

    result.hasPixels = std::any_of(result.pixels.begin(), result.pixels.end(), [](const auto& pixel) {
        return pixel.rgbReserved != 0 || pixel.rgbRed != 0 || pixel.rgbGreen != 0 || pixel.rgbBlue != 0;
    });
    result.revision = revision;
    return result;
}
catch (const std::bad_alloc&)
{
    LOG_HR(E_OUTOFMEMORY);
    auto& result = til::at(_composites, static_cast<size_t>(position));
    result = {};
    return result;
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

void ImageSlice::CopyKittyCells(const ROW& srcRow, const til::CoordType srcColumn, ROW& dstRow, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd, const uint32_t imageId)
{
    const auto srcSlice = srcRow.GetImageSlice();
    if (!srcSlice || srcRow.GetLineRendition() != dstRow.GetLineRendition()) [[unlikely]]
    {
        EraseKittyCells(dstRow, dstColumnBegin, dstColumnEnd, imageId);
        return;
    }

    auto dstSlice = dstRow.GetMutableImageSlice();
    if (dstSlice && dstSlice->CellSize() != srcSlice->CellSize()) [[unlikely]]
    {
        EraseKittyCells(dstRow, dstColumnBegin, dstColumnEnd, imageId);
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
    if (dstSlice->_copyKittyCells(*srcSlice, srcColumn << scale, dstColumnBegin << scale, dstColumnEnd << scale, imageId))
    {
        dstRow.SetImageSlice(nullptr);
    }
}

void ImageSlice::_swap(ImageSlice& other) noexcept
{
    using std::swap;
    swap(_revision, other._revision);
    swap(_cellSize, other._cellSize);
    swap(_pixelBuffer, other._pixelBuffer);
    swap(_columnOwners, other._columnOwners);
    swap(_columnBegin, other._columnBegin);
    swap(_columnEnd, other._columnEnd);
    swap(_pixelWidth, other._pixelWidth);
    swap(_kittyLayers, other._kittyLayers);
    swap(_composites, other._composites);
    swap(_accountedKittyBytes, other._accountedKittyBytes);
}

bool ImageSlice::_copyCells(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd)
try
{
    ImageSlice staged{ *this };
    const auto empty = staged._copyCellsInPlace(srcSlice, srcColumn, dstColumnBegin, dstColumnEnd);
    if (!empty)
    {
        _swap(staged);
    }
    return empty;
}
catch (const std::bad_alloc&)
{
    LOG_HR(E_OUTOFMEMORY);
    return false;
}

bool ImageSlice::_copyCellsInPlace(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd)
{
    if (_cellSize != srcSlice._cellSize)
    {
        return _eraseCells(dstColumnBegin, dstColumnEnd);
    }

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
        auto available = KittyLayerBytesAvailable();
        for (const auto& srcLayer : srcSlice._kittyLayers)
        {
            const auto required = KittyWriteMemoryUpperBound(dstWriteBegin, dstWriteEnd, srcLayer.zIndex, srcLayer.imageId, srcLayer.placementId);
            if (required > available)
            {
                LOG_HR(E_OUTOFMEMORY);
                return false;
            }
            available -= required;
        }

        struct LayerSnapshot
        {
            int32_t zIndex = 0;
            uint32_t imageId = 0;
            uint64_t placementId = 0;
            std::vector<RGBQUAD> pixels;
            std::vector<uint32_t> sourceIndices;
            std::vector<uint8_t> columns;
        };

        const auto writeCellCount = dstWriteEnd - dstWriteBegin;
        const auto writePixelWidth = writeCellCount * _cellSize.width;
        std::vector<LayerSnapshot> layerSnapshots;
        layerSnapshots.reserve(srcSlice._kittyLayers.size());
        for (const auto& srcLayer : srcSlice._kittyLayers)
        {
            LayerSnapshot snapshot;
            snapshot.zIndex = srcLayer.zIndex;
            snapshot.imageId = srcLayer.imageId;
            snapshot.placementId = srcLayer.placementId;
            snapshot.pixels.resize(static_cast<size_t>(writePixelWidth) * _cellSize.height);
            snapshot.sourceIndices.resize(static_cast<size_t>(writePixelWidth) * _cellSize.height, NoSourceIndex);
            snapshot.columns.resize(static_cast<size_t>(writeCellCount));

            const auto srcOwnerOffset = static_cast<size_t>(srcUsedBegin - srcSlice._columnBegin);
            for (auto i = 0; i < writeCellCount; ++i)
            {
                if (srcOwnerOffset + i < srcLayer.columns.size())
                {
                    til::at(snapshot.columns, i) = til::at(srcLayer.columns, srcOwnerOffset + i);
                }
            }

            const auto srcPixelOffset = (srcUsedBegin - srcSlice._columnBegin) * _cellSize.width;
            auto srcPixels = std::next(srcLayer.pixels.data(), srcPixelOffset);
            auto srcSourceIndices = std::next(srcLayer.sourceIndices.data(), srcPixelOffset);
            auto dstPixels = snapshot.pixels.data();
            auto dstSourceIndices = snapshot.sourceIndices.data();
            for (auto y = 0; y < _cellSize.height; ++y)
            {
                std::memcpy(dstPixels, srcPixels, static_cast<size_t>(writePixelWidth) * sizeof(RGBQUAD));
                std::memcpy(dstSourceIndices, srcSourceIndices, static_cast<size_t>(writePixelWidth) * sizeof(uint32_t));
                std::advance(srcPixels, srcSlice._pixelWidth);
                std::advance(srcSourceIndices, srcSlice._pixelWidth);
                std::advance(dstPixels, writePixelWidth);
                std::advance(dstSourceIndices, writePixelWidth);
            }
            layerSnapshots.emplace_back(std::move(snapshot));
        }

        auto dstIterator = MutablePixels(dstWriteBegin, dstWriteEnd);
        auto srcIterator = srcSlice.Pixels(srcUsedBegin);
        const auto writeByteCount = sizeof(RGBQUAD) * writeCellCount * _cellSize.width;
        for (auto y = 0; y < _cellSize.height; y++)
        {
            std::memmove(dstIterator, srcIterator, writeByteCount);
            std::advance(srcIterator, srcSlice._pixelWidth);
            std::advance(dstIterator, _pixelWidth);
        }
        // Carry over the legacy plane's ownership separately from z-aware layer
        // coverage. ColumnOwner() returns the visually top layer and would incorrectly
        // attach that Kitty id to co-resident Sixel pixels after a block move.
        std::vector<uint32_t> movedOwners(writeCellCount);
        for (auto i = 0; i < writeCellCount; i++)
        {
            const auto srcOwnerOffset = static_cast<size_t>(srcUsedBegin + i - srcSlice._columnBegin);
            if (srcOwnerOffset < srcSlice._columnOwners.size())
            {
                til::at(movedOwners, i) = til::at(srcSlice._columnOwners, srcOwnerOffset);
            }
        }
        for (auto i = 0; i < writeCellCount; i++)
        {
            const auto dstCol = dstWriteBegin + i;
            SetColumnOwner(dstCol, dstCol + 1, til::at(movedOwners, i));
        }

        // A block copy replaces every Kitty layer in the destination range. Snapshot
        // first (above) so overlapping in-place moves cannot destroy their source.
        for (auto& dstLayer : _kittyLayers)
        {
            _clearLayerColumns(dstLayer, dstWriteBegin, dstWriteEnd);
        }
        for (const auto& snapshot : layerSnapshots)
        {
            auto& dstLayer = _getKittyLayer(snapshot.zIndex, snapshot.imageId, snapshot.placementId);
            const auto dstOwnerOffset = static_cast<size_t>(dstWriteBegin - _columnBegin);
            std::copy(snapshot.columns.begin(), snapshot.columns.end(), dstLayer.columns.begin() + dstOwnerOffset);

            const auto dstPixelOffset = (dstWriteBegin - _columnBegin) * _cellSize.width;
            auto dstPixels = std::next(dstLayer.pixels.data(), dstPixelOffset);
            auto dstSourceIndices = std::next(dstLayer.sourceIndices.data(), dstPixelOffset);
            auto srcPixels = snapshot.pixels.data();
            auto srcSourceIndices = snapshot.sourceIndices.data();
            for (auto y = 0; y < _cellSize.height; ++y)
            {
                std::memcpy(dstPixels, srcPixels, static_cast<size_t>(writePixelWidth) * sizeof(RGBQUAD));
                std::memcpy(dstSourceIndices, srcSourceIndices, static_cast<size_t>(writePixelWidth) * sizeof(uint32_t));
                std::advance(srcPixels, writePixelWidth);
                std::advance(srcSourceIndices, writePixelWidth);
                std::advance(dstPixels, _pixelWidth);
                std::advance(dstSourceIndices, _pixelWidth);
            }
        }
        _removeEmptyKittyLayers();
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
    return !_hasContent();
}

bool ImageSlice::_copyKittyCells(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd, const uint32_t imageId)
try
{
    ImageSlice staged{ *this };
    const auto empty = staged._copyKittyCellsInPlace(srcSlice, srcColumn, dstColumnBegin, dstColumnEnd, imageId);
    if (!empty)
    {
        _swap(staged);
    }
    return empty;
}
catch (const std::bad_alloc&)
{
    LOG_HR(E_OUTOFMEMORY);
    return false;
}

bool ImageSlice::_copyKittyCellsInPlace(const ImageSlice& srcSlice, const til::CoordType srcColumn, const til::CoordType dstColumnBegin, const til::CoordType dstColumnEnd, const uint32_t imageId)
{
    if (imageId == 0)
    {
        return !_hasContent();
    }
    if (_cellSize != srcSlice._cellSize)
    {
        return _eraseKittyCells(dstColumnBegin, dstColumnEnd, imageId);
    }

    struct LayerSnapshot
    {
        int32_t zIndex = 0;
        uint32_t imageId = 0;
        std::vector<RGBQUAD> pixels;
        std::vector<uint8_t> columns;
    };

    const auto cellCount = std::max<til::CoordType>(0, dstColumnEnd - dstColumnBegin);
    const auto pixelWidth = cellCount * _cellSize.width;
    std::vector<LayerSnapshot> snapshots;
    const auto getSnapshot = [&](const int32_t zIndex, const uint32_t imageId) -> LayerSnapshot& {
        const auto it = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& snapshot) {
            return snapshot.zIndex == zIndex && snapshot.imageId == imageId;
        });
        if (it != snapshots.end())
        {
            return *it;
        }

        auto& snapshot = snapshots.emplace_back();
        snapshot.zIndex = zIndex;
        snapshot.imageId = imageId;
        snapshot.pixels.resize(static_cast<size_t>(pixelWidth) * _cellSize.height);
        snapshot.columns.resize(static_cast<size_t>(cellCount));
        return snapshot;
    };

    for (const auto& srcLayer : srcSlice._kittyLayers)
    {
        if (srcLayer.imageId != imageId)
        {
            continue;
        }

        LayerSnapshot* snapshot = nullptr;
        for (auto i = 0; i < cellCount; ++i)
        {
            const auto sourceColumn = srcColumn + i;
            if (sourceColumn < srcSlice._columnBegin || sourceColumn >= srcSlice._columnEnd)
            {
                continue;
            }

            const auto sourceIndex = static_cast<size_t>(sourceColumn - srcSlice._columnBegin);
            if (sourceIndex >= srcLayer.columns.size() || til::at(srcLayer.columns, sourceIndex) == 0)
            {
                continue;
            }

            if (!snapshot)
            {
                snapshot = &getSnapshot(srcLayer.zIndex, srcLayer.imageId);
            }
            til::at(snapshot->columns, i) = 1;

            const auto sourcePixelOffset = (sourceColumn - srcSlice._columnBegin) * _cellSize.width;
            auto source = std::next(srcLayer.pixels.data(), sourcePixelOffset);
            auto destination = std::next(snapshot->pixels.data(), i * _cellSize.width);
            const auto rowByteCount = static_cast<size_t>(_cellSize.width) * sizeof(RGBQUAD);
            for (auto y = 0; y < _cellSize.height; ++y)
            {
                std::memcpy(destination, source, rowByteCount);
                std::advance(source, srcSlice._pixelWidth);
                std::advance(destination, pixelWidth);
            }
        }
    }

    // Older single-plane Kitty cells are promoted to the default z layer. This
    // lets them move without overwriting co-resident unowned Sixel pixels.
    for (auto i = 0; i < cellCount; ++i)
    {
        const auto sourceColumn = srcColumn + i;
        if (sourceColumn < srcSlice._columnBegin || sourceColumn >= srcSlice._columnEnd)
        {
            continue;
        }

        const auto sourceIndex = static_cast<size_t>(sourceColumn - srcSlice._columnBegin);
        const auto owner = sourceIndex < srcSlice._columnOwners.size() ? til::at(srcSlice._columnOwners, sourceIndex) : 0;
        if (owner != imageId)
        {
            continue;
        }

        auto& snapshot = getSnapshot(0, owner);
        til::at(snapshot.columns, i) = 1;
        auto source = srcSlice.Pixels(sourceColumn);
        auto destination = std::next(snapshot.pixels.data(), i * _cellSize.width);
        const auto rowByteCount = static_cast<size_t>(_cellSize.width) * sizeof(RGBQUAD);
        for (auto y = 0; y < _cellSize.height; ++y)
        {
            std::memcpy(destination, source, rowByteCount);
            std::advance(source, srcSlice._pixelWidth);
            std::advance(destination, pixelWidth);
        }
    }

    _eraseKittyCells(dstColumnBegin, dstColumnEnd, imageId);
    if (cellCount > 0 && !snapshots.empty())
    {
        _ensureRange(dstColumnBegin, dstColumnEnd);

        auto available = KittyLayerBytesAvailable();
        for (const auto& snapshot : snapshots)
        {
            const auto required = KittyWriteMemoryUpperBound(dstColumnBegin, dstColumnEnd, snapshot.zIndex, snapshot.imageId);
            if (required > available)
            {
                throw std::bad_alloc{};
            }
            available -= required;
        }

        const auto destinationIndex = static_cast<size_t>(dstColumnBegin - _columnBegin);
        const auto destinationPixelOffset = (dstColumnBegin - _columnBegin) * _cellSize.width;
        for (const auto& snapshot : snapshots)
        {
            auto& layer = _getKittyLayer(snapshot.zIndex, snapshot.imageId);
            std::copy(snapshot.columns.begin(), snapshot.columns.end(), layer.columns.begin() + destinationIndex);

            auto source = snapshot.pixels.data();
            auto destination = std::next(layer.pixels.data(), destinationPixelOffset);
            const auto rowByteCount = static_cast<size_t>(pixelWidth) * sizeof(RGBQUAD);
            for (auto y = 0; y < _cellSize.height; ++y)
            {
                std::memcpy(destination, source, rowByteCount);
                std::advance(source, pixelWidth);
                std::advance(destination, _pixelWidth);
            }
        }
    }

    return !_hasContent();
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

void ImageSlice::_mergePreservedCells(const ImageSlice& srcSlice)
{
    for (auto column = srcSlice._columnBegin; column < srcSlice._columnEnd; ++column)
    {
        const auto sourceIndex = static_cast<size_t>(column - srcSlice._columnBegin);
        const auto sourceOwner = sourceIndex < srcSlice._columnOwners.size() ? til::at(srcSlice._columnOwners, sourceIndex) : 0;
        const auto destinationIndex = column >= _columnBegin && column < _columnEnd ?
                                          static_cast<size_t>(column - _columnBegin) :
                                          _columnOwners.size();
        const auto destinationOwner = destinationIndex < _columnOwners.size() ? til::at(_columnOwners, destinationIndex) : 0;
        if (sourceOwner != 0 ||
            !srcSlice._legacyCellHasPixels(column) ||
            destinationOwner != 0 ||
            _legacyCellHasPixels(column))
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

    const auto hasPreservedLayers = std::any_of(srcSlice._kittyLayers.begin(), srcSlice._kittyLayers.end(), [](const auto& layer) {
        return std::find(layer.columns.begin(), layer.columns.end(), uint8_t{ 1 }) != layer.columns.end();
    });
    if (!hasPreservedLayers)
    {
        return;
    }

    _ensureRange(srcSlice._columnBegin, srcSlice._columnEnd);
    auto available = KittyLayerBytesAvailable();
    for (const auto& srcLayer : srcSlice._kittyLayers)
    {
        if (std::find(srcLayer.columns.begin(), srcLayer.columns.end(), uint8_t{ 1 }) == srcLayer.columns.end())
        {
            continue;
        }

        const auto required = KittyWriteMemoryUpperBound(_columnBegin, _columnEnd, srcLayer.zIndex, srcLayer.imageId);
        if (required > available)
        {
            throw std::bad_alloc{};
        }
        available -= required;
    }

    for (const auto& srcLayer : srcSlice._kittyLayers)
    {
        if (std::find(srcLayer.columns.begin(), srcLayer.columns.end(), uint8_t{ 1 }) == srcLayer.columns.end())
        {
            continue;
        }

        auto& dstLayer = _getKittyLayer(srcLayer.zIndex, srcLayer.imageId);
        for (auto column = srcSlice._columnBegin; column < srcSlice._columnEnd; ++column)
        {
            const auto sourceIndex = static_cast<size_t>(column - srcSlice._columnBegin);
            const auto destinationIndex = static_cast<size_t>(column - _columnBegin);
            if (sourceIndex >= srcLayer.columns.size() ||
                til::at(srcLayer.columns, sourceIndex) == 0 ||
                til::at(dstLayer.columns, destinationIndex) != 0)
            {
                continue;
            }

            til::at(dstLayer.columns, destinationIndex) = 1;
            const auto sourcePixelOffset = (column - srcSlice._columnBegin) * _cellSize.width;
            const auto destinationPixelOffset = (column - _columnBegin) * _cellSize.width;
            auto source = std::next(srcLayer.pixels.data(), sourcePixelOffset);
            auto destination = std::next(dstLayer.pixels.data(), destinationPixelOffset);
            const auto rowByteCount = static_cast<size_t>(_cellSize.width) * sizeof(RGBQUAD);
            for (auto y = 0; y < _cellSize.height; ++y)
            {
                std::memcpy(destination, source, rowByteCount);
                std::advance(source, srcSlice._pixelWidth);
                std::advance(destination, _pixelWidth);
            }
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

void ImageSlice::EraseKittyCells(ROW& row, const til::CoordType columnBegin, const til::CoordType columnEnd, const uint32_t imageId)
{
    const auto imageSlice = row.GetMutableImageSlice();
    if (imageSlice) [[unlikely]]
    {
        const auto scale = row.GetLineRendition() != LineRendition::SingleWidth ? 1 : 0;
        if (imageSlice->_eraseKittyCells(columnBegin << scale, columnEnd << scale, imageId))
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
            for (auto& layer : _kittyLayers)
            {
                _clearLayerColumns(layer, eraseBegin, eraseEnd);
            }
            _removeEmptyKittyLayers();
        }
        return !_hasContent();
    }
}

bool ImageSlice::_eraseKittyCells(const til::CoordType columnBegin, const til::CoordType columnEnd, const uint32_t imageId)
{
    if (imageId == 0)
    {
        return !_hasContent();
    }

    const auto eraseBegin = std::max(columnBegin, _columnBegin);
    const auto eraseEnd = std::min(columnEnd, _columnEnd);
    for (auto column = eraseBegin; column < eraseEnd; ++column)
    {
        const auto index = static_cast<size_t>(column - _columnBegin);
        if (index >= _columnOwners.size() || til::at(_columnOwners, index) != imageId)
        {
            continue;
        }

        til::at(_columnOwners, index) = 0;
        auto iterator = std::next(_pixelBuffer.data(), gsl::narrow_cast<til::CoordType>(index) * _cellSize.width);
        for (auto y = 0; y < _cellSize.height; ++y)
        {
            std::memset(iterator, 0, _cellSize.width * sizeof(RGBQUAD));
            std::advance(iterator, _pixelWidth);
        }
    }

    for (auto& layer : _kittyLayers)
    {
        if (layer.imageId == imageId)
        {
            _clearLayerColumns(layer, eraseBegin, eraseEnd);
        }
    }
    _removeEmptyKittyLayers();
    return !_hasContent();
}

bool ImageSlice::_legacyCellHasPixels(const til::CoordType column) const noexcept
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
