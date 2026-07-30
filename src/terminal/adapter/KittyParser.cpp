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
            case L'c':
                c.cols = _ParseUint(value);
                break;
            case L'r':
                c.rows = _ParseUint(value);
                break;
            case L'x':
                c.srcX = _ParseUint(value);
                break;
            case L'y':
                c.srcY = _ParseUint(value);
                break;
            case L'w':
                c.srcW = _ParseUint(value);
                break;
            case L'h':
                c.srcH = _ParseUint(value);
                break;
            case L'X':
                c.cellOffsetX = _ParseUint(value);
                break;
            case L'Y':
                c.cellOffsetY = _ParseUint(value);
                break;
            case L'o':
                c.compression = value.front();
                break;
            case L't':
                c.medium = value.front();
                break;
            case L'C':
                c.noCursorMovement = _ParseUint(value) != 0;
                break;
            case L'p':
                c.placementId = _ParseUint(value);
                c.havePlacementId = true;
                break;
            case L'P':
                c.parentImageId = _ParseUint(value);
                c.haveParent = true;
                break;
            case L'Q':
                c.parentPlacementId = _ParseUint(value);
                break;
            case L'H':
                c.offsetH = _ParseInt(value);
                break;
            case L'V':
                c.offsetV = _ParseInt(value);
                break;
            case L'z':
                c.zIndex = _ParseInt(value);
                c.haveZ = true;
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
    c.havePlacementId = c.havePlacementId && c.placementId != 0;
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
    const auto moveCursor = !command.noCursorMovement;

    auto success = true;
    std::wstring_view code = L"OK";
    auto assignedId = imageId;

    const auto displayKittyPlacement = [&](const uint32_t targetImageId, const Image& image) {
        const auto placementId = command.havePlacementId ? command.placementId : 0u;
        auto layerId = _nextLayerId++;
        if (layerId == 0)
        {
            layerId = _nextLayerId++;
        }

        auto priorPlacement = std::optional<Placement>{};
        if (placementId != 0)
        {
            const auto existing = _placements.find({ targetImageId, placementId });
            if (existing != _placements.end())
            {
                priorPlacement = existing->second;
            }
        }
        const auto priorAnchor = priorPlacement ? _derivePlacementAnchor(*priorPlacement) : std::nullopt;
        const auto removePriorPlacement = [&]() {
            if (priorPlacement)
            {
                _erasePlacementCells(*priorPlacement);
            }
        };
        const auto registerPlacement = [&](const Placement& placement) {
            if (placement.placementId != 0)
            {
                _registerPlacement(placement);
                return;
            }
            while (_anonymousPlacements.size() >= MaxPlacements)
            {
                _erasePlacementCells(_anonymousPlacements.front());
                _anonymousPlacements.erase(_anonymousPlacements.begin());
            }
            _anonymousPlacements.push_back(placement);
        };
        if (command.haveParent)
        {
            std::wstring_view resolveCode = L"OK";
            const auto parentAnchor = _resolvePlacementAnchor(command.parentImageId, command.parentPlacementId, { targetImageId, placementId }, resolveCode);
            if (!parentAnchor)
            {
                success = false;
                code = resolveCode;
                return;
            }
            auto page = _dispatcher._pages.ActivePage();
            const auto maxCol = std::max(0, page.Width() - 1);
            const auto maxRow = std::max(0, page.Bottom() - 1);
            const til::point childAnchor{
                static_cast<til::CoordType>(std::clamp<int64_t>(static_cast<int64_t>(parentAnchor->x) + command.offsetH, 0, maxCol)),
                static_cast<til::CoordType>(std::clamp<int64_t>(static_cast<int64_t>(parentAnchor->y) + command.offsetV, 0, maxRow)),
            };
            const auto movesChildren = priorPlacement &&
                                       (!priorAnchor || priorAnchor->x != childAnchor.x || priorAnchor->y != childAnchor.y);
            if (movesChildren &&
                !_movePlacementChildren({ targetImageId, placementId }, childAnchor, false, code))
            {
                success = false;
                return;
            }
            til::size drawn;
            try
            {
                drawn = _placeImage(image,
                                    false,
                                    targetImageId,
                                    layerId,
                                    command.cols,
                                    command.rows,
                                    command.srcX,
                                    command.srcY,
                                    command.srcW,
                                    command.srcH,
                                    command.cellOffsetX,
                                    command.cellOffsetY,
                                    command.zIndex,
                                    childAnchor);
            }
            catch (const std::bad_alloc&)
            {
                success = false;
                code = L"ENOMEM:image layer memory limit exceeded";
                return;
            }
            if (drawn.width <= 0 || drawn.height <= 0)
            {
                return;
            }
            Placement placement;
            placement.imageId = targetImageId;
            placement.placementId = placementId;
            placement.layerId = layerId;
            placement.anchorCol = childAnchor.x;
            placement.anchorRow = childAnchor.y;
            placement.cols = drawn.width;
            placement.rows = drawn.height;
            placement.displayCols = command.cols;
            placement.displayRows = command.rows;
            placement.srcX = command.srcX;
            placement.srcY = command.srcY;
            placement.srcW = command.srcW;
            placement.srcH = command.srcH;
            placement.cellOffsetX = command.cellOffsetX;
            placement.cellOffsetY = command.cellOffsetY;
            placement.parentImageId = command.parentImageId;
            placement.parentPlacementId = command.parentPlacementId;
            placement.offsetH = command.offsetH;
            placement.offsetV = command.offsetV;
            placement.zIndex = command.zIndex;
            placement.hasParent = true;
            registerPlacement(placement);
            if (placementId != 0 && movesChildren &&
                !_movePlacementChildren({ targetImageId, placementId }, childAnchor, true, code))
            {
                _erasePlacementCells(placement);
                _placements[{ targetImageId, placementId }] = *priorPlacement;
                success = false;
                return;
            }
            removePriorPlacement();
            return;
        }

        const auto cursorPos = _dispatcher._pages.ActivePage().Cursor().GetPosition();
        const auto movesChildren = priorPlacement &&
                                   (!priorAnchor || priorAnchor->x != cursorPos.x || priorAnchor->y != cursorPos.y);
        if (movesChildren &&
            !_movePlacementChildren({ targetImageId, placementId }, cursorPos, false, code))
        {
            success = false;
            return;
        }
        til::size drawn;
        try
        {
            drawn = _placeImage(image,
                                moveCursor,
                                targetImageId,
                                layerId,
                                command.cols,
                                command.rows,
                                command.srcX,
                                command.srcY,
                                command.srcW,
                                command.srcH,
                                command.cellOffsetX,
                                command.cellOffsetY,
                                command.zIndex);
        }
        catch (const std::bad_alloc&)
        {
            success = false;
            code = L"ENOMEM:image layer memory limit exceeded";
            return;
        }
        if (drawn.width <= 0 || drawn.height <= 0)
        {
            if (placementId != 0 && priorPlacement)
            {
                _deletePlacement(targetImageId, placementId, false);
            }
            return;
        }

        Placement placement;
        placement.imageId = targetImageId;
        placement.placementId = placementId;
        placement.layerId = layerId;
        placement.anchorCol = cursorPos.x;
        placement.anchorRow = cursorPos.y;
        placement.cols = drawn.width;
        placement.rows = drawn.height;
        placement.displayCols = command.cols;
        placement.displayRows = command.rows;
        placement.srcX = command.srcX;
        placement.srcY = command.srcY;
        placement.srcW = command.srcW;
        placement.srcH = command.srcH;
        placement.cellOffsetX = command.cellOffsetX;
        placement.cellOffsetY = command.cellOffsetY;
        placement.zIndex = command.zIndex;
        placement.hasParent = false;
        registerPlacement(placement);
        if (placementId != 0 && movesChildren &&
            !_movePlacementChildren({ targetImageId, placementId }, cursorPos, true, code))
        {
            _erasePlacementCells(placement);
            _placements[{ targetImageId, placementId }] = *priorPlacement;
            success = false;
            return;
        }
        removePriorPlacement();
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

            std::vector<RGBQUAD> decoded;
            til::size decodedSize;
            if (!directPixels)
            {
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
                if (reverse != _imageNumbers.end() && !reverse->second.empty())
                {
                    const auto targetImageId = reverse->second.back();
                    const auto it = _images.find(targetImageId);
                    if (it != _images.end())
                    {
                        target = &it->second;
                        targetId = targetImageId;
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
                if (success)
                {
                    _touchImage(targetId);
                }
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
                    if (command.havePlacementId)
                    {
                        _deletePlacement(imageId, command.placementId, freeData);
                    }
                    else
                    {
                        _erasePlacementsForImage(imageId);
                        _eraseImagePlacements(imageId);
                        if (freeData)
                        {
                            _eraseImage(imageId);
                        }
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
                    if (it != _imageNumbers.end() && !it->second.empty())
                    {
                        const auto targetId = it->second.back();
                        if (command.havePlacementId)
                        {
                            _deletePlacement(targetId, command.placementId, freeData);
                        }
                        else
                        {
                            _erasePlacementsForImage(targetId);
                            _eraseImagePlacements(targetId);
                            if (freeData)
                            {
                                _eraseImage(targetId);
                            }
                        }
                    }
                }
                else
                {
                    success = false;
                    code = L"EINVAL:delete by number requires I";
                }
                break;
            case L'c':
            case L'C':
            {
                const auto page = _dispatcher._pages.ActivePage();
                const auto cursor = page.Cursor().GetPosition();
                _deleteImagesIntersecting(cursor.x, cursor.y, cursor.x + 1, cursor.y + 1, freeData);
                break;
            }
            case L'p':
            case L'P':
                if (command.srcX != 0 && command.srcY != 0)
                {
                    const auto page = _dispatcher._pages.ActivePage();
                    const auto x = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(command.srcX) - 1, page.Width()));
                    const auto y = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(page.Top()) + command.srcY - 1, page.Bottom()));
                    _deleteImagesIntersecting(x, y, x + 1, y + 1, freeData);
                }
                break;
            case L'x':
            case L'X':
                if (command.srcX != 0)
                {
                    const auto page = _dispatcher._pages.ActivePage();
                    const auto column = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(command.srcX) - 1, page.Width()));
                    _deleteImagesIntersecting(column, page.Top(), column + 1, page.Bottom(), freeData);
                }
                break;
            case L'y':
            case L'Y':
                if (command.srcY != 0)
                {
                    const auto page = _dispatcher._pages.ActivePage();
                    const auto row = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(page.Top()) + command.srcY - 1, page.Bottom()));
                    _deleteImagesIntersecting(0, row, page.Width(), row + 1, freeData);
                }
                break;
            case L'r':
            case L'R':
                _deleteImagesInIdRange(command.srcX, command.srcY, freeData);
                break;
            case L'z':
            case L'Z':
                _deletePlacementsByZ(command.zIndex, freeData);
                break;
            case L'q':
            case L'Q':
                if (command.srcX != 0 && command.srcY != 0)
                {
                    const auto page = _dispatcher._pages.ActivePage();
                    const auto x = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(command.srcX) - 1, page.Width()));
                    const auto y = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(page.Top()) + command.srcY - 1, page.Bottom()));
                    _deletePlacementsByZ(command.zIndex, freeData, til::point{ x, y });
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
    if (command.havePlacementId && (haveId || haveNumber))
    {
        response += fmt::format(FMT_COMPILE(L",p={}"), command.placementId);
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

int32_t KittyParser::_ParseInt(const std::wstring_view value) noexcept
{
    auto negative = false;
    auto digits = value;
    if (!digits.empty() && (digits.front() == L'-' || digits.front() == L'+'))
    {
        negative = digits.front() == L'-';
        digits = digits.substr(1);
    }
    const auto magnitude = _ParseUint(digits);
    if (negative)
    {
        return magnitude >= 0x80000000u ? INT32_MIN : -static_cast<int32_t>(magnitude);
    }
    return magnitude > 0x7FFFFFFFu ? INT32_MAX : static_cast<int32_t>(magnitude);
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
    const auto selectVictims = [&](const bool placed) {
        for (const auto candidateId : _imageOrder)
        {
            if (projectedBytes <= MaxTotalBytes && projectedCount <= MaxImages)
            {
                break;
            }
            if (candidateId == id || _isImagePlaced(candidateId) != placed)
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
    };
    selectVictims(false);
    selectVictims(true);
    if (projectedBytes > MaxTotalBytes || projectedCount > MaxImages)
    {
        return false;
    }

    for (const auto victimId : victims)
    {
        _erasePlacementsForImage(victimId);
        _eraseImagePlacements(victimId);
        _eraseImage(victimId);
    }

    _eraseImagePlacements(id);
    _erasePlacementsForImage(id);
    _eraseImage(id);
    _imageOrder.push_back(id);
    _images[id] = std::move(image);
    _totalPixelBytes += newBytes;
    if (number != 0)
    {
        _imageNumbers[number].push_back(id);
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
        if (reverse != _imageNumbers.end())
        {
            auto& ids = reverse->second;
            ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
            if (ids.empty())
            {
                _imageNumbers.erase(reverse);
            }
        }
    }
    _images.erase(it);
    for (auto placement = _placements.begin(); placement != _placements.end();)
    {
        placement = placement->first.first == id ? _placements.erase(placement) : std::next(placement);
    }
    _anonymousPlacements.erase(
        std::remove_if(_anonymousPlacements.begin(), _anonymousPlacements.end(), [id](const Placement& placement) noexcept {
            return placement.imageId == id;
        }),
        _anonymousPlacements.end());
    if (!_imageOrder.empty() && _imageOrder.front() == id)
    {
        _imageOrder.pop_front();
    }
    else
    {
        _imageOrder.erase(std::remove(_imageOrder.begin(), _imageOrder.end(), id), _imageOrder.end());
    }
}

void KittyParser::_touchImage(const uint32_t id) noexcept
{
    const auto it = std::find(_imageOrder.begin(), _imageOrder.end(), id);
    if (it != _imageOrder.end())
    {
        std::rotate(it, std::next(it), _imageOrder.end());
    }
}

bool KittyParser::_isImagePlaced(const uint32_t id) const
{
    auto found = false;
    _dispatcher._pages.ForEachPage([&](const Page page) {
        if (found)
        {
            return;
        }
        for (const auto& placement : page.Buffer().GetImages().All())
        {
            const auto identity = placement.Identity();
            if (identity.protocol == ImagePlacement::Key::Protocol::Kitty && identity.imageId == id)
            {
                found = true;
                break;
            }
        }
    });
    return found;
}

void KittyParser::_clearImages() noexcept
try
{
    _images.clear();
    _imageNumbers.clear();
    _imageOrder.clear();
    _placements.clear();
    _anonymousPlacements.clear();
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
    state.placements = std::move(_placements);
    state.anonymousPlacements = std::move(_anonymousPlacements);
    _images.clear();
    _imageNumbers.clear();
    _imageOrder.clear();
    _placements.clear();
    _anonymousPlacements.clear();
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
    _placements = std::move(state.placements);
    _anonymousPlacements = std::move(state.anonymousPlacements);
}

size_t KittyParser::_retainedPixelBytes() const noexcept
{
    const auto retained = _mainBufferState ? _mainBufferState->totalPixelBytes : size_t{ 0 };
    return _totalPixelBytes > SIZE_MAX - retained ? SIZE_MAX : _totalPixelBytes + retained;
}

void KittyParser::_releaseImageSurface(Image& image) noexcept
{
    image.surface.reset();
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

til::size KittyParser::_placeImage(const Image& image,
                                   const bool moveCursor,
                                   const uint32_t imageId,
                                   const uint64_t layerId,
                                   const uint32_t cols,
                                   const uint32_t rows,
                                   const uint32_t srcX,
                                   const uint32_t srcY,
                                   const uint32_t srcW,
                                   const uint32_t srcH,
                                   const uint32_t cellOffsetX,
                                   const uint32_t cellOffsetY,
                                   const int32_t zIndex,
                                   const std::optional<til::point> anchor)
{
    if (!image.pixels || image.pixels->empty() || image.width == 0 || image.height == 0)
    {
        return {};
    }

    const auto page = _dispatcher._pages.ActivePage();
    auto& buffer = page.Buffer();
    const auto origin = anchor.has_value() ? *anchor : page.Cursor().GetPosition();
    const auto cellSize = _dispatcher._api.GetCellSize();
    const auto cellWidth = std::max(1, cellSize.width);
    const auto cellHeight = std::max(1, cellSize.height);
    const til::size clampedCellSize{ cellWidth, cellHeight };
    const auto imageWidth = static_cast<til::CoordType>(image.width);
    const auto imageHeight = static_cast<til::CoordType>(image.height);
    const auto offsetX = static_cast<til::CoordType>(std::min<uint32_t>(cellOffsetX, static_cast<uint32_t>(cellWidth - 1)));
    const auto offsetY = static_cast<til::CoordType>(std::min<uint32_t>(cellOffsetY, static_cast<uint32_t>(cellHeight - 1)));

    const auto cropX = static_cast<til::CoordType>(std::min<uint32_t>(srcX, image.width));
    const auto cropY = static_cast<til::CoordType>(std::min<uint32_t>(srcY, image.height));
    const auto cropW = srcW == 0 ? imageWidth - cropX : std::min(static_cast<til::CoordType>(std::min<uint32_t>(srcW, image.width)), imageWidth - cropX);
    const auto cropH = srcH == 0 ? imageHeight - cropY : std::min(static_cast<til::CoordType>(std::min<uint32_t>(srcH, image.height)), imageHeight - cropY);
    if (cropW <= 0 || cropH <= 0)
    {
        return {};
    }

    const auto columnBegin = origin.x;
    const auto [targetW64, targetH64] = _targetPixels(cropW, cropH, cols, rows, cellWidth, cellHeight);
    const auto targetW = std::max<int64_t>(targetW64, 1);
    const auto targetH = std::max<int64_t>(targetH64, 1);
    const auto columns = static_cast<til::CoordType>(std::min<int64_t>((targetW + cellWidth - 1) / cellWidth, page.Width()));
    const auto columnEnd = std::min(columnBegin + columns, page.Width());
    if (columnEnd <= columnBegin)
    {
        return {};
    }

    const auto rowSpan = static_cast<til::CoordType>(std::min<int64_t>((targetH + cellHeight - 1) / cellHeight, page.Bottom()));
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
            zIndex,
            { cropX, cropY, cropX + cropW, cropY + cropH },
            {
                .cellSize = clampedCellSize,
                .targetWidth = gsl::narrow_cast<uint64_t>(targetW),
                .targetHeight = gsl::narrow_cast<uint64_t>(targetH),
                .offset = { offsetX, offsetY },
            },
        });
        if (const auto entry = _images.find(imageId); entry != _images.end())
        {
            entry->second.hasRenderedPlacements = true;
        }
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

KittyParser::TargetSize KittyParser::_targetPixels(const int64_t cropW, const int64_t cropH, const uint32_t cols, const uint32_t rows, const int64_t cellWidth, const int64_t cellHeight) noexcept
{
    constexpr uint32_t maxCells = 8192;
    const int64_t requestedCols = std::min(cols, maxCells);
    const int64_t requestedRows = std::min(rows, maxCells);
    const auto safeCropW = std::max<int64_t>(cropW, 1);
    const auto safeCropH = std::max<int64_t>(cropH, 1);
    if (requestedCols != 0 && requestedRows != 0)
    {
        return { requestedCols * cellWidth, requestedRows * cellHeight };
    }
    if (requestedCols != 0)
    {
        const auto width = requestedCols * cellWidth;
        return { width, safeCropH * width / safeCropW };
    }
    if (requestedRows != 0)
    {
        const auto height = requestedRows * cellHeight;
        return { safeCropW * height / safeCropH, height };
    }
    return { safeCropW, safeCropH };
}

void KittyParser::_registerPlacement(const Placement& placement)
{
    if (placement.imageId == 0 || placement.placementId == 0)
    {
        return;
    }

    const std::pair<uint32_t, uint32_t> key{ placement.imageId, placement.placementId };
    _placements[key] = placement;
    while (_placements.size() > MaxPlacements)
    {
        auto victim = _placements.begin();
        if (victim->first == key && std::next(victim) != _placements.end())
        {
            ++victim;
        }
        const auto victimKey = victim->first;
        const auto victimValue = victim->second;
        _erasePlacementCells(victimValue);
        _placements.erase(victim);
        std::deque<std::pair<uint32_t, uint32_t>> removed{ victimKey };
        _cascadePlacementChildren(removed, 0);
    }
}

bool KittyParser::_movePlacementChildren(const std::pair<uint32_t, uint32_t>& parent, const til::point parentAnchor, const bool apply, std::wstring_view& code)
{
    using PlacementKey = std::pair<uint32_t, uint32_t>;
    struct PendingParent
    {
        PlacementKey key;
        til::point anchor;
        int depth = 0;
    };
    struct MovePlan
    {
        std::optional<PlacementKey> key;
        size_t anonymousIndex = 0;
        Placement previous;
        std::optional<til::point> oldAnchor;
        til::point anchor;
    };

    std::map<PlacementKey, std::vector<PlacementKey>> registeredChildren;
    for (const auto& entry : _placements)
    {
        const auto& child = entry.second;
        if (child.hasParent)
        {
            registeredChildren[{ child.parentImageId, child.parentPlacementId }].push_back(entry.first);
        }
    }
    std::map<PlacementKey, std::vector<size_t>> anonymousChildren;
    for (size_t i = 0; i < _anonymousPlacements.size(); ++i)
    {
        const auto& child = _anonymousPlacements[i];
        if (child.hasParent)
        {
            anonymousChildren[{ child.parentImageId, child.parentPlacementId }].push_back(i);
        }
    }

    auto page = _dispatcher._pages.ActivePage();
    const auto maxCol = std::max(0, page.Width() - 1);
    const auto maxRow = std::max(0, page.Bottom() - 1);
    const auto childAnchorFor = [&](const Placement& child, const til::point anchor) {
        return til::point{
            static_cast<til::CoordType>(std::clamp<int64_t>(static_cast<int64_t>(anchor.x) + child.offsetH, 0, maxCol)),
            static_cast<til::CoordType>(std::clamp<int64_t>(static_cast<int64_t>(anchor.y) + child.offsetV, 0, maxRow)),
        };
    };

    std::deque<PendingParent> pending{ { parent, parentAnchor, 0 } };
    std::set<PlacementKey> visited;
    std::vector<MovePlan> plan;
    while (!pending.empty())
    {
        const auto current = pending.front();
        pending.pop_front();
        if (current.depth > MaxPlacementDepth || !visited.emplace(current.key).second)
        {
            code = current.depth > MaxPlacementDepth ?
                       L"ETOODEEP:relative placement chain too deep" :
                       L"ECYCLE:relative placement cycle";
            return false;
        }
        if (const auto children = registeredChildren.find(current.key); children != registeredChildren.end())
        {
            for (const auto& childKey : children->second)
            {
                const auto childIt = _placements.find(childKey);
                if (childIt == _placements.end())
                {
                    code = L"ENOPARENT:relative child not found";
                    return false;
                }
                const auto& child = childIt->second;
                const auto anchor = childAnchorFor(child, current.anchor);
                const auto oldAnchor = _derivePlacementAnchor(child);
                if (!oldAnchor || *oldAnchor != anchor)
                {
                    plan.push_back({ childKey, 0, child, oldAnchor, anchor });
                }
                pending.push_back({ childKey, anchor, current.depth + 1 });
            }
        }

        if (const auto children = anonymousChildren.find(current.key); children != anonymousChildren.end())
        {
            for (const auto childIndex : children->second)
            {
                const auto& child = _anonymousPlacements[childIndex];
                const auto anchor = childAnchorFor(child, current.anchor);
                const auto oldAnchor = _derivePlacementAnchor(child);
                if (!oldAnchor || *oldAnchor != anchor)
                {
                    plan.push_back({ std::nullopt, childIndex, child, oldAnchor, anchor });
                }
            }
        }
    }

    if (!apply)
    {
        return std::ranges::all_of(plan, [&](const auto& move) {
            if (_images.contains(move.previous.imageId))
            {
                return true;
            }
            code = L"ENOENT:relative child image not found";
            return false;
        });
    }

    for (const auto& move : plan)
    {
        _erasePlacementCells(move.previous);
    }

    auto succeeded = true;
    for (const auto& move : plan)
    {
        const auto image = _images.find(move.previous.imageId);
        if (image == _images.end())
        {
            succeeded = false;
            code = L"ENOENT:relative child image not found";
            continue;
        }
        til::size drawn;
        try
        {
            drawn = _placeImage(image->second,
                                false,
                                move.previous.imageId,
                                move.previous.layerId,
                                move.previous.displayCols,
                                move.previous.displayRows,
                                move.previous.srcX,
                                move.previous.srcY,
                                move.previous.srcW,
                                move.previous.srcH,
                                move.previous.cellOffsetX,
                                move.previous.cellOffsetY,
                                move.previous.zIndex,
                                move.anchor);
        }
        catch (const std::bad_alloc&)
        {
            if (succeeded)
            {
                code = L"ENOMEM:image layer memory limit exceeded";
            }
            succeeded = false;
            continue;
        }
        if (drawn.width <= 0 || drawn.height <= 0)
        {
            if (succeeded)
            {
                code = L"EINVAL:relative child has empty geometry";
            }
            succeeded = false;
            continue;
        }

        auto* child = move.key ?
                          &_placements.at(*move.key) :
                          &_anonymousPlacements.at(move.anonymousIndex);
        child->anchorCol = move.anchor.x;
        child->anchorRow = move.anchor.y;
        child->cols = drawn.width;
        child->rows = drawn.height;
    }
    if (succeeded)
    {
        return true;
    }

    for (const auto& move : plan)
    {
        _erasePlacementCells(move.previous);
    }
    for (const auto& move : plan)
    {
        auto* child = move.key ?
                          &_placements.at(*move.key) :
                          &_anonymousPlacements.at(move.anonymousIndex);
        *child = move.previous;
        if (!move.oldAnchor)
        {
            continue;
        }
        const auto image = _images.find(move.previous.imageId);
        if (image == _images.end())
        {
            continue;
        }
        try
        {
            _placeImage(image->second,
                        false,
                        move.previous.imageId,
                        move.previous.layerId,
                        move.previous.displayCols,
                        move.previous.displayRows,
                        move.previous.srcX,
                        move.previous.srcY,
                        move.previous.srcW,
                        move.previous.srcH,
                        move.previous.cellOffsetX,
                        move.previous.cellOffsetY,
                        move.previous.zIndex,
                        *move.oldAnchor);
        }
        catch (const std::bad_alloc&)
        {
        }
    }
    return false;
}

void KittyParser::_erasePlacementCells(const Placement& placement)
{
    if (placement.layerId == 0)
    {
        return;
    }

    const auto visiblePageNumber = _dispatcher._pages.VisiblePage().Number();
    _dispatcher._pages.ForEachPage([&](const Page page) {
        auto& buffer = page.Buffer();
        auto firstRow = page.Bottom();
        auto lastRow = 0;
        const ImagePlacement::Key key{ placement.imageId, placement.layerId, ImagePlacement::Key::Protocol::Kitty };
        for (const auto& image : buffer.GetImages().All())
        {
            if (image.Identity() == key)
            {
                firstRow = std::min(firstRow, image.CellBounds().top);
                lastRow = std::max(lastRow, image.CellBounds().bottom - 1);
            }
        }
        buffer.GetMutableImages().Erase(key);
        if (page.Number() == visiblePageNumber && firstRow <= lastRow)
        {
            buffer.TriggerRedraw(Viewport::FromExclusive({ 0, firstRow, page.Width(), lastRow + 1 }));
        }
    });

    if (const auto image = _images.find(placement.imageId); image != _images.end())
    {
        image->second.hasRenderedPlacements = _imageHasRenderedPlacements(placement.imageId);
        if (!image->second.hasRenderedPlacements)
        {
            _releaseImageSurface(image->second);
        }
    }
}

void KittyParser::_erasePlacementsForImage(const uint32_t imageId)
{
    std::deque<std::pair<uint32_t, uint32_t>> removed;
    for (auto placement = _placements.begin(); placement != _placements.end();)
    {
        if (placement->first.first == imageId)
        {
            removed.push_back(placement->first);
            placement = _placements.erase(placement);
        }
        else
        {
            ++placement;
        }
    }

    for (auto placement = _anonymousPlacements.begin(); placement != _anonymousPlacements.end();)
    {
        if (placement->imageId == imageId)
        {
            _erasePlacementCells(*placement);
            placement = _anonymousPlacements.erase(placement);
        }
        else
        {
            ++placement;
        }
    }

    _cascadePlacementChildren(removed, imageId);
}

bool KittyParser::_imageHasPlacements(const uint32_t id) const noexcept
{
    for (const auto& [key, placement] : _placements)
    {
        static_cast<void>(placement);
        if (key.first == id)
        {
            return true;
        }
    }
    return std::any_of(_anonymousPlacements.begin(), _anonymousPlacements.end(), [id](const Placement& placement) {
        return placement.imageId == id;
    });
}

bool KittyParser::_imageHasRenderedPlacements(const uint32_t id) const
{
    auto found = false;
    _dispatcher._pages.ForEachPage([&](const Page page) {
        if (found)
        {
            return;
        }
        for (const auto& placement : page.Buffer().GetImages().All())
        {
            const auto key = placement.Identity();
            if (key.protocol == ImagePlacement::Key::Protocol::Kitty && key.imageId == id)
            {
                found = true;
                return;
            }
        }
    });
    return found;
}

void KittyParser::_cascadePlacementChildren(std::deque<std::pair<uint32_t, uint32_t>>& removed, const uint32_t keepImageId)
{
    auto guard = MaxPlacements + 1;
    while (!removed.empty() && guard-- > 0)
    {
        const auto parent = removed.front();
        removed.pop_front();
        for (auto placement = _placements.begin(); placement != _placements.end();)
        {
            const auto& child = placement->second;
            if (child.hasParent && child.parentImageId == parent.first && child.parentPlacementId == parent.second)
            {
                _erasePlacementCells(child);
                removed.push_back(placement->first);
                placement = _placements.erase(placement);
            }
            else
            {
                ++placement;
            }
        }

        for (auto placement = _anonymousPlacements.begin(); placement != _anonymousPlacements.end();)
        {
            if (placement->hasParent && placement->parentImageId == parent.first && placement->parentPlacementId == parent.second)
            {
                const auto anonymousImageId = placement->imageId;
                _erasePlacementCells(*placement);
                placement = _anonymousPlacements.erase(placement);
                if (anonymousImageId != keepImageId && _images.count(anonymousImageId) != 0 && !_imageHasPlacements(anonymousImageId))
                {
                    _eraseImage(anonymousImageId);
                    _eraseImagePlacements(anonymousImageId);
                }
            }
            else
            {
                ++placement;
            }
        }

        const auto childImageId = parent.first;
        if (childImageId != keepImageId && _images.count(childImageId) != 0 && !_imageHasPlacements(childImageId))
        {
            _eraseImage(childImageId);
            _eraseImagePlacements(childImageId);
        }
    }
}

void KittyParser::_deletePlacement(const uint32_t imageId, const uint32_t placementId, const bool freeData)
{
    const auto placement = _placements.find({ imageId, placementId });
    if (placement == _placements.end())
    {
        return;
    }

    _erasePlacementCells(placement->second);
    std::deque<std::pair<uint32_t, uint32_t>> removed{ { imageId, placementId } };
    _placements.erase(placement);
    _cascadePlacementChildren(removed, freeData ? 0 : imageId);
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
        image->second.hasRenderedPlacements = false;
        _releaseImageSurface(image->second);
    }
}

void KittyParser::_deleteAllPlacements(const bool freeData)
{
    std::vector<std::pair<uint32_t, uint32_t>> selectedPlacements;
    std::vector<uint64_t> anonymousLayers;
    std::vector<uint32_t> affectedImageIds;

    for (const auto& [key, placement] : _placements)
    {
        // Directly rendered placements own a positive footprint. Later virtual prototypes
        // retain an empty footprint and must not be selected by d=a/A.
        if (placement.cols > 0 && placement.rows > 0)
        {
            selectedPlacements.push_back(key);
            affectedImageIds.push_back(placement.imageId);
        }
    }
    for (const auto& placement : _anonymousPlacements)
    {
        if (placement.cols > 0 && placement.rows > 0)
        {
            anonymousLayers.push_back(placement.layerId);
            affectedImageIds.push_back(placement.imageId);
        }
    }
    std::sort(anonymousLayers.begin(), anonymousLayers.end());
    std::sort(affectedImageIds.begin(), affectedImageIds.end());
    affectedImageIds.erase(std::unique(affectedImageIds.begin(), affectedImageIds.end()), affectedImageIds.end());

    std::vector<std::pair<uint32_t, uint32_t>> selectedRoots;
    for (const auto& key : selectedPlacements)
    {
        const auto& placement = _placements.at(key);
        const std::pair<uint32_t, uint32_t> parentKey{ placement.parentImageId, placement.parentPlacementId };
        const auto parentIsSelected = placement.hasParent &&
                                      std::find(selectedPlacements.begin(), selectedPlacements.end(), parentKey) != selectedPlacements.end();
        if (!parentIsSelected)
        {
            selectedRoots.push_back(key);
        }
    }
    for (const auto& key : selectedRoots)
    {
        _deletePlacement(key.first, key.second, false);
    }
    for (auto placement = _anonymousPlacements.begin(); placement != _anonymousPlacements.end();)
    {
        if (std::binary_search(anonymousLayers.begin(), anonymousLayers.end(), placement->layerId))
        {
            _erasePlacementCells(*placement);
            placement = _anonymousPlacements.erase(placement);
        }
        else
        {
            ++placement;
        }
    }

    if (freeData)
    {
        for (const auto imageId : affectedImageIds)
        {
            if (!_imageHasPlacements(imageId) && !_imageHasRenderedPlacements(imageId))
            {
                _eraseImage(imageId);
            }
        }
    }
}

void KittyParser::_deleteImagesIntersecting(const til::CoordType left,
                                            const til::CoordType top,
                                            const til::CoordType right,
                                            const til::CoordType bottom,
                                            const bool freeData)
{
    auto page = _dispatcher._pages.ActivePage();
    auto& buffer = page.Buffer();
    const auto rowBegin = std::max(0, top);
    const auto rowEnd = std::min(bottom, page.Bottom());
    const auto columnBegin = std::max(0, left);
    const auto columnEnd = std::min(right, page.Width());
    if (rowEnd <= rowBegin || columnEnd <= columnBegin)
    {
        return;
    }

    std::vector<std::pair<uint32_t, uint64_t>> physicalPlacements;
    for (const auto& [key, placement] : _placements)
    {
        static_cast<void>(key);
        if (placement.cols > 0 && placement.rows > 0)
        {
            physicalPlacements.emplace_back(placement.imageId, placement.layerId);
        }
    }
    for (const auto& placement : _anonymousPlacements)
    {
        if (placement.cols > 0 && placement.rows > 0)
        {
            physicalPlacements.emplace_back(placement.imageId, placement.layerId);
        }
    }
    std::sort(physicalPlacements.begin(), physicalPlacements.end());
    physicalPlacements.erase(std::unique(physicalPlacements.begin(), physicalPlacements.end()), physicalPlacements.end());

    std::vector<std::pair<uint32_t, uint64_t>> selectedPlacements;
    const til::rect target{ columnBegin, rowBegin, columnEnd, rowEnd };
    for (const auto& placement : buffer.GetImages().IntersectingRows(rowBegin, rowEnd))
    {
        const auto key = placement.Identity();
        const std::pair<uint32_t, uint64_t> identity{ key.imageId, key.layerId };
        if (key.protocol != ImagePlacement::Key::Protocol::Kitty ||
            (placement.CellBounds() & target).empty() ||
            !std::binary_search(physicalPlacements.begin(), physicalPlacements.end(), identity))
        {
            continue;
        }
        selectedPlacements.push_back(identity);
    }
    std::sort(selectedPlacements.begin(), selectedPlacements.end());
    selectedPlacements.erase(std::unique(selectedPlacements.begin(), selectedPlacements.end()), selectedPlacements.end());

    std::vector<std::pair<uint32_t, uint32_t>> namedPlacements;
    std::vector<uint32_t> affectedImageIds;
    for (const auto& [key, placement] : _placements)
    {
        const std::pair<uint32_t, uint64_t> identity{ placement.imageId, placement.layerId };
        if (std::binary_search(selectedPlacements.begin(), selectedPlacements.end(), identity))
        {
            namedPlacements.push_back(key);
            affectedImageIds.push_back(placement.imageId);
        }
    }
    for (const auto& placement : _anonymousPlacements)
    {
        const std::pair<uint32_t, uint64_t> identity{ placement.imageId, placement.layerId };
        if (std::binary_search(selectedPlacements.begin(), selectedPlacements.end(), identity))
        {
            affectedImageIds.push_back(placement.imageId);
        }
    }
    std::sort(affectedImageIds.begin(), affectedImageIds.end());
    affectedImageIds.erase(std::unique(affectedImageIds.begin(), affectedImageIds.end()), affectedImageIds.end());

    for (const auto& key : namedPlacements)
    {
        _deletePlacement(key.first, key.second, false);
    }
    for (auto placement = _anonymousPlacements.begin(); placement != _anonymousPlacements.end();)
    {
        const std::pair<uint32_t, uint64_t> identity{ placement->imageId, placement->layerId };
        if (std::binary_search(selectedPlacements.begin(), selectedPlacements.end(), identity))
        {
            _erasePlacementCells(*placement);
            placement = _anonymousPlacements.erase(placement);
        }
        else
        {
            ++placement;
        }
    }

    if (freeData)
    {
        for (const auto imageId : affectedImageIds)
        {
            if (!_imageHasPlacements(imageId) && !_imageHasRenderedPlacements(imageId))
            {
                _eraseImage(imageId);
            }
        }
    }
}

void KittyParser::_deleteImagesInIdRange(const uint32_t lo, const uint32_t hi, const bool freeData)
{
    if (lo == 0 || hi == 0 || lo > hi)
    {
        return;
    }

    std::vector<uint32_t> imageIds;
    for (const auto& [imageId, image] : _images)
    {
        static_cast<void>(image);
        if (imageId >= lo && imageId <= hi)
        {
            imageIds.push_back(imageId);
        }
    }
    for (const auto imageId : imageIds)
    {
        _erasePlacementsForImage(imageId);
        if (freeData)
        {
            _eraseImage(imageId);
        }
        _eraseImagePlacements(imageId);
    }
}

void KittyParser::_deletePlacementsByZ(const int32_t zIndex, const bool freeData, const std::optional<til::point> cell)
{
    auto page = _dispatcher._pages.ActivePage();
    auto& buffer = page.Buffer();
    std::vector<uint64_t> layerIds;

    if (cell)
    {
        if (cell->x >= 0 && cell->x < page.Width() && cell->y >= 0 && cell->y < page.Bottom())
        {
            for (const auto& image : buffer.GetImages().IntersectingRows(cell->y, cell->y + 1))
            {
                const auto key = image.Identity();
                const auto bounds = image.CellBounds();
                if (key.protocol == ImagePlacement::Key::Protocol::Kitty &&
                    image.ZIndex() == zIndex &&
                    bounds.left <= cell->x && cell->x < bounds.right &&
                    bounds.top <= cell->y && cell->y < bounds.bottom)
                {
                    const auto physical =
                        std::any_of(_placements.begin(), _placements.end(), [&](const auto& entry) {
                            return entry.second.imageId == key.imageId && entry.second.layerId == key.layerId;
                        }) ||
                        std::any_of(_anonymousPlacements.begin(), _anonymousPlacements.end(), [&](const Placement& placement) {
                            return placement.imageId == key.imageId && placement.layerId == key.layerId;
                        });
                    if (physical)
                    {
                        layerIds.push_back(key.layerId);
                    }
                }
            }
        }
    }
    else
    {
        for (const auto& [key, placement] : _placements)
        {
            static_cast<void>(key);
            if (placement.zIndex == zIndex)
            {
                layerIds.push_back(placement.layerId);
            }
        }
        for (const auto& placement : _anonymousPlacements)
        {
            if (placement.zIndex == zIndex)
            {
                layerIds.push_back(placement.layerId);
            }
        }
    }

    std::sort(layerIds.begin(), layerIds.end());
    layerIds.erase(std::unique(layerIds.begin(), layerIds.end()), layerIds.end());
    if (layerIds.empty())
    {
        return;
    }

    const auto selected = [&](const Placement& placement) {
        return std::binary_search(layerIds.begin(), layerIds.end(), placement.layerId);
    };

    std::vector<std::pair<uint32_t, uint32_t>> selectedPlacements;
    for (const auto& [key, placement] : _placements)
    {
        if (selected(placement))
        {
            selectedPlacements.push_back(key);
        }
    }
    std::vector<std::pair<uint32_t, uint32_t>> selectedRoots;
    for (const auto& key : selectedPlacements)
    {
        const auto& placement = _placements.at(key);
        const std::pair<uint32_t, uint32_t> parentKey{ placement.parentImageId, placement.parentPlacementId };
        const auto parentIsSelected = placement.hasParent &&
                                      std::find(selectedPlacements.begin(), selectedPlacements.end(), parentKey) != selectedPlacements.end();
        if (!parentIsSelected)
        {
            selectedRoots.push_back(key);
        }
    }
    for (const auto& key : selectedRoots)
    {
        const auto placement = _placements.find(key);
        if (placement != _placements.end())
        {
            _erasePlacementCells(placement->second);
            _placements.erase(placement);
            std::deque<std::pair<uint32_t, uint32_t>> removed{ key };
            _cascadePlacementChildren(removed, key.first);
        }
    }

    std::vector<uint32_t> imageIds;
    for (auto placement = _anonymousPlacements.begin(); placement != _anonymousPlacements.end();)
    {
        if (selected(*placement))
        {
            if (std::find(imageIds.begin(), imageIds.end(), placement->imageId) == imageIds.end())
            {
                imageIds.push_back(placement->imageId);
            }
            _erasePlacementCells(*placement);
            placement = _anonymousPlacements.erase(placement);
        }
        else
        {
            ++placement;
        }
    }
    for (const auto& key : selectedPlacements)
    {
        if (std::find(imageIds.begin(), imageIds.end(), key.first) == imageIds.end())
        {
            imageIds.push_back(key.first);
        }
    }

    if (freeData)
    {
        for (const auto imageId : imageIds)
        {
            if (!_imageHasPlacements(imageId) && !_imageHasRenderedPlacements(imageId))
            {
                _eraseImage(imageId);
            }
        }
    }
}

std::optional<til::point> KittyParser::_derivePlacementAnchor(const Placement& placement) const
{
    if (placement.layerId == 0)
    {
        return std::nullopt;
    }
    auto page = _dispatcher._pages.ActivePage();
    auto& buffer = page.Buffer();
    auto minRow = page.Bottom();
    auto minCol = page.Width();
    auto found = false;
    for (const auto& image : buffer.GetImages().All())
    {
        if (image.Identity() != ImagePlacement::Key{ placement.imageId, placement.layerId, ImagePlacement::Key::Protocol::Kitty })
        {
            continue;
        }
        const auto bounds = image.CellBounds();
        minRow = std::min(minRow, bounds.top);
        minCol = std::min(minCol, bounds.left);
        found = true;
    }
    return found ? std::optional<til::point>{ til::point{ minCol, minRow } } : std::nullopt;
}

std::optional<til::point> KittyParser::_resolvePlacementAnchor(const uint32_t parentImageId,
                                                               const uint32_t parentPlacementId,
                                                               const std::pair<uint32_t, uint32_t> origin,
                                                               std::wstring_view& code) const
{
    std::vector<std::pair<uint32_t, uint32_t>> visited{ origin };
    std::pair<uint32_t, uint32_t> key{ parentImageId, parentPlacementId };
    std::optional<til::point> immediateAnchor;
    const auto descendantsFit = [&](const int ancestorDepth) {
        if (_placements.find(origin) == _placements.end())
        {
            return true;
        }
        std::map<std::pair<uint32_t, uint32_t>, std::vector<std::pair<uint32_t, uint32_t>>> children;
        for (const auto& entry : _placements)
        {
            const auto& placement = entry.second;
            if (placement.hasParent)
            {
                children[{ placement.parentImageId, placement.parentPlacementId }].push_back(entry.first);
            }
        }

        std::deque<std::pair<std::pair<uint32_t, uint32_t>, int>> pending{ { origin, 0 } };
        std::set<std::pair<uint32_t, uint32_t>> seen;
        while (!pending.empty())
        {
            const auto [current, descendantDepth] = pending.front();
            pending.pop_front();
            if (!seen.emplace(current).second || ancestorDepth + descendantDepth > MaxPlacementDepth)
            {
                return false;
            }
            if (const auto found = children.find(current); found != children.end())
            {
                for (const auto& child : found->second)
                {
                    pending.push_back({ child, descendantDepth + 1 });
                }
            }
        }
        return true;
    };
    for (auto depth = 1; depth <= MaxPlacementDepth; ++depth)
    {
        if (std::find(visited.begin(), visited.end(), key) != visited.end())
        {
            code = L"ECYCLE:relative placement cycle";
            return std::nullopt;
        }
        visited.push_back(key);

        const auto placement = _placements.find(key);
        if (placement == _placements.end())
        {
            code = L"ENOPARENT:relative parent not found";
            return std::nullopt;
        }

        const auto& parent = placement->second;
        if (depth == 1)
        {
            immediateAnchor = _derivePlacementAnchor(parent);
            if (!immediateAnchor)
            {
                code = L"ENOPARENT:relative parent not found";
                return std::nullopt;
            }
        }
        if (!parent.hasParent)
        {
            if (!descendantsFit(depth))
            {
                code = L"ETOODEEP:relative placement chain too deep";
                return std::nullopt;
            }
            return immediateAnchor;
        }
        key = { parent.parentImageId, parent.parentPlacementId };
    }
    code = L"ETOODEEP:relative placement chain too deep";
    return std::nullopt;
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
