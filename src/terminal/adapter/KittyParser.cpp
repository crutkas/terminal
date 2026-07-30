// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "KittyParser.hpp"
#include "adaptDispatch.hpp"
#include "../../types/inc/Viewport.hpp"
#include "../parser/ascii.hpp"

// inflatelib (microsoft/inflatelib) provides the RFC 1951 DEFLATE decoder backing the
// Kitty graphics o=z (zlib) path. It handles raw Deflate only, so the surrounding RFC
// 1950 zlib header and Adler-32 trailer are validated here.
#include <inflatelib.hpp>

using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::VirtualTerminal;

KittyParser::KittyParser(AdaptDispatch& dispatcher) noexcept :
    _dispatcher{ dispatcher }
{
}

void KittyParser::HardReset() noexcept
{
    _clearImages();
    _mainBufferState.reset();
    _clearChunk();
}

void KittyParser::ErasePlacements()
{
    _deleteAllPlacements(false);
}

void KittyParser::SaveMainBufferState() noexcept
{
    _mainBufferState.emplace(_takeBufferState());
    _clearChunk();
}

void KittyParser::DiscardBufferState() noexcept
{
    _clearImages();
    _clearChunk();
}

void KittyParser::RestoreMainBufferState() noexcept
{
    if (_mainBufferState)
    {
        auto state = std::move(*_mainBufferState);
        _mainBufferState.reset();
        _restoreBufferState(std::move(state));
    }
}

// Handles the Kitty graphics protocol (APC G <control>;<payload> ST). The parser
// has already consumed the 'G' identifier and routed us here on the strength of it.
ITermDispatch::StringHandler KittyParser::DefineImage()
{
    return [this, control = std::wstring{}, payload = std::string{}, inControl = true, controlValid = true, payloadValid = true, payloadTooLarge = false](const auto ch) mutable noexcept -> bool {
        try
        {
            if (ch == AsciiChars::CAN || ch == AsciiChars::SUB)
            {
                _clearChunk();
                return false;
            }
            if (ch == AsciiChars::ESC)
            {
                _HandleSequence(control, payload, controlValid, payloadValid, payloadTooLarge);
                return false;
            }
            if (inControl && ch == L';')
            {
                inControl = false;
                return true;
            }
            if (inControl)
            {
                if (control.size() < MaxControl)
                {
                    control.push_back(ch);
                }
                else
                {
                    controlValid = false;
                }
            }
            else if (ch > 0x7F)
            {
                payloadValid = false;
            }
            else if (payload.size() < MaxPayload)
            {
                payload.push_back(static_cast<char>(ch));
            }
            else
            {
                payloadValid = false;
                payloadTooLarge = true;
            }
            return true;
        }
        catch (const std::bad_alloc&)
        {
            LOG_HR(E_OUTOFMEMORY);
            return false;
        }
        catch (...)
        {
            return false;
        }
    };
}

KittyParser::Control KittyParser::_ParseControl(const std::wstring_view control) noexcept
{
    Control c;
    size_t pos = 0;
    while (pos <= control.size())
    {
        const auto comma = control.find(L',', pos);
        const auto end = comma == std::wstring_view::npos ? control.size() : comma;
        const auto pair = control.substr(pos, end - pos);
        const auto eq = pair.find(L'=');
        if (eq == 1 && pair.size() > 2)
        {
            const auto key = pair.front();
            const auto value = pair.substr(eq + 1);
            if (key != L'm' && key != L'q')
            {
                c.hasNonChunkKey = true;
            }
            if (key != L'm' && key != L'q' && key != L'a')
            {
                c.hasNonChunkKeyOtherThanAction = true;
            }
            switch (key)
            {
            case L'a':
                c.action = value.front();
                break;
            case L'd':
                c.deleteTarget = value.front();
                break;
            case L'i':
                c.imageId = _ParseUint(value);
                c.haveId = true;
                break;
            case L'I':
                c.imageNumber = _ParseUint(value);
                c.haveNumber = true;
                break;
            case L'q':
                c.quiet = _ParseUint(value);
                break;
            case L'f':
                c.format = _ParseUint(value);
                break;
            case L's':
                c.width = _ParseUint(value);
                break;
            case L'v':
                c.height = _ParseUint(value);
                break;
            case L'o':
                c.compression = value.front();
                break;
            case L't':
                c.medium = value.front();
                break;
            case L'm':
                c.moreChunks = _ParseUint(value) != 0;
                c.mPresent = true;
                break;
            default:
                break;
            }
        }
        if (comma == std::wstring_view::npos)
        {
            break;
        }
        pos = comma + 1;
    }

    c.haveId = c.haveId && c.imageId != 0;
    c.haveNumber = c.haveNumber && c.imageNumber != 0;
    return c;
}

