// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "KittyParser.hpp"
#include "adaptDispatch.hpp"
#include <til/unicode.h>
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
            case L'U':
                c.virtualPlacement = _ParseUint(value) != 0;
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
        const auto priorAnchor = priorPlacement ?
                                     (priorPlacement->isVirtual ?
                                          _deriveVirtualPlacementAnchor(targetImageId, placementId) :
                                          _derivePlacementAnchor(*priorPlacement)) :
                                     std::nullopt;
        const auto removePriorPlacement = [&]() {
            if (priorPlacement)
            {
                _erasePlacementCells(*priorPlacement);
                if (priorPlacement->isVirtual)
                {
                    _virtualIds.erase({ targetImageId, placementId });
                }
                if (placementId == 0)
                {
                    _virtualIds.erase({ targetImageId, 0u });
                    std::erase_if(_anonymousPlacements, [&](const auto& placement) {
                        return placement.layerId == priorPlacement->layerId;
                    });
                }
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
                const auto& victim = _anonymousPlacements.front();
                _erasePlacementCells(victim);
                if (victim.isVirtual)
                {
                    _virtualIds.erase({ victim.imageId, 0u });
                }
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
            placement.isVirtual = false;
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
        placement.isVirtual = false;
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
    const auto storeKittyVirtualPlacement = [&](const uint32_t targetImageId, const Image& image) {
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
        else
        {
            const auto existing = std::find_if(_anonymousPlacements.begin(), _anonymousPlacements.end(), [&](const auto& placement) {
                return placement.isVirtual && placement.imageId == targetImageId;
            });
            if (existing != _anonymousPlacements.end())
            {
                priorPlacement = *existing;
            }
        }

        _storeVirtualPlacement(targetImageId, placementId, image, command.cols, command.rows, command.srcX, command.srcY, command.srcW, command.srcH, command.zIndex, layerId);

        if (priorPlacement)
        {
            _erasePlacementCells(*priorPlacement);
            if (placementId != 0)
            {
                _placements.erase({ targetImageId, placementId });
            }
            else
            {
                std::erase_if(_anonymousPlacements, [&](const auto& placement) {
                    return placement.layerId == priorPlacement->layerId;
                });
            }
        }

        const auto virtualPlacement = _virtualIds.find({ targetImageId, placementId });
        if (virtualPlacement == _virtualIds.end())
        {
            return;
        }
        Placement placement;
        placement.imageId = targetImageId;
        placement.placementId = placementId;
        placement.layerId = layerId;
        placement.zIndex = command.zIndex;
        placement.isVirtual = true;
        if (placementId != 0)
        {
            _registerPlacement(placement);
        }
        else
        {
            while (_anonymousPlacements.size() >= MaxPlacements)
            {
                const auto& victim = _anonymousPlacements.front();
                _erasePlacementCells(victim);
                if (victim.isVirtual)
                {
                    _virtualIds.erase({ victim.imageId, 0u });
                }
                _anonymousPlacements.erase(_anonymousPlacements.begin());
            }
            _anonymousPlacements.push_back(placement);
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
            if (command.virtualPlacement && command.haveParent)
            {
                // A virtual (U=1) placement cannot itself be relative. This is a conflict
                // between two control keys, so it is settled here, before anything is
                // decoded, read, or registered: deciding it after the image had been stored
                // would replace or evict registry entries on behalf of a command that then
                // fails.
                success = false;
                code = L"EINVAL:virtual placements cannot be relative";
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
                if (command.virtualPlacement)
                {
                    const auto stored = _images.find(assignedId);
                    if (stored != _images.end())
                    {
                        storeKittyVirtualPlacement(assignedId, stored->second);
                    }
                }
                else
                {
                    std::erase_if(_virtualIds, [&](const auto& entry) {
                        return entry.first.first == assignedId;
                    });
                }
                if (action == L'T' && !command.virtualPlacement)
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
            else if (command.virtualPlacement)
            {
                if (command.haveParent)
                {
                    // A virtual (U=1) placement cannot itself be relative.
                    success = false;
                    code = L"EINVAL:virtual placements cannot be relative";
                }
                else
                {
                    // Virtual put: eligible for placeholders with the requested grid, no cursor draw.
                    storeKittyVirtualPlacement(targetId, *target);
                }
            }
            else
            {
                // Re-putting the same (i, p) replaces the prior placement (move/resize); that
                // replacement is now handled inside displayKittyPlacement, which erases the prior
                // placement's cells ONLY once the new placement is known to succeed. This avoids
                // destroying an existing placement when a relative re-put fails to resolve.
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
                        // A virtual (U=1) placement is itself a placement, deleted by i/I/n/N/r/R
                        // regardless of case (spec); only the image DATA free is case-gated, so drop
                        // the virtual grid here so a later placeholder doesn't re-render it.
                        std::erase_if(_virtualIds, [&](const auto& entry) {
                            return entry.first.first == imageId;
                        });
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
                            std::erase_if(_virtualIds, [&](const auto& entry) {
                                return entry.first.first == targetId;
                            });
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
    std::erase_if(_virtualIds, [&](const auto& entry) {
        return entry.first.first == id;
    });
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
    _virtualIds.clear();
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
    state.virtualIds = std::move(_virtualIds);
    state.placements = std::move(_placements);
    state.anonymousPlacements = std::move(_anonymousPlacements);
    _images.clear();
    _imageNumbers.clear();
    _imageOrder.clear();
    _virtualIds.clear();
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
    _virtualIds = std::move(state.virtualIds);
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

// Records the fixed grid geometry of a virtual (U=1) placement so later Unicode-placeholder
// rendering slices the image by a STABLE rows x cols grid. The grid is the cell span the same
// image would occupy if drawn at the cursor (shared _targetPixels), so c-only/r-only
// keep aspect. Omitted placeholder coordinates are resolved from persistent left-cell metadata,
// not placement-global state, so re-storing does not disturb already-written placeholder text.
void KittyParser::_storeVirtualPlacement(const uint32_t id, const uint32_t placementId, const Image& image, const uint32_t cols, const uint32_t rows, const uint32_t srcX, const uint32_t srcY, const uint32_t srcW, const uint32_t srcH, const int32_t zIndex, const uint64_t layerId)
{
    constexpr uint32_t maxCells = 8192;
    const auto cellSize = _dispatcher._api.GetCellSize();
    const int64_t cellWidth = std::max(1, cellSize.width);
    const int64_t cellHeight = std::max(1, cellSize.height);
    const auto imageWidth = static_cast<int64_t>(image.width);
    const auto imageHeight = static_cast<int64_t>(image.height);
    // Source crop rect (pixels), clamped to the image; w/h=0 (or past the edge) extends to the
    // right/bottom edge. Matches _placeImage so a U=1 placement samples the same sub-rect
    // a direct c/r draw would, and the grid aspect follows the CROP (not the full image).
    const auto cropX = std::min<int64_t>(srcX, imageWidth);
    const auto cropY = std::min<int64_t>(srcY, imageHeight);
    const auto cropW = srcW == 0 ? imageWidth - cropX : std::min<int64_t>(std::min<int64_t>(srcW, imageWidth), imageWidth - cropX);
    const auto cropH = srcH == 0 ? imageHeight - cropY : std::min<int64_t>(std::min<int64_t>(srcH, imageHeight), imageHeight - cropY);
    if (cropW <= 0 || cropH <= 0)
    {
        // An empty crop (x/y at or past the image edge) displays nothing, matching _placeImage.
        // Register no virtual grid -- and drop any prior one on re-put -- so a later placeholder for
        // this id draws nothing rather than sampling outside the crop (which would read the adjacent
        // pixel row and leak cropped-out data).
        _virtualIds.erase({ id, placementId });
        return;
    }
    const auto [targetW, targetH] = _targetPixels(cropW, cropH, cols, rows, cellWidth, cellHeight);
    const auto gridCols = std::clamp<int64_t>((targetW + cellWidth - 1) / cellWidth, 1, maxCells);
    const auto gridRows = std::clamp<int64_t>((targetH + cellHeight - 1) / cellHeight, 1, maxCells);
    auto& placement = _virtualIds[{ id, placementId }];
    placement.cols = static_cast<uint32_t>(gridCols);
    placement.rows = static_cast<uint32_t>(gridRows);
    placement.cropX = static_cast<uint32_t>(cropX);
    placement.cropY = static_cast<uint32_t>(cropY);
    placement.cropW = static_cast<uint32_t>(cropW);
    placement.cropH = static_cast<uint32_t>(cropH);
    // Keep the exact scaled target size (64-bit: aspect-preserving scaling can exceed 2^32) so
    // placeholder sampling matches _placeImage's continuous scaling (not a per-cell source
    // split) for non-divisible geometry -- storing it narrower would truncate and diverge.
    placement.targetW = static_cast<uint64_t>(std::max<int64_t>(targetW, 1));
    placement.targetH = static_cast<uint64_t>(std::max<int64_t>(targetH, 1));
    placement.layerId = layerId;
    placement.zIndex = zIndex;
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
        if (victimValue.isVirtual)
        {
            _virtualIds.erase(victimKey);
        }
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

    std::erase_if(_virtualIds, [&](const auto& entry) {
        return entry.first.first == imageId;
    });

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
                if (child.isVirtual)
                {
                    _virtualIds.erase(placement->first);
                }
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
    _virtualIds.erase({ imageId, placementId });
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
        std::erase_if(_virtualIds, [&](const auto& entry) {
            return entry.first.first == imageId;
        });
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
                            return !entry.second.isVirtual &&
                                   entry.second.imageId == key.imageId &&
                                   entry.second.layerId == key.layerId;
                        }) ||
                        std::any_of(_anonymousPlacements.begin(), _anonymousPlacements.end(), [&](const Placement& placement) {
                            return !placement.isVirtual &&
                                   placement.imageId == key.imageId &&
                                   placement.layerId == key.layerId;
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
            if (!placement.isVirtual && placement.zIndex == zIndex)
            {
                layerIds.push_back(placement.layerId);
            }
        }
        for (const auto& placement : _anonymousPlacements)
        {
            if (!placement.isVirtual && placement.zIndex == zIndex)
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
        return !placement.isVirtual &&
               std::binary_search(layerIds.begin(), layerIds.end(), placement.layerId);
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
    if (placement.layerId == 0 || placement.isVirtual)
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

// Derives the on-screen anchor of a virtual (U=1) parent from its Unicode-placeholder cells:
// the top-left is the minimum x/y over its direct-renderer fragments. Returns nullopt if no
// placeholder fragment for the image is currently on screen.
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
std::optional<til::point> KittyParser::_deriveVirtualPlacementAnchor(const uint32_t imageId, const uint32_t placementId) const
{
    const auto virtualPlacement = _virtualIds.find({ imageId, placementId });
    if (imageId == 0 || virtualPlacement == _virtualIds.end())
    {
        return std::nullopt;
    }
    const auto layerId = virtualPlacement->second.layerId;
    auto page = _dispatcher._pages.ActivePage();
    auto& buffer = page.Buffer();
    auto minRow = page.Bottom();
    auto minCol = page.Width();
    auto found = false;
    for (const auto& image : buffer.GetImages().All())
    {
        if (image.Identity() != ImagePlacement::Key{ imageId, layerId, ImagePlacement::Key::Protocol::Kitty })
        {
            continue;
        }
        const auto bounds = image.CellBounds();
        minRow = std::min(minRow, bounds.top);
        minCol = std::min(minCol, bounds.left);
        found = true;
    }
    if (!found)
    {
        return std::nullopt;
    }
    return til::point{ minCol, minRow };
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
            // Not a registered placement. An ANONYMOUS virtual image (U=1 with no placement id)
            // is still a valid parent referenced as (imageId, 0): its anchor comes from the
            // on-screen Unicode-placeholder cells owned by that image id. A non-zero Q that
            // matched no registered placement is a dangling reference -> ENOPARENT.
            if (key.second == 0 && _virtualIds.count(key) != 0)
            {
                const auto derived = _deriveVirtualPlacementAnchor(key.first, key.second);
                if (!derived)
                {
                    code = L"ENOPARENT:relative parent not found";
                    return std::nullopt;
                }
                if (depth == 1)
                {
                    immediateAnchor = derived;
                }
                if (!descendantsFit(depth))
                {
                    code = L"ETOODEEP:relative placement chain too deep";
                    return std::nullopt;
                }
                return immediateAnchor; // a virtual image is always a chain leaf
            }
            code = L"ENOPARENT:relative parent not found";
            return std::nullopt;
        }

        const auto& parent = placement->second;
        if (depth == 1)
        {
            immediateAnchor = parent.isVirtual ? _deriveVirtualPlacementAnchor(key.first, key.second) : _derivePlacementAnchor(parent);
            if (!immediateAnchor)
            {
                code = L"ENOPARENT:relative parent not found";
                return std::nullopt;
            }
        }
        if (parent.isVirtual || !parent.hasParent)
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

// Maps a kitty row/column combining diacritic to its 0-based index, or -1 if the glyph isn't a
// placeholder diacritic. This is the full 297-entry kitty "rowcolumn-diacritics" table, sorted
// ascending so a binary search resolves the index; entries past U+FFFF (musical-symbol combining
// marks, indices 283-296) address grids larger than 283 cells in a dimension. Vendored from the
// kitty graphics protocol spec (the list is identical across implementations). The optional 3rd
// diacritic (high byte of a >24-bit id) and 256-color ids are handled by the caller.
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#unicode-placeholders
int KittyParser::_PlaceholderDiacriticIndex(const char32_t ch) noexcept
{
    static constexpr char32_t table[] = {
        0x0305,
        0x030D,
        0x030E,
        0x0310,
        0x0312,
        0x033D,
        0x033E,
        0x033F,
        0x0346,
        0x034A,
        0x034B,
        0x034C,
        0x0350,
        0x0351,
        0x0352,
        0x0357,
        0x035B,
        0x0363,
        0x0364,
        0x0365,
        0x0366,
        0x0367,
        0x0368,
        0x0369,
        0x036A,
        0x036B,
        0x036C,
        0x036D,
        0x036E,
        0x036F,
        0x0483,
        0x0484,
        0x0485,
        0x0486,
        0x0487,
        0x0592,
        0x0593,
        0x0594,
        0x0595,
        0x0597,
        0x0598,
        0x0599,
        0x059C,
        0x059D,
        0x059E,
        0x059F,
        0x05A0,
        0x05A1,
        0x05A8,
        0x05A9,
        0x05AB,
        0x05AC,
        0x05AF,
        0x05C4,
        0x0610,
        0x0611,
        0x0612,
        0x0613,
        0x0614,
        0x0615,
        0x0616,
        0x0617,
        0x0657,
        0x0658,
        0x0659,
        0x065A,
        0x065B,
        0x065D,
        0x065E,
        0x06D6,
        0x06D7,
        0x06D8,
        0x06D9,
        0x06DA,
        0x06DB,
        0x06DC,
        0x06DF,
        0x06E0,
        0x06E1,
        0x06E2,
        0x06E4,
        0x06E7,
        0x06E8,
        0x06EB,
        0x06EC,
        0x0730,
        0x0732,
        0x0733,
        0x0735,
        0x0736,
        0x073A,
        0x073D,
        0x073F,
        0x0740,
        0x0741,
        0x0743,
        0x0745,
        0x0747,
        0x0749,
        0x074A,
        0x07EB,
        0x07EC,
        0x07ED,
        0x07EE,
        0x07EF,
        0x07F0,
        0x07F1,
        0x07F3,
        0x0816,
        0x0817,
        0x0818,
        0x0819,
        0x081B,
        0x081C,
        0x081D,
        0x081E,
        0x081F,
        0x0820,
        0x0821,
        0x0822,
        0x0823,
        0x0825,
        0x0826,
        0x0827,
        0x0829,
        0x082A,
        0x082B,
        0x082C,
        0x082D,
        0x0951,
        0x0953,
        0x0954,
        0x0F82,
        0x0F83,
        0x0F86,
        0x0F87,
        0x135D,
        0x135E,
        0x135F,
        0x17DD,
        0x193A,
        0x1A17,
        0x1A75,
        0x1A76,
        0x1A77,
        0x1A78,
        0x1A79,
        0x1A7A,
        0x1A7B,
        0x1A7C,
        0x1B6B,
        0x1B6D,
        0x1B6E,
        0x1B6F,
        0x1B70,
        0x1B71,
        0x1B72,
        0x1B73,
        0x1CD0,
        0x1CD1,
        0x1CD2,
        0x1CDA,
        0x1CDB,
        0x1CE0,
        0x1DC0,
        0x1DC1,
        0x1DC3,
        0x1DC4,
        0x1DC5,
        0x1DC6,
        0x1DC7,
        0x1DC8,
        0x1DC9,
        0x1DCB,
        0x1DCC,
        0x1DD1,
        0x1DD2,
        0x1DD3,
        0x1DD4,
        0x1DD5,
        0x1DD6,
        0x1DD7,
        0x1DD8,
        0x1DD9,
        0x1DDA,
        0x1DDB,
        0x1DDC,
        0x1DDD,
        0x1DDE,
        0x1DDF,
        0x1DE0,
        0x1DE1,
        0x1DE2,
        0x1DE3,
        0x1DE4,
        0x1DE5,
        0x1DE6,
        0x1DFE,
        0x20D0,
        0x20D1,
        0x20D4,
        0x20D5,
        0x20D6,
        0x20D7,
        0x20DB,
        0x20DC,
        0x20E1,
        0x20E7,
        0x20E9,
        0x20F0,
        0x2CEF,
        0x2CF0,
        0x2CF1,
        0x2DE0,
        0x2DE1,
        0x2DE2,
        0x2DE3,
        0x2DE4,
        0x2DE5,
        0x2DE6,
        0x2DE7,
        0x2DE8,
        0x2DE9,
        0x2DEA,
        0x2DEB,
        0x2DEC,
        0x2DED,
        0x2DEE,
        0x2DEF,
        0x2DF0,
        0x2DF1,
        0x2DF2,
        0x2DF3,
        0x2DF4,
        0x2DF5,
        0x2DF6,
        0x2DF7,
        0x2DF8,
        0x2DF9,
        0x2DFA,
        0x2DFB,
        0x2DFC,
        0x2DFD,
        0x2DFE,
        0x2DFF,
        0xA66F,
        0xA67C,
        0xA67D,
        0xA6F0,
        0xA6F1,
        0xA8E0,
        0xA8E1,
        0xA8E2,
        0xA8E3,
        0xA8E4,
        0xA8E5,
        0xA8E6,
        0xA8E7,
        0xA8E8,
        0xA8E9,
        0xA8EA,
        0xA8EB,
        0xA8EC,
        0xA8ED,
        0xA8EE,
        0xA8EF,
        0xA8F0,
        0xA8F1,
        0xAAB0,
        0xAAB2,
        0xAAB3,
        0xAAB7,
        0xAAB8,
        0xAABE,
        0xAABF,
        0xAAC1,
        0xFE20,
        0xFE21,
        0xFE22,
        0xFE23,
        0xFE24,
        0xFE25,
        0xFE26,
        0x10A0F,
        0x10A38,
        0x1D185,
        0x1D186,
        0x1D187,
        0x1D188,
        0x1D189,
        0x1D1AA,
        0x1D1AB,
        0x1D1AC,
        0x1D1AD,
        0x1D242,
        0x1D243,
        0x1D244,
    };
    const auto it = std::lower_bound(std::begin(table), std::end(table), ch);
    if (it != std::end(table) && *it == ch)
    {
        return static_cast<int>(it - std::begin(table));
    }
    return -1;
}

// Registers one visible cell fragment of a Unicode placeholder. The renderer samples
// the complete scaled placement, so adjacent fragments share one source surface.
bool KittyParser::_placeImageCellRef(const Image& image, const uint32_t imageId, const til::CoordType column, const til::CoordType row, const uint32_t cellRow, const uint32_t cellCol, const VirtualPlacement& place, std::vector<ImagePlacement>& fragments)
{
    if (!image.pixels || image.pixels->empty() || image.width == 0 || image.height == 0)
    {
        return false;
    }
    auto page = _dispatcher._pages.ActivePage();
    if (column < 0 || column >= page.Width() || row < 0 || row >= page.Bottom())
    {
        return false;
    }
    const auto cellSize = _dispatcher._api.GetCellSize();
    const auto cellWidth = std::max(1, cellSize.width);
    const auto cellHeight = std::max(1, cellSize.height);
    const til::size clampedCellSize{ cellWidth, cellHeight };
    const auto gridCols = std::max<uint32_t>(place.cols, 1);
    const auto gridRows = std::max<uint32_t>(place.rows, 1);
    // An explicit row/column outside the placement grid selects no tile: draw nothing rather
    // than clamping to (and duplicating) the edge tile.
    if (cellCol >= gridCols || cellRow >= gridRows)
    {
        return false;
    }
    // Crop rect (absolute image pixels) captured at store time; 0 = unset => full image. targetW/H
    // is the exact scaled size; fall back to the grid-filled size if a legacy entry lacks it.
    const auto cropX = static_cast<til::CoordType>(place.cropX);
    const auto cropY = static_cast<til::CoordType>(place.cropY);
    const auto cropW = std::max<til::CoordType>(static_cast<til::CoordType>(place.cropW != 0 ? place.cropW : image.width), 1);
    const auto cropH = std::max<til::CoordType>(static_cast<til::CoordType>(place.cropH != 0 ? place.cropH : image.height), 1);
    const auto targetW = std::max<int64_t>(place.targetW != 0 ? static_cast<int64_t>(place.targetW) : static_cast<int64_t>(gridCols) * cellWidth, 1);
    const auto targetH = std::max<int64_t>(place.targetH != 0 ? static_cast<int64_t>(place.targetH) : static_cast<int64_t>(gridRows) * cellHeight, 1);
    auto surface = image.surface;
    const auto newSurface = !surface;
    if (newSurface)
    {
        surface = std::make_shared<::Image>(
            til::size{ gsl::narrow_cast<til::CoordType>(image.width), gsl::narrow_cast<til::CoordType>(image.height) },
            image.pixels);
    }

    const auto originalLeft = gsl::narrow<til::CoordType>(static_cast<int64_t>(column) - cellCol);
    const auto originalTop = gsl::narrow<til::CoordType>(static_cast<int64_t>(row) - cellRow);
    const til::rect originalBounds{
        originalLeft,
        originalTop,
        gsl::narrow<til::CoordType>(static_cast<int64_t>(originalLeft) + gridCols),
        gsl::narrow<til::CoordType>(static_cast<int64_t>(originalTop) + gridRows),
    };
    auto fragment = ImagePlacement::FromFragment(
        { imageId, place.layerId, ImagePlacement::Key::Protocol::Kitty },
        surface,
        { column, row, column + 1, row + 1 },
        originalBounds,
        place.zIndex,
        { cropX, cropY, cropX + cropW, cropY + cropH },
        {
            .cellSize = clampedCellSize,
            .targetWidth = gsl::narrow_cast<uint64_t>(targetW),
            .targetHeight = gsl::narrow_cast<uint64_t>(targetH),
        });
    fragments.emplace_back(std::move(fragment));
    if (newSurface)
    {
        image.surface = std::move(surface);
    }
    return true;
}

// True when a grapheme cluster is made up entirely of kitty rowcolumn diacritics, i.e. it is the
// tail of a placeholder cell whose write was split, not a cell of its own.
bool KittyParser::_IsPlaceholderDiacriticRun(const std::wstring_view cluster) noexcept
{
    if (cluster.empty())
    {
        return false;
    }
    for (size_t i = 0; i < cluster.size(); ++i)
    {
        auto cp = static_cast<char32_t>(cluster[i]);
        if (til::is_leading_surrogate(cluster[i]) && i + 1 < cluster.size() && til::is_trailing_surrogate(cluster[i + 1]))
        {
            cp = til::combine_surrogates(cluster[i], cluster[i + 1]);
            ++i;
        }
        if (_PlaceholderDiacriticIndex(cp) < 0)
        {
            return false;
        }
    }
    return true;
}

// True when a run opens with a kitty rowcolumn diacritic. Such a run carries no U+10EEEE of its
// own, but its leading marks join - and so re-complete - the placeholder cell the previous write
// left behind, which is why the writer still has to route it through RenderPlaceholders.
bool KittyParser::StartsWithPlaceholderDiacritic(const std::wstring_view text) noexcept
{
    if (text.empty())
    {
        return false;
    }
    auto cp = static_cast<char32_t>(text[0]);
    if (til::is_leading_surrogate(text[0]) && text.size() > 1 && til::is_trailing_surrogate(text[1]))
    {
        cp = til::combine_surrogates(text[0], text[1]);
    }
    return _PlaceholderDiacriticIndex(cp) >= 0;
}

// Overlays each U+10EEEE placeholder in one just-written segment with its sub-rect of the
// (virtual) image named by the cell's foreground (24-bit RGB or a 256-color index = the id).
// The grid (rows x cols) is the geometry recorded when the image was stored virtually, so it
// stays constant however the cells are chunked across writes. A cell's grid (row,col) comes
// from its kitty combining diacritics (1st = row, 2nd = col). Missing values inherit only from
// the immediate-left placeholder when the protocol's foreground/underline and adjacency gates
// match; otherwise they default to zero. The resolved coordinates and image-id high byte are
// stored with the text cell so inheritance survives separate writes, scrolling, and reflow.
// The screen column steps by each glyph's real width (NavigateToNext), so a wide (CJK) glyph
// before a placeholder doesn't shift it. Called per segment with the segment's true post-wrap
// row and start column.
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#unicode-placeholders
void KittyParser::RenderPlaceholders(const std::wstring_view segment, const til::CoordType screenRow, const til::CoordType startColumn)
{
    auto page = _dispatcher._pages.ActivePage();
    auto& buffer = page.Buffer();
    auto& row = buffer.GetMutableRowByOffset(screenRow);
    auto column = startColumn;
    // Track the drawn placeholder cell span so ONE bounded redraw covers the whole segment/row,
    // instead of a TriggerRedraw per cell on the text-output hot path (matches _placeImage).
    auto firstDrawnCol = page.Width();
    auto lastDrawnCol = static_cast<til::CoordType>(-1);
    std::vector<ImagePlacement> fragments;
    fragments.reserve(std::min(segment.size(), static_cast<size_t>(page.Width())));
    const auto recordDraw = [&](const til::CoordType drawnColumn) noexcept {
        firstDrawnCol = std::min(firstDrawnCol, drawnColumn);
        lastDrawnCol = std::max(lastDrawnCol, drawnColumn);
    };
    for (size_t i = 0; i < segment.size();)
    {
        const auto next = buffer.GraphemeNext(segment, i);
        const auto cluster = segment.substr(i, next - i);
        // A write may be split anywhere, including before or between a placeholder's diacritics -
        // the console write path chunks long runs. The orphaned marks then open the next segment,
        // where they join the cell the previous segment already wrote, occupying no column of
        // their own. Counting them as a cell shifted every placeholder that followed one column
        // right, onto a cell carrying no image foreground, so that tile was silently dropped and
        // the grid rendered with a hole in it.
        if (i == 0 && _IsPlaceholderDiacriticRun(cluster))
        {
            // The joined cell was resolved from whatever part of its cluster had arrived, so a
            // split ahead of the first mark left it addressing grid (0,0). The row now holds the
            // finished grapheme, so resolve that cell again from it - at its own column, which
            // keeps the write column where it is.
            const auto previous = row.NavigateToPrevious(startColumn);
            if (previous < startColumn && _renderPlaceholderCell(row, row.GlyphAt(previous), previous, screenRow, fragments))
            {
                recordDraw(previous);
            }
            i = next;
            continue;
        }
        if (_renderPlaceholderCell(row, cluster, column, screenRow, fragments))
        {
            recordDraw(column);
        }
        // Advance by the glyph's real cell width; guard against a non-advancing step.
        const auto nextColumn = row.NavigateToNext(column);
        column = nextColumn > column ? nextColumn : column + 1;
        i = next;
    }
    buffer.GetMutableImages().AddOrReplaceAreas(std::move(fragments));
    // One bounded redraw for every placeholder tile drawn in this segment (avoids a per-cell
    // TriggerRedraw on the text hot path; mirrors _placeImage's single-redraw model).
    if (lastDrawnCol >= firstDrawnCol)
    {
        buffer.TriggerRedraw(Viewport::FromExclusive({ firstDrawnCol, screenRow, std::min<til::CoordType>(lastDrawnCol + 1, page.Width()), screenRow + 1 }));
    }
}

// Resolves one text cell that stands in for an image cell, from its complete grapheme cluster,
// and stages the tile it addresses. Anything that is not a placeholder base is left alone.
// Returns true if a tile was staged, so the caller can extend its redraw span.
bool KittyParser::_renderPlaceholderCell(ROW& row, const std::wstring_view cluster, const til::CoordType column, const til::CoordType screenRow, std::vector<ImagePlacement>& fragments)
{
    if (cluster.size() < 2 || cluster[0] != PlaceholderCodePointHigh || cluster[1] != PlaceholderCodePointLow)
    {
        return false;
    }
    const auto colorId = [](const TextColor color) noexcept {
        if (color.IsRgb())
        {
            const auto rgb = color.GetRGB();
            return (static_cast<uint32_t>(GetRValue(rgb)) << 16) | (static_cast<uint32_t>(GetGValue(rgb)) << 8) | GetBValue(rgb);
        }
        return color.IsIndex256() ? static_cast<uint32_t>(color.GetIndex()) : 0u;
    };
    // The first two recognized diacritics in the cluster give row then column; an optional
    // third gives the most significant byte of a >24-bit image id (composed below).
    // idHighByte starts at -1 (absent) so a 4th+ diacritic cannot overwrite an explicit 3rd
    // diacritic of index 0 (the spec ignores extras); absent or 0 leaves a plain 24-bit id.
    auto rowDiacritic = -1;
    auto colDiacritic = -1;
    auto idHighByte = -1;
    for (auto j = size_t{ 2 }; j < cluster.size(); ++j)
    {
        // A row/col diacritic may be an astral combining mark (index >= 283 in the
        // rowcolumn-diacritics table), stored as a UTF-16 surrogate pair; decode it to a
        // full codepoint before the lookup so large grids address the right row/column.
        auto cp = static_cast<char32_t>(cluster[j]);
        if (til::is_leading_surrogate(cluster[j]) && j + 1 < cluster.size() && til::is_trailing_surrogate(cluster[j + 1]))
        {
            cp = til::combine_surrogates(cluster[j], cluster[j + 1]);
            ++j;
        }
        if (const auto idx = _PlaceholderDiacriticIndex(cp); idx >= 0)
        {
            if (rowDiacritic < 0)
            {
                rowDiacritic = idx;
            }
            else if (colDiacritic < 0)
            {
                colDiacritic = idx;
            }
            else if (idHighByte < 0)
            {
                idHighByte = idx;
            }
        }
    }
    const auto attributes = row.GetAttrByColumn(column);
    const auto fg = attributes.GetForeground();
    if (!(fg.IsRgb() || fg.IsIndex256()) || idHighByte > 255)
    {
        return false;
    }
    const auto imageIdLow = colorId(fg);

    // Kitty's omission rules are positional:
    //  * no diacritics: inherit row, left column + 1, and high byte;
    //  * row only: inherit column + 1/high byte only when the rows match;
    //  * row+column: inherit only the high byte when the coordinates are adjacent.
    // Every inheritance case also requires matching foreground image-id and underline
    // placement-id color values. At column 0 or after any failed gate, omitted values
    // remain zero.
    auto cellRow = rowDiacritic >= 0 ? static_cast<uint32_t>(rowDiacritic) : 0u;
    auto cellCol = colDiacritic >= 0 ? static_cast<uint32_t>(colDiacritic) : 0u;
    auto highByte = idHighByte >= 0 ? static_cast<uint8_t>(idHighByte) : uint8_t{ 0 };
    const auto left = column > 0 ? row.GetImageCellRef(column - 1) : nullptr;
    const auto leftAttributes = left ? row.GetAttrByColumn(column - 1) : TextAttribute{};
    const auto attributesMatch = left &&
                                 colorId(leftAttributes.GetForeground()) == imageIdLow &&
                                 colorId(leftAttributes.GetUnderlineColor()) == colorId(attributes.GetUnderlineColor());
    if (rowDiacritic < 0)
    {
        if (attributesMatch)
        {
            cellRow = left->row;
            cellCol = static_cast<uint32_t>(left->column) + 1;
            highByte = left->imageIdHighByte;
        }
    }
    else if (colDiacritic < 0)
    {
        if (attributesMatch && left->row == cellRow)
        {
            cellCol = static_cast<uint32_t>(left->column) + 1;
            highByte = left->imageIdHighByte;
        }
    }
    else if (idHighByte < 0)
    {
        if (attributesMatch && left->row == cellRow && static_cast<uint32_t>(left->column) + 1 == cellCol)
        {
            highByte = left->imageIdHighByte;
        }
    }

    // Compose the effective id only after resolving an omitted high byte from the
    // left cell. A non-zero byte selects a >24-bit image; a missing image/placement
    // draws nothing but the resolved cell metadata remains available to its right.
    const auto imageId = highByte > 0 ? (imageIdLow | (static_cast<uint32_t>(highByte) << 24)) : imageIdLow;
    const auto placementId = colorId(attributes.GetUnderlineColor());
    const auto placement = _virtualIds.find({ imageId, placementId });
    const auto imageEntry = _images.find(imageId);
    const auto layerId = placement != _virtualIds.end() ? placement->second.layerId : 0;
    auto drawn = false;
    if (placement != _virtualIds.end() && imageEntry != _images.end())
    {
        const auto& place = placement->second;
        drawn = _placeImageCellRef(imageEntry->second, imageId, column, screenRow, cellRow, cellCol, place, fragments);
        if (drawn)
        {
            imageEntry->second.hasRenderedPlacements = true;
        }
    }
    // A layer id is only recorded once that layer has actually received this
    // cell's pixels. A placeholder outside the placement grid draws nothing, and
    // claiming a layer it has no pixels in would tell reflow to carry across a
    // region the layer does not cover. The grid coordinates are recorded either
    // way, because the cell to the right resolves its own column and image-id
    // high byte from them.
    row.SetImageCellRef(column, ImageCellRef{
                                    .layerId = drawn ? layerId : 0,
                                    .column = cellCol,
                                    .row = gsl::narrow_cast<uint16_t>(cellRow),
                                    .imageIdHighByte = highByte,
                                    .valid = true,
                                });
    return drawn;
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