void KittyParser::_HandleSequence(const std::wstring_view control, const std::string_view payload, const bool controlValid, const bool payloadValid, const bool payloadTooLarge)
{
    const auto command = _ParseControl(control);

    if (!controlValid)
    {
        _clearChunk();
        if (command.quiet < 2)
        {
            _dispatcher._ReturnApcResponse(L"G;EINVAL:control block too long");
        }
        return;
    }

    const auto repeatsActiveAction = _chunkActive &&
                                     command.action == _chunkControl.action &&
                                     !command.hasNonChunkKeyOtherThanAction;
    const auto isContinuation = command.mPresent && (!command.hasNonChunkKey || repeatsActiveAction);
    if (_chunkActive && !isContinuation)
    {
        _clearChunk();
    }

    if (_chunkActive || command.moreChunks)
    {
        if (!_chunkActive)
        {
            _chunkActive = true;
            _chunkControl = command;
            _chunkPayload.clear();
            _chunkPayloadValid = true;
            _chunkPayloadTooLarge = false;
        }

        _chunkPayloadValid = _chunkPayloadValid && payloadValid;
        _chunkPayloadTooLarge = _chunkPayloadTooLarge || payloadTooLarge;
        if (command.quiet != 0)
        {
            _chunkControl.quiet = command.quiet;
        }
        if (_chunkPayload.size() + payload.size() > MaxPayload)
        {
            _chunkPayloadValid = false;
            _chunkPayloadTooLarge = true;
        }
        else
        {
            _chunkPayload.append(payload);
        }

        if (command.moreChunks)
        {
            return;
        }

        auto finalControl = _chunkControl;
        finalControl.moreChunks = false;
        const auto finalPayload = std::move(_chunkPayload);
        const auto finalValid = _chunkPayloadValid;
        const auto finalTooLarge = _chunkPayloadTooLarge;
        _clearChunk();
        _ProcessCommand(finalControl, finalPayload, finalValid, finalTooLarge);
        return;
    }

    _ProcessCommand(command, payload, payloadValid, payloadTooLarge);
}

bool KittyParser::_localMediaAllowed() const noexcept
{
    return _dispatcher._optionalFeatures.test(ITermDispatch::OptionalFeature::KittyLocalMedia);
}

void KittyParser::_ProcessCommand(const Control& command, const std::string_view payload, const bool payloadValid, const bool payloadTooLarge)
{
    const auto action = command.action;
    const auto deleteTarget = command.deleteTarget;
    const auto imageId = command.imageId;
    const auto imageNumber = command.imageNumber;
    const auto quiet = command.quiet;
    const auto format = command.format;
    const auto width = command.width;
    const auto height = command.height;
    const auto compression = command.compression;
    const auto haveId = command.haveId;
    const auto haveNumber = command.haveNumber;
    const auto medium = command.medium;

    auto success = true;
    std::wstring_view code = L"OK";
    auto assignedId = imageId;

    const auto displayKittyPlacement = [&](const uint32_t targetImageId, const Image& image) {
        auto layerId = _nextLayerId++;
        if (layerId == 0)
        {
            layerId = _nextLayerId++;
        }
        try
        {
            _placeImage(image, true, targetImageId, layerId);
        }
        catch (const std::bad_alloc&)
        {
            success = false;
            code = L"ENOMEM:image layer memory limit exceeded";
        }
    };

    if (haveId && haveNumber)
    {
        success = false;
        code = L"EINVAL:i and I are mutually exclusive";
    }
    else
    {
        switch (action)
        {
        case L't':
        case L'T':
        case L'q':
        {
            std::vector<uint8_t> bytes;
            if (format != 24 && format != 32 && format != 100)
            {
                success = false;
                code = L"EINVAL:unsupported format";
                break;
            }
            if (medium != L'd')
            {
                // The final capability check is present, but local transports are not
                // wired until their dedicated layer. No path or mapping access occurs here.
                static_cast<void>(_localMediaAllowed());
                success = false;
                code = L"EINVAL:unsupported transmission medium";
                break;
            }
            if (compression != 0 && compression != L'z')
            {
                success = false;
                code = L"EINVAL:unsupported compression";
                break;
            }
            if (!payloadValid || !_DecodeBase64(payload, bytes))
            {
                success = false;
                code = payloadTooLarge ? L"EFBIG:payload exceeds maximum size" : L"EINVAL:bad payload";
                break;
            }
            if (compression == L'z')
            {
                std::vector<uint8_t> inflated;
                if (!_inflateZlib(bytes, inflated, MaxPayload))
                {
                    success = false;
                    code = L"EINVAL:invalid compressed data";
                    break;
                }
                bytes = std::move(inflated);
            }

            const auto depth = format == 24 ? 3u : (format == 32 ? 4u : 0u);
            const auto directPixels = depth != 0;
            if (directPixels)
            {
                if (width == 0 || height == 0)
                {
                    success = false;
                    code = L"EINVAL:missing dimensions";
                    break;
                }
                if (width > static_cast<uint32_t>(::Image::MaximumDimension) ||
                    height > static_cast<uint32_t>(::Image::MaximumDimension))
                {
                    success = false;
                    code = L"EFBIG:image dimensions exceed renderer limit";
                    break;
                }
                const auto area = static_cast<uint64_t>(width) * height;
                if (area > bytes.size() / depth || area * depth != bytes.size())
                {
                    success = false;
                    code = L"EINVAL:payload size mismatch";
                    break;
                }
            }

            if (action != L'q')
            {
                Image image;
                image.number = haveNumber ? imageNumber : 0;
                if (directPixels)
                {
                    image.width = width;
                    image.height = height;
                    image.pixels = std::make_shared<std::vector<RGBQUAD>>(_decodePixels(format, bytes));
                }
                else
                {
                    std::vector<RGBQUAD> decoded;
                    til::size decodedSize;
                    const auto decodedSuccessfully =
                        !bytes.empty() &&
                        _dispatcher._api.DecodeImageToBgra(bytes, decoded, decodedSize) &&
                        decodedSize.width > 0 && decodedSize.height > 0 &&
                        decoded.size() == static_cast<size_t>(decodedSize.width) * decodedSize.height;
                    if (decodedSuccessfully &&
                        (decodedSize.width > ::Image::MaximumDimension || decodedSize.height > ::Image::MaximumDimension))
                    {
                        success = false;
                        code = L"EFBIG:image dimensions exceed renderer limit";
                        break;
                    }
                    if (!decodedSuccessfully)
                    {
                        success = false;
                        code = L"EBADPNG:could not decode image";
                        break;
                    }
                    image.width = static_cast<uint32_t>(decodedSize.width);
                    image.height = static_cast<uint32_t>(decodedSize.height);
                    image.pixels = std::make_shared<std::vector<RGBQUAD>>(std::move(decoded));
                }

                assignedId = haveId ? imageId : _assignImageId();
                if (!_registerImage(assignedId, std::move(image)))
                {
                    success = false;
                    code = L"ENOSPC:image storage limit exceeded";
                    break;
                }
                if (action == L'T')
                {
                    const auto stored = _images.find(assignedId);
                    if (stored != _images.end())
                    {
                        displayKittyPlacement(assignedId, stored->second);
                    }
                }
            }
            break;
        }
        case L'p':
        {
            const Image* target = nullptr;
            uint32_t targetId = 0;
            if (haveId)
            {
                const auto it = _images.find(imageId);
                if (it != _images.end())
                {
                    target = &it->second;
                    targetId = imageId;
                }
            }
            else if (haveNumber)
            {
                const auto reverse = _imageNumbers.find(imageNumber);
                if (reverse != _imageNumbers.end())
                {
                    const auto it = _images.find(reverse->second);
                    if (it != _images.end())
                    {
                        target = &it->second;
                        targetId = reverse->second;
                    }
                }
            }
            if (!target)
            {
                success = false;
                code = L"ENOENT:image not found";
            }
            else
            {
                displayKittyPlacement(targetId, *target);
            }
            break;
        }
        case L'd':
        {
            const auto freeData = deleteTarget >= L'A' && deleteTarget <= L'Z';
            switch (deleteTarget)
            {
            case L'a':
            case L'A':
                _deleteAllPlacements(freeData);
                break;
            case L'i':
            case L'I':
                if (haveId)
                {
                    _eraseImagePlacements(imageId);
                    if (freeData)
                    {
                        _eraseImage(imageId);
                    }
                }
                else
                {
                    success = false;
                    code = L"EINVAL:delete by id requires i";
                }
                break;
            case L'n':
            case L'N':
                if (haveNumber)
                {
                    const auto it = _imageNumbers.find(imageNumber);
                    if (it != _imageNumbers.end())
                    {
                        const auto targetId = it->second;
                        _eraseImagePlacements(targetId);
                        if (freeData)
                        {
                            _eraseImage(targetId);
                        }
                    }
                }
                else
                {
                    success = false;
                    code = L"EINVAL:delete by number requires I";
                }
                break;
            default:
                success = false;
                code = L"EINVAL:unsupported delete target";
                break;
            }
            break;
        }
        default:
            success = false;
            code = L"EINVAL:unknown action";
            break;
        }
    }

    if (success && quiet >= 1)
    {
        return;
    }
    if (!success && quiet >= 2)
    {
        return;
    }
    if (success && !haveId && !haveNumber && action != L'q')
    {
        return;
    }

    std::wstring response = L"G";
    if (haveId)
    {
        response += fmt::format(FMT_COMPILE(L"i={}"), imageId);
    }
    else if (haveNumber)
    {
        if (assignedId != 0)
        {
            response += fmt::format(FMT_COMPILE(L"i={},I={}"), assignedId, imageNumber);
        }
        else
        {
            response += fmt::format(FMT_COMPILE(L"I={}"), imageNumber);
        }
    }
    response.push_back(L';');
    response.append(code);
    _dispatcher._ReturnApcResponse(response);
}

uint32_t KittyParser::_ParseUint(const std::wstring_view value) noexcept
{
    uint64_t result = 0;
    for (const auto ch : value)
    {
        if (ch < L'0' || ch > L'9')
        {
            break;
        }
        result = result * 10 + static_cast<uint64_t>(ch - L'0');
        if (result > 0xFFFFFFFF)
        {
            result = 0xFFFFFFFF;
            break;
        }
    }
    return static_cast<uint32_t>(result);
}

uint32_t KittyParser::_assignImageId()
{
    while (_images.find(_nextImageId) != _images.end() || _nextImageId == 0)
    {
        ++_nextImageId;
    }
    return _nextImageId++;
}

bool KittyParser::_registerImage(const uint32_t id, Image&& image)
{
    const auto number = image.number;
    const auto newBytes = image.PixelBytes();
    const auto existing = _images.find(id);
    const auto existingBytes = existing != _images.end() ? existing->second.PixelBytes() : size_t{ 0 };
    const auto retainedBytes = _mainBufferState ? _mainBufferState->totalPixelBytes : size_t{ 0 };
    auto projectedBytes = retainedBytes + (_totalPixelBytes - existingBytes) + newBytes;
    auto projectedCount = _images.size() - (existing != _images.end() ? 1 : 0) + 1;
    std::vector<uint32_t> victims;
    for (const auto candidateId : _imageOrder)
    {
        if (projectedBytes <= MaxTotalBytes && projectedCount <= MaxImages)
        {
            break;
        }
        if (candidateId == id)
        {
            continue;
        }
        const auto candidate = _images.find(candidateId);
        if (candidate != _images.end())
        {
            projectedBytes -= candidate->second.PixelBytes();
            --projectedCount;
            victims.push_back(candidateId);
        }
    }
    if (projectedBytes > MaxTotalBytes || projectedCount > MaxImages)
    {
        return false;
    }

    for (const auto victimId : victims)
    {
        _eraseImagePlacements(victimId);
        _eraseImage(victimId);
    }

    _eraseImagePlacements(id);
    _eraseImage(id);
    _imageOrder.push_back(id);
    _images[id] = std::move(image);
    _totalPixelBytes += newBytes;
    if (number != 0)
    {
        _imageNumbers[number] = id;
    }
    return true;
}

void KittyParser::_eraseImage(const uint32_t id)
{
    const auto it = _images.find(id);
    if (it == _images.end())
    {
        return;
    }
    _totalPixelBytes -= it->second.PixelBytes();
    if (it->second.number != 0)
    {
        const auto reverse = _imageNumbers.find(it->second.number);
        if (reverse != _imageNumbers.end() && reverse->second == id)
        {
            _imageNumbers.erase(reverse);
        }
    }
    _images.erase(it);
    if (!_imageOrder.empty() && _imageOrder.front() == id)
    {
        _imageOrder.pop_front();
    }
    else
    {
        _imageOrder.erase(std::remove(_imageOrder.begin(), _imageOrder.end(), id), _imageOrder.end());
    }
}

void KittyParser::_clearImages() noexcept
try
{
    _images.clear();
    _imageNumbers.clear();
    _imageOrder.clear();
    _totalPixelBytes = 0;
    const auto visiblePageNumber = _dispatcher._pages.VisiblePage().Number();
    _dispatcher._pages.ForEachPage([&](const Page page) {
        auto& buffer = page.Buffer();
        const auto removed = buffer.GetMutableImages().EraseProtocol(ImagePlacement::Key::Protocol::Kitty);
        if (removed != 0 && page.Number() == visiblePageNumber)
        {
            buffer.TriggerRedraw(Viewport::FromExclusive({ 0, 0, page.Width(), page.Bottom() }));
        }
    });
}
catch (...)
{
}

KittyParser::BufferState KittyParser::_takeBufferState() noexcept
{
    BufferState state;
    state.nextImageId = std::exchange(_nextImageId, 1);
    state.nextLayerId = std::exchange(_nextLayerId, 1);
    state.totalPixelBytes = std::exchange(_totalPixelBytes, 0);
    state.images = std::move(_images);
    state.imageNumbers = std::move(_imageNumbers);
    state.imageOrder = std::move(_imageOrder);
    _images.clear();
    _imageNumbers.clear();
    _imageOrder.clear();
    return state;
}

void KittyParser::_restoreBufferState(BufferState&& state) noexcept
{
    _nextImageId = state.nextImageId;
    _nextLayerId = state.nextLayerId;
    _totalPixelBytes = state.totalPixelBytes;
    _images = std::move(state.images);
    _imageNumbers = std::move(state.imageNumbers);
    _imageOrder = std::move(state.imageOrder);
}

size_t KittyParser::_retainedPixelBytes() const noexcept
{
    const auto retained = _mainBufferState ? _mainBufferState->totalPixelBytes : size_t{ 0 };
    return _totalPixelBytes > SIZE_MAX - retained ? SIZE_MAX : _totalPixelBytes + retained;
}

void KittyParser::_clearChunk() noexcept
{
    _chunkActive = false;
    _chunkPayloadValid = true;
    _chunkPayloadTooLarge = false;
    _chunkControl = {};
    _chunkPayload = {};
}

std::vector<RGBQUAD> KittyParser::_decodePixels(const uint32_t format, const std::vector<uint8_t>& bytes)
{
    std::vector<RGBQUAD> pixels;
    const auto depth = format == 24 ? 3u : (format == 32 ? 4u : 0u);
    if (depth == 0 || bytes.empty())
    {
        return pixels;
    }
    const auto count = bytes.size() / depth;
    pixels.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const auto base = i * depth;
        RGBQUAD pixel{};
        if (depth == 4)
        {
            const uint32_t alpha = bytes[base + 3];
            pixel.rgbRed = static_cast<BYTE>(bytes[base] * alpha / 255);
            pixel.rgbGreen = static_cast<BYTE>(bytes[base + 1] * alpha / 255);
            pixel.rgbBlue = static_cast<BYTE>(bytes[base + 2] * alpha / 255);
            pixel.rgbReserved = static_cast<BYTE>(alpha);
        }
        else
        {
            pixel.rgbRed = bytes[base];
            pixel.rgbGreen = bytes[base + 1];
            pixel.rgbBlue = bytes[base + 2];
            pixel.rgbReserved = 255;
        }
        pixels.push_back(pixel);
    }
    return pixels;
}

til::size KittyParser::_placeImage(const Image& image, const bool moveCursor, const uint32_t imageId, const uint64_t layerId)
{
    if (!image.pixels || image.pixels->empty() || image.width == 0 || image.height == 0)
    {
        return {};
    }

    const auto page = _dispatcher._pages.ActivePage();
    auto& buffer = page.Buffer();
    const auto origin = page.Cursor().GetPosition();
    const auto cellSize = _dispatcher._api.GetCellSize();
    const auto cellWidth = std::max(1, cellSize.width);
    const auto cellHeight = std::max(1, cellSize.height);
    const til::size clampedCellSize{ cellWidth, cellHeight };
    const auto imageWidth = static_cast<til::CoordType>(image.width);
    const auto imageHeight = static_cast<til::CoordType>(image.height);

    const auto columnBegin = origin.x;
    const auto columns = static_cast<til::CoordType>(std::min<int64_t>(
        (static_cast<int64_t>(imageWidth) + cellWidth - 1) / cellWidth,
        page.Width()));
    const auto columnEnd = std::min(columnBegin + columns, page.Width());
    if (columnEnd <= columnBegin)
    {
        return {};
    }

    const auto rowSpan = static_cast<til::CoordType>(std::min<int64_t>(
        (static_cast<int64_t>(imageHeight) + cellHeight - 1) / cellHeight,
        page.Bottom()));
    const auto redrawTop = std::max(0, origin.y);
    const auto redrawBottom = std::min(origin.y + rowSpan, page.Bottom());
    const auto drawnColumns = columnEnd - columnBegin;
    const auto drawnRows = std::max(0, redrawBottom - origin.y);
    if (drawnColumns > 0 && drawnRows > 0)
    {
        auto surface = image.surface;
        if (!surface)
        {
            surface = std::make_shared<::Image>(til::size{ imageWidth, imageHeight }, image.pixels);
            image.surface = surface;
        }

        const ImagePlacement::Key key{ imageId, layerId, ImagePlacement::Key::Protocol::Kitty };
        buffer.GetMutableImages().AddOrReplace(ImagePlacement{
            key,
            std::move(surface),
            { columnBegin, origin.y, columnEnd, redrawBottom },
            0,
            { 0, 0, imageWidth, imageHeight },
            {
                .cellSize = clampedCellSize,
                .targetWidth = image.width,
                .targetHeight = image.height,
                .offset = {},
            },
        });
    }
    buffer.TriggerRedraw(Viewport::FromExclusive({ 0, redrawTop, page.Width(), redrawBottom }));

    if (moveCursor)
    {
        page.Cursor().SetPosition({
            std::min(columnEnd, page.Width() - 1),
            std::min(origin.y + rowSpan, page.Bottom() - 1),
        });
    }
    return { drawnColumns, drawnRows };
}

void KittyParser::_eraseImagePlacements(const uint32_t imageId)
{
    if (imageId == 0)
    {
        return;
    }
    const auto visiblePageNumber = _dispatcher._pages.VisiblePage().Number();
    _dispatcher._pages.ForEachPage([&](const Page page) {
        auto& buffer = page.Buffer();
        auto firstRow = page.Bottom();
        auto lastRow = 0;
        for (const auto& placement : buffer.GetImages().All())
        {
            const auto key = placement.Identity();
            if (key.protocol == ImagePlacement::Key::Protocol::Kitty && key.imageId == imageId)
            {
                firstRow = std::min(firstRow, placement.CellBounds().top);
                lastRow = std::max(lastRow, placement.CellBounds().bottom - 1);
            }
        }
        buffer.GetMutableImages().EraseImage(ImagePlacement::Key::Protocol::Kitty, imageId);
        if (page.Number() == visiblePageNumber && firstRow <= lastRow)
        {
            buffer.TriggerRedraw(Viewport::FromExclusive({ 0, firstRow, page.Width(), lastRow + 1 }));
        }
    });
    if (const auto image = _images.find(imageId); image != _images.end())
    {
        image->second.surface.reset();
    }
}

void KittyParser::_deleteAllPlacements(const bool freeData)
{
    if (freeData)
    {
        _clearImages();
        return;
    }

    const auto visiblePageNumber = _dispatcher._pages.VisiblePage().Number();
    _dispatcher._pages.ForEachPage([&](const Page page) {
        auto& buffer = page.Buffer();
        const auto removed = buffer.GetMutableImages().EraseProtocol(ImagePlacement::Key::Protocol::Kitty);
        if (removed != 0 && page.Number() == visiblePageNumber)
        {
            buffer.TriggerRedraw(Viewport::FromExclusive({ 0, 0, page.Width(), page.Bottom() }));
        }
    });
    for (auto& [id, image] : _images)
    {
        static_cast<void>(id);
        image.surface.reset();
    }
}

bool KittyParser::_DecodeBase64(const std::string_view input, std::vector<uint8_t>& output) noexcept
{
    output.clear();
    if (input.size() % 4 != 0)
    {
        return false;
    }

    const auto sextet = [](const char ch) noexcept -> int {
        if (ch >= 'A' && ch <= 'Z')
        {
            return ch - 'A';
        }
        if (ch >= 'a' && ch <= 'z')
        {
            return ch - 'a' + 26;
        }
        if (ch >= '0' && ch <= '9')
        {
            return ch - '0' + 52;
        }
        if (ch == '+')
        {
            return 62;
        }
        if (ch == '/')
        {
            return 63;
        }
        return -1;
    };

    try
    {
        output.reserve(input.size() / 4 * 3);
    }
    catch (...)
    {
        return false;
    }

    for (size_t i = 0; i < input.size(); i += 4)
    {
        const auto ch2 = input[i + 2];
        const auto ch3 = input[i + 3];
        const auto value0 = sextet(input[i]);
        const auto value1 = sextet(input[i + 1]);
        if (value0 < 0 || value1 < 0)
        {
            return false;
        }
        if (ch2 == '=')
        {
            if (ch3 != '=' || i + 4 != input.size())
            {
                return false;
            }
            output.push_back(static_cast<uint8_t>((value0 << 2) | (value1 >> 4)));
            break;
        }
        const auto value2 = sextet(ch2);
        if (value2 < 0)
        {
            return false;
        }
        if (ch3 == '=')
        {
            if (i + 4 != input.size())
            {
                return false;
            }
            output.push_back(static_cast<uint8_t>((value0 << 2) | (value1 >> 4)));
            output.push_back(static_cast<uint8_t>((value1 << 4) | (value2 >> 2)));
            break;
        }
        const auto value3 = sextet(ch3);
        if (value3 < 0)
        {
            return false;
        }
        output.push_back(static_cast<uint8_t>((value0 << 2) | (value1 >> 4)));
        output.push_back(static_cast<uint8_t>((value1 << 4) | (value2 >> 2)));
        output.push_back(static_cast<uint8_t>((value2 << 6) | value3));
    }

    return true;
}

bool KittyParser::_inflateZlib(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, const size_t cap) noexcept
try
{
    output.clear();
    if (input.size() < 6)
    {
        return false;
    }
    const auto cmf = input[0];
    const auto flg = input[1];
    if (((static_cast<unsigned>(cmf) << 8) | flg) % 31u != 0)
    {
        return false;
    }
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7)
    {
        return false;
    }
    if ((flg & 0x20) != 0)
    {
        return false;
    }

    const auto deflateAvailable = input.size() - 2u;
    std::span<const std::byte> remainingInput{ reinterpret_cast<const std::byte*>(input.data() + 2), deflateAvailable };
    const auto limit = cap + 1;
    constexpr size_t firstChunk = 8 * 1024;

    inflatelib::stream stream;
    size_t produced = 0;
    auto reachedEnd = false;
    while (!reachedEnd)
    {
        if (produced == output.size())
        {
            if (output.size() >= limit)
            {
                break;
            }
            output.resize(std::min(limit, std::max(firstChunk, output.size() * 2)));
        }

        std::span<std::byte> outputWindow{ reinterpret_cast<std::byte*>(output.data() + produced), output.size() - produced };
        const auto inputBefore = remainingInput.size();
        const auto outputBefore = outputWindow.size();
        const auto result = stream.try_inflate(remainingInput, outputWindow);
        if (result < INFLATELIB_OK)
        {
            output.clear();
            return false;
        }

        produced += outputBefore - outputWindow.size();
        reachedEnd = result == INFLATELIB_EOF;
        if (!reachedEnd && remainingInput.size() == inputBefore && outputWindow.size() == outputBefore)
        {
            output.clear();
            return false;
        }
    }

    if (!reachedEnd || produced == 0 || produced > cap)
    {
        output.clear();
        return false;
    }
    output.resize(produced);

    const auto consumed = deflateAvailable - remainingInput.size();
    const auto streamEnd = 2u + consumed + 4u;
    if (streamEnd != input.size())
    {
        output.clear();
        return false;
    }

    const auto adler32 = [](const uint8_t* data, size_t size) noexcept -> uint32_t {
        uint32_t a = 1;
        uint32_t b = 0;
        while (size != 0)
        {
            auto count = size < 5552u ? size : size_t{ 5552 };
            size -= count;
            do
            {
                a += *data++;
                b += a;
            } while (--count != 0);
            a %= 65521u;
            b %= 65521u;
        }
        return (b << 16) | a;
    };
    const auto* const tail = input.data() + 2u + consumed;
    const auto expected = (static_cast<uint32_t>(tail[0]) << 24) |
                          (static_cast<uint32_t>(tail[1]) << 16) |
                          (static_cast<uint32_t>(tail[2]) << 8) |
                          static_cast<uint32_t>(tail[3]);
    if (adler32(output.data(), output.size()) != expected)
    {
        output.clear();
        return false;
    }

    return true;
}
catch (...)
{
    output.clear();
    return false;
}
