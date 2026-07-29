// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "KittyParser.hpp"
#include "adaptDispatch.hpp"
#include "../../inc/unicode.hpp"
#include <til/unicode.h>
#include "../../types/inc/Viewport.hpp"
#include "../parser/ascii.hpp"

// inflatelib (microsoft/inflatelib) provides the RFC 1951 DEFLATE decoder backing the
// Kitty graphics o=z (zlib) path. It handles raw Deflate only, so the surrounding RFC
// 1950 zlib header and Adler-32 trailer are validated here.
#include <inflatelib.hpp>

using namespace Microsoft::Console::Types;
using namespace Microsoft::Console::VirtualTerminal;
using Microsoft::Console::Utils::ReadSharedMemoryResult;

KittyParser::KittyParser(AdaptDispatch& dispatcher) noexcept :
    _dispatcher{ dispatcher }
{
}

// A hard reset drops everything: the images, the placements, the state saved for
// the main buffer, and any transfer that was in progress.
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
    _scheduleAnimationTimer();
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
    RefreshImageSurfaces();
    _scheduleAnimationTimer();
}

// Handles the Kitty graphics protocol (APC G <control>;<payload> ST). The parser
// has already consumed the 'G' identifier and routed us here on the strength of it.
// The control block (key=value pairs up to ';') is accumulated, and the base64
// payload after ';' is collected (bounded). Both are parsed on ESC. The handler is
// exception-safe: any failure declines the rest.
//
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/
ITermDispatch::StringHandler KittyParser::DefineImage()
{
    return [this, control = std::wstring{}, payload = std::string{}, inControl = true, controlValid = true, payloadValid = true, payloadTooLarge = false](const auto ch) mutable noexcept -> bool {
        try
        {
            if (ch == AsciiChars::CAN || ch == AsciiChars::SUB)
            {
                // A CAN/SUB aborted this APC; StateMachine::_ActionInterrupt reports
                // the aborting character itself. Discard any cross-sequence Kitty
                // chunk state so a later bare 'm=' cannot finalize this aborted
                // transfer, and decline without processing the partial payload.
                // A real APC data byte can never be 0x18 or 0x1A because those
                // characters are what trigger the abort.
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
                // The control block is bounded, but the excess cannot simply be dropped:
                // truncating "i=1234" to "i=12" produces a different command that is still
                // perfectly valid, so the terminal would act on something the application
                // never asked for. An over-long block is refused instead, the same way an
                // over-long payload is. No legal control block comes close to this bound.
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
                // The payload is base64 (ASCII only); anything else is invalid.
                payloadValid = false;
            }
            else if (payload.size() < MaxPayload)
            {
                payload.push_back(static_cast<char>(ch));
            }
            else
            {
                // The payload exceeds MaxPayload; flag it distinctly so the command
                // is reported as EFBIG (too large) rather than EINVAL (malformed).
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

// Parses a Kitty graphics control block (comma-separated key=value pairs) into a
// Control. Keys are single characters with a non-empty value; a zero or empty
// id/number is treated as unspecified.
// Protocol (control key reference): https://sw.kovidgoyal.net/kitty/graphics-protocol/#control-data-reference
KittyParser::Control KittyParser::_ParseControl(const std::wstring_view control) noexcept
{
    Control c;
    size_t pos = 0;
    while (pos <= control.size())
    {
        const auto comma = control.find(L',', pos);
        const auto end = (comma == std::wstring_view::npos) ? control.size() : comma;
        const auto pair = control.substr(pos, end - pos);
        const auto eq = pair.find(L'=');
        // Keys are single characters with a non-empty value (e.g. "i=5").
        if (eq == 1 && pair.size() > 2)
        {
            const auto key = pair.front();
            const auto value = pair.substr(eq + 1);
            if (key != L'm' && key != L'q')
            {
                // Per the kitty chunked-transmission spec, a continuation chunk may
                // carry both 'm' and 'q' (quiet); neither marks a fresh command.
                c.hasNonChunkKey = true; // distinguishes a fresh command from an 'm='/'q=' continuation
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
                c.upperX = c.cellOffsetX;
                break;
            case L'Y':
                c.cellOffsetY = _ParseUint(value);
                c.upperY = c.cellOffsetY;
                break;
            case L'O':
                // Uppercase O is the byte offset into a transmitted file (t=f / t=t);
                // lowercase keys s/v are the pixel width/height, so case matters here.
                // Parsed as 64-bit so a real >4 GiB offset is preserved, not truncated.
                c.fileOffset = _ParseUint64(value);
                break;
            case L'S':
                // Uppercase S is the number of file bytes to read (0 = to EOF), 64-bit so
                // a large request is not silently wrapped (the host caps the actual read).
                c.fileSize = _ParseUint64(value);
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
            case L'U':
                c.virtualPlacement = _ParseUint(value) != 0;
                break;
            case L'p': // placement id: one display of an image (ignored when imageId==0)
                c.placementId = _ParseUint(value);
                c.havePlacementId = true;
                break;
            case L'P': // parent image id: position this placement relative to that image's placement
                c.parentImageId = _ParseUint(value);
                c.haveParent = true;
                break;
            case L'Q': // parent placement id (with P) identifying the exact parent placement
                c.parentPlacementId = _ParseUint(value);
                break;
            case L'H': // signed horizontal cell offset from the parent anchor (+right / -left)
                c.offsetH = _ParseInt(value);
                break;
            case L'V': // signed vertical cell offset from the parent anchor (+down / -up)
                c.offsetV = _ParseInt(value);
                break;
            case L'z': // signed z-index: negative placements render below text
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

    // A zero (or empty) id/number means "unspecified" in the kitty protocol.
    c.haveId = c.haveId && c.imageId != 0;
    c.haveNumber = c.haveNumber && c.imageNumber != 0;
    // A placement id of 0 means "no placement id" (valid ids are 1..4294967295).
    c.havePlacementId = c.havePlacementId && c.placementId != 0;
    return c;
}

// Dispatches a Kitty graphics command. A chunked transmission (m=1) accumulates its
// base64 payload across sequences and is processed only when the final chunk (m=0)
// arrives, using the control from the first chunk. Only one transfer runs at a time.
void KittyParser::_HandleSequence(const std::wstring_view control, const std::string_view payload, const bool controlValid, const bool payloadValid, const bool payloadTooLarge)
{
    const auto command = _ParseControl(control);
    const auto isSharedMemory = command.medium == L's';

    // A control block that hit the length bound was truncated, so none of the keys parsed
    // out of it can be trusted -- not the action, not the image id, and not the quiet level.
    // Acting on it would mean acting on a command the application never sent, so it is
    // refused outright. Any in-progress chunked transfer goes too, because a truncated
    // control cannot be trusted to say whether it continues one.
    if (!controlValid)
    {
        _clearChunk();
        if (command.quiet < 2)
        {
            _dispatcher._ReturnApcResponse(L"G;EINVAL:control block too long");
        }
        return;
    }

    // Only a bare 'm='-prefixed sequence continues an in-progress transfer. Any other
    // command starts fresh, so if a transfer is already active it was orphaned (e.g.
    // its APC was aborted mid-stream) and its stale state must be discarded first.
    const auto repeatsActiveAction = _chunkActive &&
                                     command.action == _chunkControl.action &&
                                     !command.hasNonChunkKeyOtherThanAction;
    // Per the protocol, m= applies only to direct transmission and is ignored for
    // shared-memory media, whose payload is already just a local resource name.
    const auto isContinuation = command.mPresent && !isSharedMemory && (!command.hasNonChunkKey || repeatsActiveAction);
    if (_chunkActive && !isContinuation)
    {
        _clearChunk();
    }

    if (_chunkActive || (command.moreChunks && !isSharedMemory))
    {
        if (!_chunkActive)
        {
            _chunkActive = true;
            _chunkControl = command;
            _chunkPayload.clear();
            _chunkPayloadValid = true;
            _chunkPayloadTooLarge = false;
        }

        // Accumulate this chunk's payload, bounded in total by MaxPayload.
        _chunkPayloadValid = _chunkPayloadValid && payloadValid;
        _chunkPayloadTooLarge = _chunkPayloadTooLarge || payloadTooLarge;

        // The kitty spec allows the quiet (q) setting on any chunk, including the
        // final one; carry a non-default value forward so it governs the assembled
        // command's acknowledgement.
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
            return; // more chunks to come; no response until completion
        }

        // Final chunk: assemble and process using the first chunk's control.
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

// t=f, t=t, and t=s hand the terminal a name -- a file path, a temporary file to read
// and then delete, or a shared memory object -- that came off the output stream, and ask
// it to go touch the machine. Only the host knows whether that is acceptable for a given
// session, so it is an opt-in capability rather than a protocol constant. Inline t=d
// carries its own bytes and needs no permission.
bool KittyParser::_localMediaAllowed() const noexcept
{
    return _dispatcher._optionalFeatures.test(ITermDispatch::OptionalFeature::KittyLocalMedia);
}

// Validates and applies a fully-assembled Kitty graphics command, then emits the
// acknowledgement. Ids are re-emitted as decimal only. Actions: a=t/T transmit
// (and display), a=p put/display, a=q query, a=d delete, and a=f/a/a/c animation.
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#display-images-on-screen
// and https://sw.kovidgoyal.net/kitty/graphics-protocol/#deleting-images
// and https://sw.kovidgoyal.net/kitty/graphics-protocol/#animation
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
    auto assignedId = imageId; // id to report back (auto-assigned for transmit-by-number)

    // Displays an image for a put/transmit-and-display, honoring relative placements
    // (P=/Q=/H=/V=). For a relative placement it resolves the parent's absolute anchor,
    // offsets by (H, V), draws at that anchor without moving the cursor, and records the
    // placement; on a resolution error it sets success/code and draws nothing. For a normal
    // placement it draws at the cursor (honoring C) and records a placement when p= is given.
    // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
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
            // (Virtual + relative is rejected before this lambda runs, at the a=T and a=p sites;
            // this lambda only ever executes for non-virtual placements.)
            std::wstring_view resolveCode = L"OK";
            const auto parentAnchor = _resolvePlacementAnchor(command.parentImageId, command.parentPlacementId, { targetImageId, placementId }, resolveCode);
            if (!parentAnchor)
            {
                // Resolution failed (ECYCLE/ENOPARENT/ETOODEEP): draw nothing and, crucially,
                // leave the prior (targetImageId, placementId) placement intact -- we have not
                // touched the registry or any cells yet.
                success = false;
                code = resolveCode;
                return;
            }
            // childAnchor = parentAnchor + (H, V) in cells, clamped to the page.
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
            // Draw at the resolved anchor; the cursor must NOT move for a relative placement.
            til::size drawn;
            try
            {
                drawn = _placeImage(image, false, targetImageId, layerId, command.cols, command.rows, command.srcX, command.srcY, command.srcW, command.srcH, command.cellOffsetX, command.cellOffsetY, command.zIndex, childAnchor);
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
        // Normal (non-relative) placement: anchor at the cursor, honoring C.
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
            drawn = _placeImage(image, moveCursor, targetImageId, layerId, command.cols, command.rows, command.srcX, command.srcY, command.srcW, command.srcH, command.cellOffsetX, command.cellOffsetY, command.zIndex);
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
        // An image id and an image number are mutually exclusive.
        success = false;
        code = L"EINVAL:i and I are mutually exclusive";
    }
    else
    {
        switch (action)
        {
        case L't': // transmit
        case L'T': // transmit and display
        case L'q': // query: validate the request the same way but do not store
        {
            std::vector<uint8_t> bytes;
            if (format != 24 && format != 32 && format != 100)
            {
                success = false;
                code = L"EINVAL:unsupported format";
                break;
            }
            if (medium != L'd' && medium != L'f' && medium != L't' && medium != L's')
            {
                // t=d (direct), t=f (file), t=t (temporary file), and t=s (shared
                // memory) are supported. Reject every unrecognized medium.
                success = false;
                code = L"EINVAL:unsupported transmission medium";
                break;
            }
            if (medium != L'd' && !_localMediaAllowed())
            {
                // t=f, t=t, and t=s do not carry the image: they carry the NAME of a
                // local resource, chosen by whatever is writing to the terminal, that we
                // are then asked to open -- and for t=t, to delete. That is off unless
                // the host opts in. Refuse all three with the code an unrecognized medium
                // gets, before the payload is even decoded, so nothing is looked up or
                // deleted and the refusal reveals nothing about the machine.
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
                // Only o=z (zlib/DEFLATE) compression is defined by the protocol;
                // reject any other (undefined) selector rather than guess.
                // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#compression
                success = false;
                code = L"EINVAL:unsupported compression";
                break;
            }
            if (!payloadValid || !_DecodeBase64(payload, bytes))
            {
                success = false;
                if (payloadTooLarge)
                {
                    // An oversize payload is reported as EFBIG (too large) and nothing is
                    // stored; only a genuinely malformed payload gets EINVAL.
                    code = L"EFBIG:payload exceeds maximum size";
                }
                else
                {
                    code = L"EINVAL:bad payload";
                }
                break;
            }

            // File and shared-memory payloads carry a UTF-8 resource name rather than
            // image bytes. Decode strictly and reject embedded NULs so Win32 cannot
            // silently open a truncated name.
            const auto decodeResourceName = [](const std::vector<uint8_t>& encodedName, const size_t maxBytes, std::wstring& result) {
                if (encodedName.empty() || encodedName.size() > maxBytes ||
                    std::find(encodedName.begin(), encodedName.end(), uint8_t{ 0 }) != encodedName.end())
                {
                    return false;
                }

                const std::string utf8(encodedName.begin(), encodedName.end());
                const auto length = static_cast<int>(utf8.size());
                const auto needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
                if (needed <= 0)
                {
                    return false;
                }

                result.resize(static_cast<size_t>(needed));
                if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, result.data(), needed) != needed)
                {
                    result.clear();
                    return false;
                }
                return true;
            };

            if (medium == L'f' || medium == L't')
            {
                // For file/temporary transmission the decoded payload is not image
                // data but the image FILE PATH (UTF-8 bytes). Read the file contents
                // into `bytes` so the format-specific validation/decode below treats
                // it exactly like a direct payload. The host bounds the read to a safe
                // size (MaxPayload), so a hostile S= cannot force a huge alloc, and
                // restricts reads to local fixed-drive files (see ReadLocalFile).
                //
                // A temporary file (t=t) is deleted by the host after a successful read,
                // but only when it resides under the system temp directory and carries the
                // marker below. Kitty names its temporary files "tty-graphics-protocol-*",
                // and this is the only layer that knows that, so this is where the marker
                // lives. A query (a=q) validates the request without storing, so it must
                // NOT delete: only a real transmit (a=t / a=T) of a t=t file requests it.
                //
                // Protocol (transmission media): https://sw.kovidgoyal.net/kitty/graphics-protocol/#transferring-data
                //
                // Convert the UTF-8 path with MB_ERR_INVALID_CHARS so a malformed path is
                // rejected here (path stays empty -> EBADF below) rather than silently
                // mangled into U+FFFD substitutions: unlike til::u8u16, which uses no
                // flags and substitutes, this rejects invalid input deterministically.
                static constexpr std::wstring_view temporaryFileMarker{ L"tty-graphics-protocol" };

                std::wstring path;
                if (!decodeResourceName(bytes, static_cast<size_t>(INT_MAX), path))
                {
                    // The payload never named a file, so nothing was looked up. This says
                    // only that the request itself was malformed.
                    success = false;
                    code = L"EINVAL:invalid image file request";
                    break;
                }
                const auto deleteAfter = (medium == L't') && (action != L'q');
                std::vector<uint8_t> fileBytes;
                if (_dispatcher._api.ReadLocalFile(path, command.fileOffset, command.fileSize, deleteAfter, temporaryFileMarker, fileBytes) != til::read_file_result::ok)
                {
                    // Every failure from here on reports the same code. Separating "no such
                    // file" from "exists but could not be read" from "rejected before we
                    // opened it" would answer, one path per sequence, questions about the
                    // machine that the writer is not entitled to ask.
                    success = false;
                    code = L"EBADF:could not read file";
                    break;
                }
                bytes = std::move(fileBytes);
            }
            else if (medium == L's')
            {
                // The payload names a Windows file mapping. The host accepts only the
                // current-session namespace, opens FILE_MAP_READ, copies at most 32 MiB,
                // and closes the view/handle before returning. Per the Kitty spec there
                // is no unlink operation on Windows.
                std::wstring name;
                constexpr size_t maxMappingNameBytes = 32767;
                if (!decodeResourceName(bytes, maxMappingNameBytes, name))
                {
                    success = false;
                    code = L"EINVAL:invalid shared memory request";
                    break;
                }

                auto readSize = command.fileSize;
                if (readSize == 0 && compression != L'z' && (format == 24 || format == 32) && width != 0 && height != 0)
                {
                    // CreateFileMapping rounds section sizes to whole pages, so a mapping
                    // has no discoverable byte-exact tail. For raw pixels the protocol's
                    // dimensions provide that exact length when S= is omitted.
                    const auto depth = format == 24 ? 3ull : 4ull;
                    const auto area = static_cast<uint64_t>(width) * height;
                    readSize = area <= UINT64_MAX / depth ? area * depth : UINT64_MAX;
                }

                std::vector<uint8_t> sharedBytes;
                if (_dispatcher._api.ReadSharedMemory(name, command.fileOffset, readSize, sharedBytes) != ReadSharedMemoryResult::ok)
                {
                    // Collapsed for the same reason as the file media above: whether a named
                    // mapping exists, and whether it could be opened, are not facts this
                    // protocol gets to report back to whoever asked.
                    success = false;
                    code = L"EBADF:could not read shared memory";
                    break;
                }
                bytes = std::move(sharedBytes);
            }
            if (compression == L'z')
            {
                // o=z: the bytes acquired above (from t=d base64, a t=f / t=t file,
                // or t=s shared memory) are a zlib (RFC 1950) stream wrapping the real
                // f=24/32 pixels or an f=100 PNG. Inflate here -- after the medium
                // branch, before format-specific validation/decode -- so everything
                // downstream sees the uncompressed bytes regardless of how they arrived.
                // The inflated size is bounded to MaxPayload, so a decompression
                // bomb is rejected without ever allocating the expanded buffer.
                std::vector<uint8_t> inflated;
                const auto allowSharedMemoryPadding = medium == L's' && command.fileSize == 0;
                if (!_inflateZlib(bytes, inflated, MaxPayload, allowSharedMemoryPadding))
                {
                    success = false;
                    code = L"EINVAL:invalid compressed data";
                    break;
                }
                bytes = std::move(inflated);
            }
            // Raw pixel formats (f=24/32) require positive dimensions and an exact
            // payload of width * height * depth bytes; compare via division so hostile
            // dimensions cannot overflow. (Any o=z payload has already been inflated
            // above, so `bytes` is the raw/decoded representation at this point.)
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
            // Query (a=q) validates only; transmit decodes and stores the pixels, assigns
            // a fresh id (a number always yields a new id), and for a=T displays the image.
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
                else if (format == 100)
                {
                    // f=100 is PNG; the host decodes it to premultiplied BGRA. An empty
                    // or undecodable payload is an error, not a silently-empty image.
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
                    if (decodedSuccessfully)
                    {
                        image.width = static_cast<uint32_t>(decodedSize.width);
                        image.height = static_cast<uint32_t>(decodedSize.height);
                        image.pixels = std::make_shared<std::vector<RGBQUAD>>(std::move(decoded));
                    }
                    else
                    {
                        success = false;
                        code = L"EBADPNG:could not decode image";
                        break;
                    }
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
                    // Virtual (U=1): store the image and its grid geometry; the pixels are
                    // drawn later by Unicode placeholders, not at the cursor. (U=1 with a
                    // parent was already rejected before anything was registered.)
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
        case L'p': // put: display the referenced image
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
                const auto rev = _imageNumbers.find(imageNumber);
                if (rev != _imageNumbers.end())
                {
                    const auto it = _images.find(rev->second);
                    if (it != _images.end())
                    {
                        target = &it->second;
                        targetId = rev->second;
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
            }
            break;
        }
        case L'f': // transmit animation frame
        case L'a': // control animation
        case L'c': // compose animation frames
        {
            uint32_t targetId = 0;
            if (haveId)
            {
                targetId = imageId;
            }
            else if (haveNumber)
            {
                const auto it = _imageNumbers.find(imageNumber);
                if (it != _imageNumbers.end())
                {
                    targetId = it->second;
                }
            }
            if (targetId == 0)
            {
                success = false;
                code = L"ENOENT:image not found";
            }
            else if (action == L'f')
            {
                success = _processAnimationFrame(command, payload, payloadValid, payloadTooLarge, targetId, code);
            }
            else if (action == L'a')
            {
                success = _processAnimationControl(command, targetId, code);
            }
            else
            {
                success = _processFrameComposition(command, targetId, code);
            }
            break;
        }
        case L'd': // delete
        {
            // Lowercase d= selectors delete placements + on-screen pixels but KEEP the image data
            // (so a later a=p re-displays without re-transmitting); the UPPERCASE variant also frees
            // the image data once the image has no placements left.
            const auto freeData = (deleteTarget >= L'A' && deleteTarget <= L'Z');
            switch (deleteTarget)
            {
            case L'a': // all images (the default when d= is omitted)
            case L'A':
                _deleteAllPlacements(freeData);
                break;
            case L'i': // by image id
            case L'I':
                if (haveId)
                {
                    if (command.havePlacementId)
                    {
                        // Spec: with a p key, delete only the (imageId, placementId) placement.
                        _deletePlacement(imageId, command.placementId, freeData);
                    }
                    else
                    {
                        // Cascade to any relative children before removing the image itself.
                        _erasePlacementsForImage(imageId);
                        // A virtual (U=1) placement is itself a placement, deleted by i/I/n/N/r/R
                        // regardless of case (spec); only the image DATA free is case-gated, so drop
                        // the virtual grid here so a later placeholder doesn't re-render it.
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
                else
                {
                    success = false;
                    code = L"EINVAL:delete by id requires i";
                }
                break;
            case L'n': // by image number
            case L'N':
                if (haveNumber)
                {
                    const auto it = _imageNumbers.find(imageNumber);
                    if (it != _imageNumbers.end())
                    {
                        const auto targetId = it->second;
                        if (command.havePlacementId)
                        {
                            // Spec: with a p key, delete only the (imageId, placementId) placement.
                            _deletePlacement(targetId, command.placementId, freeData);
                        }
                        else
                        {
                            _erasePlacementsForImage(targetId);
                            std::erase_if(_virtualIds, [&](const auto& entry) {
                                return entry.first.first == targetId;
                            });
                            if (freeData)
                            {
                                _eraseImage(targetId);
                            }
                            _eraseImagePlacements(targetId);
                        }
                    }
                }
                else
                {
                    success = false;
                    code = L"EINVAL:delete by number requires I";
                }
                break;
            case L'c': // placements intersecting the current cursor cell
            case L'C':
            {
                auto page = _dispatcher._pages.ActivePage();
                const auto cursor = page.Cursor().GetPosition();
                _deleteImagesIntersecting(cursor.x, cursor.y, cursor.x + 1, cursor.y + 1, freeData);
                break;
            }
            case L'p': // placements intersecting the cell at (x, y) [x/y 1-based, viewport-relative]
            case L'P':
                if (command.srcX != 0 && command.srcY != 0)
                {
                    auto page = _dispatcher._pages.ActivePage();
                    // Protocol x/y are 1-based and viewport-relative; on-screen owner cells are
                    // buffer-absolute, so the row is offset by the viewport top (page.Top()). int64
                    // math + clamps keep a hostile x/y from overflowing til::CoordType.
                    const auto px = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(command.srcX) - 1, page.Width()));
                    const auto py = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(page.Top()) + command.srcY - 1, page.Bottom()));
                    _deleteImagesIntersecting(px, py, px + 1, py + 1, freeData);
                }
                break;
            case L'x': // placements intersecting column x [1-based, viewport-relative]
            case L'X':
                if (command.srcX != 0)
                {
                    auto page = _dispatcher._pages.ActivePage();
                    const auto col = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(command.srcX) - 1, page.Width()));
                    _deleteImagesIntersecting(col, page.Top(), col + 1, page.Bottom(), freeData);
                }
                break;
            case L'y': // placements intersecting row y [1-based, viewport-relative]
            case L'Y':
                if (command.srcY != 0)
                {
                    auto page = _dispatcher._pages.ActivePage();
                    const auto rowY = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(page.Top()) + command.srcY - 1, page.Bottom()));
                    _deleteImagesIntersecting(0, rowY, page.Width(), rowY + 1, freeData);
                }
                break;
            case L'r': // images with id in the inclusive range [x, y]
            case L'R':
                _deleteImagesInIdRange(command.srcX, command.srcY, freeData);
                break;
            case L'z': // physical placements with the exact z-index
            case L'Z':
                _deletePlacementsByZ(command.zIndex, freeData);
                break;
            case L'q': // physical placements with the exact z-index intersecting cell (x, y)
            case L'Q':
                if (command.srcX != 0 && command.srcY != 0)
                {
                    auto page = _dispatcher._pages.ActivePage();
                    const auto x = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(command.srcX) - 1, page.Width()));
                    const auto y = static_cast<til::CoordType>(std::min<int64_t>(static_cast<int64_t>(page.Top()) + command.srcY - 1, page.Bottom()));
                    _deletePlacementsByZ(command.zIndex, freeData, til::point{ x, y });
                }
                break;
            case L'f': // delete animation frame r= (default: root frame)
            case L'F':
            {
                uint32_t targetId = 0;
                if (haveId)
                {
                    targetId = imageId;
                }
                else if (haveNumber)
                {
                    const auto it = _imageNumbers.find(imageNumber);
                    if (it != _imageNumbers.end())
                    {
                        targetId = it->second;
                    }
                }
                if (targetId != 0)
                {
                    _deleteAnimationFrames(targetId, command.rows == 0 ? 1 : command.rows, freeData);
                }
                break;
            }
            default:
                success = false;
                code = L"EINVAL:unsupported delete target";
                break;
            }
            break;
        }
        default: // unrecognized action
            success = false;
            code = L"EINVAL:unknown action";
            break;
        }
    }

    // Quiet mode: q=1 suppresses success replies; q>=2 suppresses everything.
    if (success && quiet >= 1)
    {
        return;
    }
    if (!success && quiet >= 2)
    {
        return;
    }

    // Success replies are only sent when the client referenced the image by id or
    // number; an anonymous success is silent, except a query, which always answers.
    // Errors are always reported.
    if (success && !haveId && !haveNumber && action != L'q')
    {
        return;
    }

    // Echo the id and/or number. When only a number was given, the (possibly
    // auto-assigned) id is reported so the client can reference the image later.
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
    // Echo the placement id when one was given (and an image was referenced), so the client
    // can correlate the response with the (imageId, placementId) it sent. p is ignored when
    // no image id/number is present (i=0).
    if (command.havePlacementId && (haveId || haveNumber))
    {
        response += fmt::format(FMT_COMPILE(L",p={}"), command.placementId);
    }
    response.push_back(L';');
    response.append(code);
    _dispatcher._ReturnApcResponse(response);
}

// Parses a non-negative decimal integer from a Kitty control value, clamped to the
// uint32 range. Parsing stops at the first non-digit. Always noexcept.
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

// Parses a signed decimal integer from a Kitty control value (e.g. H=/V= cell offsets),
// clamped to the int32 range. An optional leading '-' negates the magnitude; parsing stops
// at the first non-digit. Always noexcept.
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
        // Clamp the negative magnitude to INT32_MIN.
        return magnitude >= 0x80000000u ? INT32_MIN : -static_cast<int32_t>(magnitude);
    }
    return magnitude > 0x7FFFFFFFu ? INT32_MAX : static_cast<int32_t>(magnitude);
}

// Like _ParseUint but for the 64-bit file offset/size keys (O=/S=). Clamps to
// UINT64_MAX on overflow so a hostile run of digits cannot wrap; the host bounds the
// actual read and rejects an offset past EOF, so a clamped value just fails cleanly.
uint64_t KittyParser::_ParseUint64(const std::wstring_view value) noexcept
{
    uint64_t result = 0;
    for (const auto ch : value)
    {
        if (ch < L'0' || ch > L'9')
        {
            break;
        }
        const auto digit = static_cast<uint64_t>(ch - L'0');
        // Detect overflow before it happens: clamp rather than wrap.
        if (result > (UINT64_MAX - digit) / 10)
        {
            result = UINT64_MAX;
            break;
        }
        result = result * 10 + digit;
    }
    return result;
}

// Returns an unused image id, skipping ids that are already registered.
uint32_t KittyParser::_assignImageId()
{
    while (_images.find(_nextImageId) != _images.end() || _nextImageId == 0)
    {
        ++_nextImageId;
    }
    return _nextImageId++;
}

// Registers (or replaces) an image id, optionally cross-referenced by number.
// The registry is bounded; the oldest entry is evicted past MaxImages. The
// reverse number->id map is kept consistent when an id's number changes.
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
        _erasePlacementsForImage(victimId);
        _eraseImagePlacements(victimId);
        _eraseImage(victimId);
    }

    // Re-transmitting an id replaces its pixels and invalidates all prior placements.
    _eraseImagePlacements(id);
    _erasePlacementsForImage(id);
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

// Removes an image id and any number/order references to it.
void KittyParser::_eraseImage(const uint32_t id)
{
    const auto it = _images.find(id);
    if (it == _images.end())
    {
        return;
    }
    _totalPixelBytes -= it->second.PixelBytes();
    // If this image was the one the host is waiting on, the deadline it was told
    // about is about to name an image that no longer exists, so it has to be
    // recomputed. Nothing changes when a still image goes, and this runs inside
    // loops that drop many at once, so only pay for it when it can matter.
    const auto wasAnimating = it->second.animationState == 2 || it->second.animationState == 3;
    // Only drop the reverse entry if it still points at this id (a newer image
    // may have taken over the number).
    if (it->second.number != 0)
    {
        const auto rev = _imageNumbers.find(it->second.number);
        if (rev != _imageNumbers.end() && rev->second == id)
        {
            _imageNumbers.erase(rev);
        }
    }
    _images.erase(it);
    std::erase_if(_virtualIds, [&](const auto& entry) {
        return entry.first.first == id;
    });
    // Drop this id's own placement entries so the registry doesn't leak when an image is
    // evicted by the LRU. (Cascade-to-children is handled by _erasePlacementsForImage
    // on an explicit delete; a bare eviction just releases this id's entries.)
    for (auto pit = _placements.begin(); pit != _placements.end();)
    {
        pit = pit->first.first == id ? _placements.erase(pit) : std::next(pit);
    }
    // Likewise drop anonymous placements of this id (no external placement key of their own).
    _anonymousPlacements.erase(
        std::remove_if(_anonymousPlacements.begin(), _anonymousPlacements.end(), [id](const Placement& p) noexcept { return p.imageId == id; }),
        _anonymousPlacements.end());
    // The eviction path always removes the front, so keep that common case O(1).
    if (!_imageOrder.empty() && _imageOrder.front() == id)
    {
        _imageOrder.pop_front();
    }
    else
    {
        _imageOrder.erase(std::remove(_imageOrder.begin(), _imageOrder.end(), id), _imageOrder.end());
    }
    if (wasAnimating)
    {
        _scheduleAnimationTimer();
    }
}

// Clears the entire Kitty image registry and its direct-renderer placements.
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
    _scheduleAnimationTimer();
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

// Discards any in-progress chunked transmission, releasing its payload buffer.
void KittyParser::_clearChunk() noexcept
{
    _chunkActive = false;
    _chunkPayloadValid = true;
    _chunkPayloadTooLarge = false;
    _chunkControl = {};
    _chunkPayload = {};
}

// Converts a direct-pixel payload (f=24 RGB or f=32 RGBA) into premultiplied BGRA
// RGBQUADs (the format the renderer expects). PNG (f=100) is not decoded here yet.
// Protocol (pixel formats): https://sw.kovidgoyal.net/kitty/graphics-protocol/#rgb-and-rgba-data
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
        RGBQUAD px{};
        if (depth == 4)
        {
            // Premultiply RGB by alpha (no-op when alpha is 255).
            const uint32_t a = bytes[base + 3];
            px.rgbRed = static_cast<BYTE>(bytes[base + 0] * a / 255);
            px.rgbGreen = static_cast<BYTE>(bytes[base + 1] * a / 255);
            px.rgbBlue = static_cast<BYTE>(bytes[base + 2] * a / 255);
            px.rgbReserved = static_cast<BYTE>(a);
        }
        else
        {
            px.rgbRed = bytes[base + 0];
            px.rgbGreen = bytes[base + 1];
            px.rgbBlue = bytes[base + 2];
            px.rgbReserved = 255;
        }
        pixels.push_back(px);
    }
    return pixels;
}

RGBQUAD KittyParser::_rgbaColor(const uint32_t rgba) noexcept
{
    const auto alpha = rgba & 0xffu;
    RGBQUAD pixel{};
    pixel.rgbRed = static_cast<BYTE>(((rgba >> 24) & 0xffu) * alpha / 255u);
    pixel.rgbGreen = static_cast<BYTE>(((rgba >> 16) & 0xffu) * alpha / 255u);
    pixel.rgbBlue = static_cast<BYTE>(((rgba >> 8) & 0xffu) * alpha / 255u);
    pixel.rgbReserved = static_cast<BYTE>(alpha);
    return pixel;
}

void KittyParser::_compositePixels(const std::span<RGBQUAD> destination, const std::span<const RGBQUAD> source, const bool replace) noexcept
{
    const auto count = std::min(destination.size(), source.size());
    for (size_t i = 0; i < count; ++i)
    {
        const auto& src = source[i];
        auto& dst = destination[i];
        if (replace)
        {
            dst = src;
            continue;
        }
        const auto inverseAlpha = 255u - src.rgbReserved;
        dst.rgbRed = static_cast<BYTE>(std::min(255u, static_cast<uint32_t>(src.rgbRed) + (static_cast<uint32_t>(dst.rgbRed) * inverseAlpha + 127u) / 255u));
        dst.rgbGreen = static_cast<BYTE>(std::min(255u, static_cast<uint32_t>(src.rgbGreen) + (static_cast<uint32_t>(dst.rgbGreen) * inverseAlpha + 127u) / 255u));
        dst.rgbBlue = static_cast<BYTE>(std::min(255u, static_cast<uint32_t>(src.rgbBlue) + (static_cast<uint32_t>(dst.rgbBlue) * inverseAlpha + 127u) / 255u));
        dst.rgbReserved = static_cast<BYTE>(std::min(255u, static_cast<uint32_t>(src.rgbReserved) + (static_cast<uint32_t>(dst.rgbReserved) * inverseAlpha + 127u) / 255u));
    }
}

size_t KittyParser::_frameCount(const Image& image) noexcept
{
    return !image.pixels || image.pixels->empty() ? 0 : image.animationFrames.size() + 1;
}

const std::vector<RGBQUAD>* KittyParser::_framePixels(const Image& image, const uint32_t frameNumber) noexcept
{
    if (frameNumber == 1)
    {
        return image.pixels.get();
    }
    const auto index = static_cast<size_t>(frameNumber) - 2;
    return index < image.animationFrames.size() ? image.animationFrames[index].pixels.get() : nullptr;
}

KittyParser::FramePixelStorage* KittyParser::_frameStorage(Image& image, const uint32_t frameNumber) noexcept
{
    if (frameNumber == 1)
    {
        return image.pixels ? &image.pixels : nullptr;
    }
    const auto index = static_cast<size_t>(frameNumber) - 2;
    return index < image.animationFrames.size() && image.animationFrames[index].pixels ?
               &image.animationFrames[index].pixels :
               nullptr;
}

const KittyParser::FramePixelStorage* KittyParser::_frameStorage(const Image& image, const uint32_t frameNumber) noexcept
{
    if (frameNumber == 1)
    {
        return image.pixels ? &image.pixels : nullptr;
    }
    const auto index = static_cast<size_t>(frameNumber) - 2;
    return index < image.animationFrames.size() && image.animationFrames[index].pixels ?
               &image.animationFrames[index].pixels :
               nullptr;
}

int32_t* KittyParser::_frameGap(Image& image, const uint32_t frameNumber) noexcept
{
    if (frameNumber == 1)
    {
        return &image.rootGapMilliseconds;
    }
    const auto index = static_cast<size_t>(frameNumber) - 2;
    return index < image.animationFrames.size() ? &image.animationFrames[index].gapMilliseconds : nullptr;
}

void KittyParser::_updateImageSurface(const uint32_t imageId, const FramePixelStorage& storage)
{
    const auto image = _images.find(imageId);
    if (image == _images.end() || !storage)
    {
        return;
    }
    const auto visiblePageNumber = _dispatcher._pages.VisiblePage().Number();
    auto surface = image->second.surface;
    auto foundPlacement = false;
    _dispatcher._pages.ForEachPage([&](const Page page) {
        auto& buffer = page.Buffer();
        auto firstRow = page.Bottom();
        auto lastRow = 0;
        for (const auto& placement : buffer.GetImages().All())
        {
            const auto key = placement.Identity();
            if (key.protocol == ImagePlacement::Key::Protocol::Kitty && key.imageId == imageId)
            {
                foundPlacement = true;
                surface = surface ? surface : placement.SurfacePointer();
                const auto bounds = placement.CellBounds();
                firstRow = std::min(firstRow, bounds.top);
                lastRow = std::max(lastRow, bounds.bottom - 1);
            }
        }
        if (page.Number() == visiblePageNumber && firstRow <= lastRow)
        {
            buffer.TriggerRedraw(Viewport::FromExclusive({ 0, firstRow, page.Width(), lastRow + 1 }));
        }
    });
    image->second.hasRenderedPlacements = foundPlacement;
    if (foundPlacement && surface)
    {
        image->second.surface = surface;
        surface->UpdatePixels(storage);
    }
    else
    {
        _releaseImageSurface(image->second);
    }
}

void KittyParser::RefreshImageSurfaces()
{
    for (const auto& [imageId, image] : _images)
    {
        if (image.animationFrames.empty())
        {
            continue;
        }
        if (const auto pixels = _frameStorage(image, image.presentedFrame))
        {
            _updateImageSurface(imageId, *pixels);
        }
    }
    _scheduleAnimationTimer();
}

bool KittyParser::_processAnimationFrame(const Control& command, const std::string_view payload, const bool payloadValid, const bool payloadTooLarge, const uint32_t imageId, std::wstring_view& code)
try
{
    auto imageIt = _images.find(imageId);
    if (imageIt == _images.end())
    {
        code = L"ENOENT:image not found";
        return false;
    }
    if (command.medium != L'd' || command.compression != 0)
    {
        code = command.medium != L'd' ? L"EINVAL:unsupported transmission medium" : L"EINVAL:unsupported compression";
        return false;
    }
    if (command.format != 24 && command.format != 32 && command.format != 100)
    {
        code = L"EINVAL:unsupported format";
        return false;
    }
    if (command.upperX > 1)
    {
        code = L"EINVAL:invalid frame composition mode";
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!payloadValid || !_DecodeBase64(payload, bytes))
    {
        code = payloadTooLarge ? L"EFBIG:payload exceeds maximum size" : L"EINVAL:bad payload";
        return false;
    }

    uint32_t frameWidth = 0;
    uint32_t frameHeight = 0;
    std::vector<RGBQUAD> framePixels;
    if (command.format == 24 || command.format == 32)
    {
        const auto depth = command.format == 24 ? 3u : 4u;
        if (command.width == 0 || command.height == 0)
        {
            code = L"EINVAL:missing frame dimensions";
            return false;
        }
        const auto area = static_cast<uint64_t>(command.width) * command.height;
        if (area > bytes.size() / depth || area * depth != bytes.size())
        {
            code = L"EINVAL:payload size mismatch";
            return false;
        }
        frameWidth = command.width;
        frameHeight = command.height;
        framePixels = _decodePixels(command.format, bytes);
    }
    else
    {
        til::size decodedSize;
        if (bytes.empty() ||
            !_dispatcher._api.DecodeImageToBgra(bytes, framePixels, decodedSize) ||
            decodedSize.width <= 0 || decodedSize.height <= 0 ||
            framePixels.size() != static_cast<size_t>(decodedSize.width) * decodedSize.height)
        {
            code = L"EBADPNG:could not decode frame";
            return false;
        }
        frameWidth = static_cast<uint32_t>(decodedSize.width);
        frameHeight = static_cast<uint32_t>(decodedSize.height);
    }

    auto& image = imageIt->second;
    if (command.srcX > image.width || command.srcY > image.height ||
        frameWidth > image.width - command.srcX || frameHeight > image.height - command.srcY)
    {
        code = L"EINVAL:frame rectangle out of bounds";
        return false;
    }

    const auto editFrame = command.rows;
    const auto backgroundFrame = command.cols;
    if (editFrame != 0 && backgroundFrame != 0)
    {
        code = L"EINVAL:cannot edit and create a frame simultaneously";
        return false;
    }
    if (editFrame == 0 && _frameCount(image) >= MaxFramesPerImage)
    {
        code = L"ENOSPC:frame count limit exceeded";
        return false;
    }
    if (editFrame != 0 && !_framePixels(image, editFrame))
    {
        code = L"ENOENT:frame not found";
        return false;
    }
    if (editFrame == 0 && backgroundFrame != 0 && !_framePixels(image, backgroundFrame))
    {
        code = L"ENOENT:background frame not found";
        return false;
    }

    const auto frameBytes = image.pixels->size() * sizeof(RGBQUAD);
    std::vector<uint32_t> victims;
    if (editFrame == 0)
    {
        // A victim whose placement is an ancestor of the target would cascade-delete
        // the image we are extending. Build the target's bounded parent ancestry once,
        // then exclude those image ids from the eviction plan.
        std::vector<uint32_t> protectedImageIds{ imageId };
        const auto protectAncestors = [&](std::pair<uint32_t, uint32_t> parent) {
            for (auto depth = 0; depth < MaxPlacementDepth; ++depth)
            {
                if (std::find(protectedImageIds.begin(), protectedImageIds.end(), parent.first) == protectedImageIds.end())
                {
                    protectedImageIds.push_back(parent.first);
                }
                const auto placement = _placements.find(parent);
                if (placement == _placements.end() || !placement->second.hasParent)
                {
                    break;
                }
                parent = { placement->second.parentImageId, placement->second.parentPlacementId };
            }
        };
        for (const auto& [key, placement] : _placements)
        {
            if (key.first == imageId && placement.hasParent)
            {
                protectAncestors({ placement.parentImageId, placement.parentPlacementId });
            }
        }
        for (const auto& placement : _anonymousPlacements)
        {
            if (placement.imageId == imageId && placement.hasParent)
            {
                protectAncestors({ placement.parentImageId, placement.parentPlacementId });
            }
        }

        const auto retainedBytes = _retainedPixelBytes();
        const auto requiredBytes = retainedBytes > MaxTotalBytes - frameBytes ?
                                       retainedBytes - (MaxTotalBytes - frameBytes) :
                                       size_t{ 0 };
        auto reclaimableBytes = size_t{ 0 };
        if (requiredBytes != 0)
        {
            for (const auto candidateId : _imageOrder)
            {
                if (std::find(protectedImageIds.begin(), protectedImageIds.end(), candidateId) != protectedImageIds.end())
                {
                    continue;
                }
                const auto candidate = _images.find(candidateId);
                if (candidate == _images.end())
                {
                    continue;
                }
                victims.push_back(candidateId);
                reclaimableBytes += candidate->second.PixelBytes();
                if (reclaimableBytes >= requiredBytes)
                {
                    break;
                }
            }
        }
        if (reclaimableBytes < requiredBytes)
        {
            code = L"ENOSPC:image storage limit exceeded";
            return false;
        }
    }

    auto canvas = std::vector<RGBQUAD>{};
    if (editFrame != 0)
    {
        canvas = *_framePixels(imageIt->second, editFrame);
    }
    else if (backgroundFrame != 0)
    {
        canvas = *_framePixels(imageIt->second, backgroundFrame);
    }
    else
    {
        canvas.assign(imageIt->second.pixels->size(), _rgbaColor(command.upperY));
    }

    for (uint32_t row = 0; row < frameHeight; ++row)
    {
        const auto srcOffset = static_cast<size_t>(row) * frameWidth;
        const auto dstOffset = static_cast<size_t>(command.srcY + row) * imageIt->second.width + command.srcX;
        _compositePixels(std::span{ canvas }.subspan(dstOffset, frameWidth),
                              std::span{ framePixels }.subspan(srcOffset, frameWidth),
                              command.upperX == 1);
    }

    auto appendedFrame = FramePixelStorage{};
    if (editFrame == 0)
    {
        // Allocate every fallible part of the append before eviction mutates unrelated
        // images. The subsequent frame move cannot allocate once capacity is reserved.
        appendedFrame = std::make_shared<std::vector<RGBQUAD>>(std::move(canvas));
        imageIt->second.animationFrames.reserve(imageIt->second.animationFrames.size() + 1);
        for (const auto victimId : victims)
        {
            if (_images.count(victimId) == 0)
            {
                continue;
            }
            _erasePlacementsForImage(victimId);
            _eraseImagePlacements(victimId);
            _eraseImage(victimId);
        }
        imageIt = _images.find(imageId);
        WI_ASSERT(imageIt != _images.end());
    }

    auto& storedImage = imageIt->second;
    uint32_t changedFrame = 0;
    if (editFrame != 0)
    {
        *_frameStorage(storedImage, editFrame) = std::make_shared<std::vector<RGBQUAD>>(std::move(canvas));
        changedFrame = editFrame;
        if (command.haveZ && command.zIndex != 0)
        {
            *_frameGap(storedImage, editFrame) = command.zIndex;
        }
    }
    else
    {
        AnimationFrame frame;
        frame.pixels = std::move(appendedFrame);
        frame.gapMilliseconds = command.haveZ && command.zIndex != 0 ? command.zIndex : 40;
        storedImage.animationFrames.push_back(std::move(frame));
        _totalPixelBytes += frameBytes;
        changedFrame = gsl::narrow_cast<uint32_t>(_frameCount(storedImage));
    }

    if (storedImage.presentedFrame == changedFrame)
    {
        _updateImageSurface(imageId, *_frameStorage(storedImage, changedFrame));
    }
    const auto now = std::chrono::steady_clock::now();
    if (storedImage.animationState == 2 && storedImage.waitingForFrames)
    {
        storedImage.waitingForFrames = false;
        _advanceImage(imageId, storedImage, now);
    }
    else if (storedImage.currentFrame == changedFrame &&
             command.haveZ && command.zIndex != 0 &&
             (storedImage.animationState == 2 || storedImage.animationState == 3))
    {
        _scheduleAnimation(imageId, storedImage, now);
    }
    _scheduleAnimationTimer();
    return true;
}
catch (const std::bad_alloc&)
{
    code = L"ENOSPC:could not allocate frame";
    return false;
}

bool KittyParser::_processAnimationControl(const Control& command, const uint32_t imageId, std::wstring_view& code)
{
    const auto imageIt = _images.find(imageId);
    if (imageIt == _images.end())
    {
        code = L"ENOENT:image not found";
        return false;
    }
    auto& image = imageIt->second;
    if (command.width > 3)
    {
        code = L"EINVAL:invalid animation state";
        return false;
    }
    if (command.cols != 0 && !_framePixels(image, command.cols))
    {
        code = L"ENOENT:frame not found";
        return false;
    }
    if (command.rows != 0 && !_frameGap(image, command.rows))
    {
        code = L"ENOENT:frame not found";
        return false;
    }

    auto reschedule = false;
    if (command.cols != 0)
    {
        image.currentFrame = command.cols;
        image.presentedFrame = image.currentFrame;
        _updateImageSurface(imageId, *_frameStorage(image, image.currentFrame));
        reschedule = true;
    }
    if (command.rows != 0 && command.haveZ && command.zIndex != 0)
    {
        *_frameGap(image, command.rows) = command.zIndex;
        reschedule |= command.rows == image.currentFrame;
    }
    if (command.height != 0)
    {
        image.loopCount = command.height;
        image.loopsRemaining = command.height == 1 ? UINT32_MAX : command.height - 1;
    }
    if (command.width != 0)
    {
        image.animationState = command.width;
        image.waitingForFrames = false;
        image.nextFrameTime = {};
        image.loopsRemaining = image.loopCount == 1 ? UINT32_MAX : image.loopCount - 1;
        reschedule = true;
    }

    if (image.animationState == 1)
    {
        image.nextFrameTime = {};
        image.waitingForFrames = false;
    }
    else if (reschedule)
    {
        _scheduleAnimation(imageId, image, std::chrono::steady_clock::now());
    }
    _scheduleAnimationTimer();
    return true;
}

bool KittyParser::_processFrameComposition(const Control& command, const uint32_t imageId, std::wstring_view& code)
try
{
    const auto imageIt = _images.find(imageId);
    if (imageIt == _images.end())
    {
        code = L"ENOENT:image not found";
        return false;
    }
    auto& image = imageIt->second;
    const auto sourceFrame = command.rows;
    const auto destinationFrame = command.cols;
    const auto* source = _framePixels(image, sourceFrame);
    const auto* destination = _framePixels(image, destinationFrame);
    if (sourceFrame == 0 || destinationFrame == 0 || !source || !destination)
    {
        code = L"ENOENT:frame not found";
        return false;
    }

    const auto width = command.srcW == 0 ? image.width : command.srcW;
    const auto height = command.srcH == 0 ? image.height : command.srcH;
    if (width == 0 || height == 0 ||
        command.srcX > image.width || command.srcY > image.height ||
        command.upperX > image.width || command.upperY > image.height ||
        width > image.width - command.srcX || height > image.height - command.srcY ||
        width > image.width - command.upperX || height > image.height - command.upperY)
    {
        code = L"EINVAL:composition rectangle out of bounds";
        return false;
    }

    const til::rect sourceRect{
        gsl::narrow_cast<til::CoordType>(command.upperX),
        gsl::narrow_cast<til::CoordType>(command.upperY),
        gsl::narrow_cast<til::CoordType>(command.upperX + width),
        gsl::narrow_cast<til::CoordType>(command.upperY + height),
    };
    const til::rect destinationRect{
        gsl::narrow_cast<til::CoordType>(command.srcX),
        gsl::narrow_cast<til::CoordType>(command.srcY),
        gsl::narrow_cast<til::CoordType>(command.srcX + width),
        gsl::narrow_cast<til::CoordType>(command.srcY + height),
    };
    const auto rectanglesOverlap = sourceRect.left < destinationRect.right &&
                                   destinationRect.left < sourceRect.right &&
                                   sourceRect.top < destinationRect.bottom &&
                                   destinationRect.top < sourceRect.bottom;
    if (sourceFrame == destinationFrame && rectanglesOverlap)
    {
        code = L"EINVAL:overlapping self-composition";
        return false;
    }

    std::vector<RGBQUAD> sourcePixels(static_cast<size_t>(width) * height);
    for (uint32_t row = 0; row < height; ++row)
    {
        const auto srcOffset = static_cast<size_t>(command.upperY + row) * image.width + command.upperX;
        std::copy_n(source->begin() + srcOffset, width, sourcePixels.begin() + static_cast<size_t>(row) * width);
    }
    auto composed = *destination;
    for (uint32_t row = 0; row < height; ++row)
    {
        const auto dstOffset = static_cast<size_t>(command.srcY + row) * image.width + command.srcX;
        _compositePixels(std::span{ composed }.subspan(dstOffset, width),
                              std::span{ sourcePixels }.subspan(static_cast<size_t>(row) * width, width),
                              command.noCursorMovement);
    }
    *_frameStorage(image, destinationFrame) = std::make_shared<std::vector<RGBQUAD>>(std::move(composed));
    if (image.presentedFrame == destinationFrame)
    {
        _updateImageSurface(imageId, *_frameStorage(image, destinationFrame));
    }
    return true;
}
catch (const std::bad_alloc&)
{
    code = L"ENOSPC:could not compose frame";
    return false;
}

void KittyParser::_scheduleAnimation(const uint32_t imageId, Image& image, const std::chrono::steady_clock::time_point now)
{
    if (image.animationState != 2 && image.animationState != 3)
    {
        image.nextFrameTime = {};
        return;
    }
    const auto* gap = _frameGap(image, image.currentFrame);
    if (!gap)
    {
        image.animationState = 1;
        image.nextFrameTime = {};
        return;
    }
    if (*gap > 0)
    {
        image.waitingForFrames = false;
        if (image.presentedFrame != image.currentFrame)
        {
            image.presentedFrame = image.currentFrame;
            _updateImageSurface(imageId, *_frameStorage(image, image.presentedFrame));
        }
        image.nextFrameTime = now + std::chrono::milliseconds(*gap);
    }
    else
    {
        image.nextFrameTime = now;
        _advanceImage(imageId, image, now);
    }
}

bool KittyParser::_advanceImage(const uint32_t imageId, Image& image, const std::chrono::steady_clock::time_point now)
{
    if (image.animationState != 2 && image.animationState != 3)
    {
        image.nextFrameTime = {};
        return false;
    }

    const auto frameCount = gsl::narrow_cast<uint32_t>(_frameCount(image));
    if (frameCount < 2)
    {
        image.waitingForFrames = image.animationState == 2;
        image.nextFrameTime = {};
        return false;
    }

    // A full pass is enough to prove that every frame is gapless. Stop rather
    // than spin forever on an animation that can never present a frame.
    for (uint32_t skipped = 0; skipped <= frameCount; ++skipped)
    {
        if (image.currentFrame < frameCount)
        {
            ++image.currentFrame;
        }
        else if (image.animationState == 2)
        {
            image.waitingForFrames = true;
            image.nextFrameTime = {};
            return false;
        }
        else
        {
            if (image.loopsRemaining != UINT32_MAX)
            {
                if (image.loopsRemaining == 0)
                {
                    image.animationState = 1;
                    image.nextFrameTime = {};
                    return false;
                }
                --image.loopsRemaining;
            }
            image.currentFrame = 1;
        }

        const auto* gap = _frameGap(image, image.currentFrame);
        if (gap && *gap > 0)
        {
            image.waitingForFrames = false;
            image.presentedFrame = image.currentFrame;
            _updateImageSurface(imageId, *_frameStorage(image, image.currentFrame));
            image.nextFrameTime = now + std::chrono::milliseconds(*gap);
            return true;
        }
    }

    image.animationState = 1;
    image.nextFrameTime = {};
    return false;
}

void KittyParser::AdvanceAnimations(const std::chrono::steady_clock::time_point now)
{
    for (auto& [imageId, image] : _images)
    {
        if ((image.animationState == 2 || image.animationState == 3) &&
            image.nextFrameTime != std::chrono::steady_clock::time_point{} &&
            image.nextFrameTime <= now)
        {
            _advanceImage(imageId, image, now);
        }
    }
    _scheduleAnimationTimer();
}

// Animated images advance on their own schedule, with nothing arriving in the
// stream to drive them. Work out when the soonest one is due and tell the host,
// which owns the render thread and decides how to arrange the wakeup. Whether
// that deadline is honoured early, late, or coalesced with other work is the
// host's business; this only reports when there is something to do.
void KittyParser::_scheduleAnimationTimer()
{
    auto earliest = std::chrono::steady_clock::time_point::max();
    for (const auto& [id, image] : _images)
    {
        static_cast<void>(id);
        if ((image.animationState == 2 || image.animationState == 3) &&
            image.nextFrameTime != std::chrono::steady_clock::time_point{})
        {
            earliest = std::min(earliest, image.nextFrameTime);
        }
    }
    _dispatcher._api.RequestTimedContentUpdate(earliest == std::chrono::steady_clock::time_point::max() ?
                                                   std::nullopt :
                                                   std::optional{ earliest });
}

void KittyParser::_deleteAnimationFrames(const uint32_t imageId, const uint32_t frameNumber, const bool freeData)
{
    const auto imageIt = _images.find(imageId);
    if (imageIt == _images.end())
    {
        return;
    }
    auto& image = imageIt->second;
    const auto frameCount = gsl::narrow_cast<uint32_t>(_frameCount(image));
    if (frameNumber == 0 || frameNumber > frameCount)
    {
        return;
    }
    if (frameCount <= 1)
    {
        if (freeData)
        {
            _erasePlacementsForImage(imageId);
            _eraseImagePlacements(imageId);
            _eraseImage(imageId);
        }
        _scheduleAnimationTimer();
        return;
    }
    const auto frameBytes = image.pixels->size() * sizeof(RGBQUAD);
    if (frameNumber == 1)
    {
        auto promoted = std::move(image.animationFrames.front());
        image.animationFrames.erase(image.animationFrames.begin());
        image.pixels = std::move(promoted.pixels);
        image.rootGapMilliseconds = promoted.gapMilliseconds;
    }
    else
    {
        image.animationFrames.erase(image.animationFrames.begin() + (frameNumber - 2));
    }
    _totalPixelBytes -= frameBytes;

    const auto remaining = gsl::narrow_cast<uint32_t>(_frameCount(image));
    if (image.currentFrame > frameNumber)
    {
        --image.currentFrame;
    }
    else if (image.currentFrame == frameNumber)
    {
        image.currentFrame = std::min(frameNumber, remaining);
    }
    if (image.presentedFrame > frameNumber)
    {
        --image.presentedFrame;
    }
    else if (image.presentedFrame == frameNumber)
    {
        image.presentedFrame = std::min(frameNumber, remaining);
    }

    if (remaining == 1)
    {
        image.animationState = 1;
        image.waitingForFrames = false;
        image.nextFrameTime = {};
    }
    _updateImageSurface(imageId, *_frameStorage(image, image.presentedFrame));

    if (freeData && remaining == 1)
    {
        _erasePlacementsForImage(imageId);
        _eraseImagePlacements(imageId);
        _eraseImage(imageId);
    }
    else if (image.animationState == 2 || image.animationState == 3)
    {
        _scheduleAnimation(imageId, image, std::chrono::steady_clock::now());
    }
    _scheduleAnimationTimer();
}

// Registers a complete shared image surface and its placement geometry. The renderer
// applies source cropping and target scaling directly, without row-local rasterization.
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#controlling-displayed-image-layout
til::size KittyParser::_placeImage(const Image& image, const bool moveCursor, const uint32_t imageId, const uint64_t layerId, const uint32_t cols, const uint32_t rows, const uint32_t srcX, const uint32_t srcY, const uint32_t srcW, const uint32_t srcH, const uint32_t cellOffsetX, const uint32_t cellOffsetY, const int32_t zIndex, const std::optional<til::point> anchor)
{
    const auto frameStorage = _frameStorage(image, image.presentedFrame);
    const auto framePixels = frameStorage ? frameStorage->get() : nullptr;
    if (!framePixels || framePixels->empty() || image.width == 0 || image.height == 0)
    {
        return {};
    }
    auto page = _dispatcher._pages.ActivePage();
    auto& buffer = page.Buffer();
    // A relative/registered placement supplies an explicit top-left anchor; otherwise the
    // cursor is the anchor. An anchored placement never moves the cursor (see caller).
    const auto origin = anchor.has_value() ? *anchor : page.Cursor().GetPosition();
    const auto cellSize = _dispatcher._api.GetCellSize();
    const auto cellWidth = std::max(1, cellSize.width);
    const auto cellHeight = std::max(1, cellSize.height);
    const til::size clampedCellSize{ cellWidth, cellHeight };
    const auto imageWidth = static_cast<til::CoordType>(image.width);
    const auto imageHeight = static_cast<til::CoordType>(image.height);
    // X/Y shift the image within the first cell (a sub-cell pixel offset), clamped to the
    // cell. The leading offset pixels are left transparent and overflow is truncated.
    const auto offsetX = static_cast<til::CoordType>(std::min<uint32_t>(cellOffsetX, static_cast<uint32_t>(cellWidth - 1)));
    const auto offsetY = static_cast<til::CoordType>(std::min<uint32_t>(cellOffsetY, static_cast<uint32_t>(cellHeight - 1)));

    // Source crop in pixels (x,y,w,h), clamped to the image; w/h=0 (or past the edge)
    // extends to the right/bottom edge.
    const auto cropX = static_cast<til::CoordType>(std::min<uint32_t>(srcX, image.width));
    const auto cropY = static_cast<til::CoordType>(std::min<uint32_t>(srcY, image.height));
    const auto cropW = srcW == 0 ? imageWidth - cropX : std::min(static_cast<til::CoordType>(std::min<uint32_t>(srcW, image.width)), imageWidth - cropX);
    const auto cropH = srcH == 0 ? imageHeight - cropY : std::min(static_cast<til::CoordType>(std::min<uint32_t>(srcH, image.height)), imageHeight - cropY);
    if (cropW <= 0 || cropH <= 0)
    {
        return {};
    }

    // Requested target pixel size: c/r set a cell span (single axis preserves aspect; neither
    // keeps native size). Shared with the virtual grid so a U=1 placement matches this draw.
    const auto [targetW64, targetH64] = _targetPixels(cropW, cropH, cols, rows, cellWidth, cellHeight);
    const auto targetW = std::max<int64_t>(targetW64, 1);
    const auto targetH = std::max<int64_t>(targetH64, 1);

    const auto columnBegin = origin.x;
    // Per the kitty spec, a sub-cell X/Y offset is NOT added to the number of columns/rows:
    // the placement rectangle (and thus the cursor advance) is sized by the image footprint
    // alone; the offset merely shifts the content within that footprint, and any overflow is
    // truncated on the right/bottom edge. Keep the span math in 64-bit (targetW/H may be up to
    // ~INT32_MAX from aspect scaling) and clamp to the page before narrowing.
    const int64_t spanWidthPx = targetW;
    const auto columns = static_cast<til::CoordType>(std::min<int64_t>((spanWidthPx + cellWidth - 1) / cellWidth, page.Width()));
    const auto columnEnd = std::min(columnBegin + columns, page.Width());
    if (columnEnd <= columnBegin)
    {
        return {};
    }
    const int64_t spanHeightPx = targetH;
    const auto rowSpan = static_cast<til::CoordType>(std::min<int64_t>((spanHeightPx + cellHeight - 1) / cellHeight, page.Bottom()));
    auto redrawTop = std::max(0, origin.y);
    auto redrawBottom = std::min(origin.y + rowSpan, page.Bottom());
    const auto drawnColumns = columnEnd - columnBegin;
    const auto drawnRows = std::max(0, redrawBottom - origin.y);
    if (drawnColumns > 0 && drawnRows > 0)
    {
        auto surface = image.surface;
        const auto newSurface = !surface;
        if (newSurface)
        {
            surface = std::make_shared<::Image>(til::size{ imageWidth, imageHeight }, *frameStorage);
        }

        const ImagePlacement::Key key{ imageId, layerId, ImagePlacement::Key::Protocol::Kitty };
        buffer.GetMutableImages().AddOrReplace(ImagePlacement{
            key,
            surface,
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
        if (newSurface)
        {
            image.surface = std::move(surface);
        }
    }
    buffer.TriggerRedraw(Viewport::FromExclusive({ 0, redrawTop, page.Width(), redrawBottom }));

    if (moveCursor)
    {
        // Per the kitty spec, the cursor moves right by the column span and down by
        // the row span of the placement (clamped to the page).
        page.Cursor().SetPosition({ std::min(columnEnd, page.Width() - 1), std::min(origin.y + rowSpan, page.Bottom() - 1) });
    }

    // Report the footprint actually drawn (after clamping) so the caller can track this
    // placement's extent and later erase exactly these cells by rectangle.
    if (drawnColumns > 0 && drawnRows > 0)
    {
        // The image is passed in by reference and may not be in the map at all (the
        // relative-placement paths hand over an entry they already resolved), so this
        // records the hint without asserting a lookup that can legitimately miss.
        if (const auto entry = _images.find(imageId); entry != _images.end())
        {
            entry->second.hasRenderedPlacements = true;
        }
    }
    return { drawnColumns, drawnRows };
}

// Maps a c=/r= request to a target pixel size. With both axes set each is cells*cell-size;
// with one set the other preserves aspect; with neither the native crop size is kept. Capped
// so the aspect multiply can't overflow. Shared by the cursor-anchored and virtual paths.
KittyParser::TargetSize KittyParser::_targetPixels(const int64_t cropW, const int64_t cropH, const uint32_t cols, const uint32_t rows, const int64_t cellWidth, const int64_t cellHeight) noexcept
{
    constexpr uint32_t maxCells = 8192;
    const int64_t reqCols = std::min(cols, maxCells);
    const int64_t reqRows = std::min(rows, maxCells);
    const auto safeCropW = std::max<int64_t>(cropW, 1);
    const auto safeCropH = std::max<int64_t>(cropH, 1);
    if (reqCols != 0 && reqRows != 0)
    {
        return { reqCols * cellWidth, reqRows * cellHeight };
    }
    if (reqCols != 0)
    {
        const auto w = reqCols * cellWidth;
        return { w, safeCropH * w / safeCropW };
    }
    if (reqRows != 0)
    {
        const auto h = reqRows * cellHeight;
        return { safeCropW * h / safeCropH, h };
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

// Records (or replaces) a placement keyed by (imageId, placementId). Re-sending the same pair
// overwrites the prior entry so a placement can be moved/resized without flicker. The registry
// is bounded; once past MaxPlacements the lowest-keyed OTHER entry is dropped (placements
// are also evicted alongside their image's LRU eviction via _erasePlacementsForImage).
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
void KittyParser::_registerPlacement(const Placement& placement)
{
    if (placement.imageId == 0 || placement.placementId == 0)
    {
        return; // a placement id is only meaningful with a real image id
    }
    const std::pair<uint32_t, uint32_t> key{ placement.imageId, placement.placementId };
    _placements[key] = placement;
    // Evict the lowest-keyed entry that ISN'T the one we just registered, so the new placement is
    // never the victim of its own insertion (mirrors the image LRU keeping the newest). Evicting a
    // victim must (a) erase its drawn cells so no ghost pixels linger and (b) cascade to its
    // relative children so none are left dangling with a gone parent. Capture the victim's key +
    // value BY VALUE before erasing, because _cascadePlacementChildren mutates
    // _placements; re-check size() and re-fetch begin() each iteration (never hold an iterator
    // across the cascade).
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

// A registered placement is the root of a bounded tree: every relative child has one parent,
// anonymous children are leaves, and creation-time validation rejects cycles/depth overflow.
// Build the tree index and complete movement plan once, then preflight it before the parent is
// changed. The apply pass erases each old collection placement by its identity before
// redrawing descendants parent-first, so the move never depends on stale creation rectangles.
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
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
            // The preflight pass treats a missing image as recoverable, and registering
            // this placement can have evicted one in between; match that rather than
            // dereferencing end().
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

    // Restore the complete descendant set if an allocation failed after the parent replacement
    // was committed. Exact layer identities let us remove any partially redrawn destination
    // layers without disturbing siblings, then recreate the prior layers at their live anchors.
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
            // Preserve the original ENOMEM response; restoration is best-effort under true OOM.
        }
    }
    return false;
}

// Removes every placement of an image and cascade-deletes any relative children: when a
// placement is removed, placements positioned relative to it are removed too; if a child's
// image then has no remaining placements, that image is deleted as well (the parent + its
// relative children form a group managed together). A bounded worklist guards against cycles.
// Anonymous relative children (no placement id, tracked in _anonymousPlacements) are also
// cascade-erased here; being id-less they are leaves and never extend the worklist.
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
void KittyParser::_erasePlacementsForImage(const uint32_t imageId)
{
    // Collect the (imageId, placementId) pairs being removed so their relative children
    // can be found and cascaded. Process iteratively to a fixed point.
    std::deque<std::pair<uint32_t, uint32_t>> removed;
    for (auto it = _placements.begin(); it != _placements.end();)
    {
        if (it->first.first == imageId)
        {
            removed.push_back(it->first);
            it = _placements.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // Drop this image's own anonymous placements (their cells are covered by the caller's broad
    // _eraseImagePlacements(imageId), but the tracking entries must go too).
    for (auto it = _anonymousPlacements.begin(); it != _anonymousPlacements.end();)
    {
        if (it->imageId == imageId)
        {
            _erasePlacementCells(*it);
            it = _anonymousPlacements.erase(it);
        }
        else
        {
            ++it;
        }
    }

    std::erase_if(_virtualIds, [&](const auto& entry) {
        return entry.first.first == imageId;
    });

    // The caller deletes `imageId` itself separately, so protect it from auto-deletion here.
    _cascadePlacementChildren(removed, imageId);
}

// True if any tracked placement (registered or anonymous) still references this image id.
bool KittyParser::_imageHasPlacements(const uint32_t id) const noexcept
{
    for (const auto& entry : _placements)
    {
        if (entry.first.first == id)
        {
            return true;
        }
    }
    for (const auto& anon : _anonymousPlacements)
    {
        if (anon.imageId == id)
        {
            return true;
        }
    }
    return false;
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

// Cascade-deletes the relative children of every placement key in `removed`, iterating to a
// fixed point. For each removed (imageId, placementId): registered children positioned relative
// to it are erased (own cells + registry entry) and themselves enqueued; anonymous (id-less,
// leaf) children are erased; and any image left with no placements is deleted -- except
// `keepImageId`, which the CALLER deletes separately and so must not be auto-deleted here. A
// bounded worklist guards against cycles in a malformed graph.
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
void KittyParser::_cascadePlacementChildren(std::deque<std::pair<uint32_t, uint32_t>>& removed, const uint32_t keepImageId)
{
    // Guard the cascade with an upper bound so a malformed graph can't loop forever.
    auto guard = MaxPlacements + 1;
    while (!removed.empty() && guard-- > 0)
    {
        const auto parent = removed.front();
        removed.pop_front();
        for (auto it = _placements.begin(); it != _placements.end();)
        {
            const auto& p = it->second;
            if (p.hasParent && p.parentImageId == parent.first && p.parentPlacementId == parent.second)
            {
                // Erase this child's exact collection placement without disturbing siblings.
                _erasePlacementCells(p);
                if (p.isVirtual)
                {
                    _virtualIds.erase(it->first);
                }
                removed.push_back(it->first);
                it = _placements.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Anonymous relative children of this parent: erase their pixels and drop them. They are
        // leaves (no placement id) so they never extend the worklist. If such a child's image is
        // then left with no placements at all, delete that image as well.
        for (auto it = _anonymousPlacements.begin(); it != _anonymousPlacements.end();)
        {
            if (it->hasParent && it->parentImageId == parent.first && it->parentPlacementId == parent.second)
            {
                const auto anonImageId = it->imageId;
                _erasePlacementCells(*it);
                it = _anonymousPlacements.erase(it);
                if (anonImageId != keepImageId && _images.count(anonImageId) != 0 && !_imageHasPlacements(anonImageId))
                {
                    _eraseImage(anonImageId);
                    _eraseImagePlacements(anonImageId);
                }
            }
            else
            {
                ++it;
            }
        }

        // If a child's image now has no placements left, delete that image too.
        const auto childImageId = parent.first;
        if (childImageId != keepImageId && _images.count(childImageId) != 0 && !_imageHasPlacements(childImageId))
        {
            _eraseImage(childImageId);
            _eraseImagePlacements(childImageId);
        }
    }
}

// Deletes a single placement (imageId, placementId): erases its own on-screen cells, removes it
// from the registry, and cascades to its relative children (registered + anonymous). When freeData
// is true (an UPPERCASE selector) an image left with no placements is also freed -- including
// imageId itself if this was its last placement; when false (lowercase) the image DATA is kept so a
// later a=p can re-display it. Relative children are freed either way, per the group lifetime. A
// no-op if the placement isn't registered. Spec: "If you specify a p key for the placement id as
// well, then only the placement with the specified image id and placement id will be deleted."
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#deleting-images
void KittyParser::_deletePlacement(const uint32_t imageId, const uint32_t placementId, const bool freeData)
{
    const auto it = _placements.find({ imageId, placementId });
    if (it == _placements.end())
    {
        return;
    }
    _erasePlacementCells(it->second);
    std::deque<std::pair<uint32_t, uint32_t>> removed{ { imageId, placementId } };
    _placements.erase(it);
    _virtualIds.erase({ imageId, placementId });
    // keepImageId protects imageId from the cascade's free-if-unused: lowercase (freeData=false)
    // keeps imageId's data; uppercase frees it when it has no placements left. Children (different
    // ids) are freed either way.
    _cascadePlacementChildren(removed, freeData ? 0 : imageId);
}

// Deletes every physical placement while leaving virtual placements (and the images rendered
// through Unicode placeholder text) untouched. Lowercase d=a keeps reusable image data; uppercase
// d=A additionally frees every image that no surviving virtual placement references.
void KittyParser::_deleteAllPlacements(const bool freeData)
{
    std::vector<std::pair<uint32_t, uint32_t>> selectedPlacements;
    for (const auto& [key, placement] : _placements)
    {
        if (!placement.isVirtual)
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
        _deletePlacement(key.first, key.second, false);
    }

    for (auto it = _anonymousPlacements.begin(); it != _anonymousPlacements.end();)
    {
        if (!it->isVirtual)
        {
            _erasePlacementCells(*it);
            it = _anonymousPlacements.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (freeData)
    {
        std::vector<uint32_t> unreferencedImages;
        for (const auto& [imageId, image] : _images)
        {
            static_cast<void>(image);
            if (!_imageHasPlacements(imageId))
            {
                unreferencedImages.push_back(imageId);
            }
        }
        for (const auto imageId : unreferencedImages)
        {
            _eraseImage(imageId);
            _eraseImagePlacements(imageId);
        }
    }
    _scheduleAnimationTimer();
}

// Deletes every NON-VIRTUAL image with at least one physical placement inside the half-open cell
// rect [left,right) x [top,bottom). Virtual placements of an affected image remain available to
// placeholder text. Backs d=c (cursor cell), d=p (cell x,y), d=x (column), and d=y (row).
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#deleting-images
void KittyParser::_deleteImagesIntersecting(const til::CoordType left, const til::CoordType top, const til::CoordType right, const til::CoordType bottom, const bool freeData)
{
    auto page = _dispatcher._pages.ActivePage();
    auto& buffer = page.Buffer();
    const auto rowBegin = std::max(0, top);
    const auto rowEnd = std::min(bottom, page.Bottom());
    const auto colBegin = std::max(0, left);
    const auto colEnd = std::min(right, page.Width());
    if (rowEnd <= rowBegin || colEnd <= colBegin)
    {
        return;
    }
    // Collect owners first; erasing mutates the collection, so views remain valid only
    // until this scan completes.
    std::vector<uint32_t> affected;
    const til::rect target{ colBegin, rowBegin, colEnd, rowEnd };
    const auto isPhysical = [&](const ImagePlacement::Key key) {
        return key.protocol == ImagePlacement::Key::Protocol::Kitty &&
               (std::any_of(_placements.begin(), _placements.end(), [&](const auto& entry) {
                  return !entry.second.isVirtual &&
                         entry.second.imageId == key.imageId &&
                         entry.second.layerId == key.layerId;
               }) ||
               std::any_of(_anonymousPlacements.begin(), _anonymousPlacements.end(), [&](const auto& placement) {
                   return !placement.isVirtual &&
                          placement.imageId == key.imageId &&
                          placement.layerId == key.layerId;
               }));
    };
    for (const auto& placement : buffer.GetImages().IntersectingRows(rowBegin, rowEnd))
    {
        const auto key = placement.Identity();
        if ((placement.CellBounds() & target).empty() || !isPhysical(key))
        {
            continue;
        }
        if (std::find(affected.begin(), affected.end(), key.imageId) == affected.end())
        {
            affected.push_back(key.imageId);
        }
    }
    for (const auto id : affected)
    {
        std::vector<std::pair<uint32_t, uint32_t>> namedPlacements;
        for (const auto& [key, placement] : _placements)
        {
            if (key.first == id && !placement.isVirtual)
            {
                namedPlacements.push_back(key);
            }
        }
        for (const auto& key : namedPlacements)
        {
            _deletePlacement(key.first, key.second, false);
        }
        for (auto placement = _anonymousPlacements.begin(); placement != _anonymousPlacements.end();)
        {
            if (placement->imageId == id && !placement->isVirtual)
            {
                _erasePlacementCells(*placement);
                placement = _anonymousPlacements.erase(placement);
            }
            else
            {
                ++placement;
            }
        }
        if (freeData && !_imageHasPlacements(id))
        {
            _eraseImage(id);
        }
    }
}

// Deletes every image whose id is in the INCLUSIVE range [lo, hi] (kitty d=r): erases each image's
// pixels and drops its placements (registered, anonymous, and virtual -- r/R DO affect virtual
// placements per the spec). When freeData is true (d=R) the image data is also freed; when false
// (d=r) the data is kept for a later a=p. A no-op if lo/hi are unset (0) or reversed.
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#deleting-images
void KittyParser::_deleteImagesInIdRange(const uint32_t lo, const uint32_t hi, const bool freeData)
{
    if (lo == 0 || hi == 0 || lo > hi)
    {
        return;
    }
    // Snapshot the matching ids first; the erase mutates _images.
    std::vector<uint32_t> ids;
    for (const auto& entry : _images)
    {
        if (entry.first >= lo && entry.first <= hi)
        {
            ids.push_back(entry.first);
        }
    }
    for (const auto id : ids)
    {
        _erasePlacementsForImage(id);
        std::erase_if(_virtualIds, [&](const auto& entry) {
            return entry.first.first == id;
        });
        if (freeData)
        {
            _eraseImage(id);
        }
        _eraseImagePlacements(id);
    }
}

// Deletes physical placements selected by an exact z-index. d=q/Q first selects
// direct placements intersecting one viewport-relative cell. Virtual placements
// are excluded by the protocol.
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
                    const auto physical = std::any_of(_placements.begin(), _placements.end(), [&](const auto& entry) {
                                              return !entry.second.isVirtual &&
                                                     entry.second.imageId == key.imageId &&
                                                     entry.second.layerId == key.layerId;
                                          }) ||
                                          std::any_of(_anonymousPlacements.begin(), _anonymousPlacements.end(), [&](const auto& placement) {
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
        // A prior selected parent may already have cascade-deleted this placement.
        const auto it = _placements.find(key);
        if (it != _placements.end())
        {
            _erasePlacementCells(it->second);
            _placements.erase(it);
            // Protect the selected root's data here. Uppercase selectors free it
            // below only after all surviving physical placements are considered;
            // relative children retain the existing group-delete behavior.
            std::deque<std::pair<uint32_t, uint32_t>> removed{ key };
            _cascadePlacementChildren(removed, key.first);
        }
    }
    std::vector<uint32_t> imageIds;
    for (auto it = _anonymousPlacements.begin(); it != _anonymousPlacements.end();)
    {
        if (selected(*it))
        {
            if (std::find(imageIds.begin(), imageIds.end(), it->imageId) == imageIds.end())
            {
                imageIds.push_back(it->imageId);
            }
            _erasePlacementCells(*it);
            it = _anonymousPlacements.erase(it);
        }
        else
        {
            ++it;
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

// Finds a physical placement by its retained collection identity. Unlike the registry's
// creation-time anchor, this position follows scroll, reflow, insertion, and block copies.
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

// Resolves the on-screen top-left anchor that a relative child (whose own key is `origin`)
// should be positioned against: the IMMEDIATE parent's current retained-layer anchor (or, for a
// virtual placement, its placeholder cells). Deriving physical anchors from the internal layer
// identity makes relative placement follow scroll/reflow instead of using creation-time registry
// coordinates. The rest of the ancestry is still walked purely to validate it. On failure sets
// `code` and returns nullopt:
//   ENOPARENT  - a referenced parent does not exist (and is not a virtual image on screen)
//   ECYCLE     - the chain loops back to an already-visited placement (including `origin`)
//   ETOODEEP   - the chain exceeds MaxPlacementDepth links
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
std::optional<til::point> KittyParser::_resolvePlacementAnchor(const uint32_t parentImageId, const uint32_t parentPlacementId, const std::pair<uint32_t, uint32_t> origin, std::wstring_view& code) const
{
    // Seed the visited set with the child being created so a re-put that loops back to it
    // (A -> ... -> A) is detected as a cycle even when the old A had a fixed anchor.
    std::vector<std::pair<uint32_t, uint32_t>> visited{ origin };
    std::pair<uint32_t, uint32_t> key{ parentImageId, parentPlacementId };
    std::optional<til::point> immediateAnchor; // captured at depth 1; the child anchors off this
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

        const auto it = _placements.find(key);
        if (it == _placements.end())
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

        const auto& p = it->second;
        if (depth == 1)
        {
            // The child's position comes from this immediate parent's actual anchor.
            immediateAnchor = p.isVirtual ? _deriveVirtualPlacementAnchor(key.first, key.second) : _derivePlacementAnchor(p);
            if (!immediateAnchor)
            {
                code = L"ENOPARENT:relative parent not found"; // virtual parent with no on-screen cells
                return std::nullopt;
            }
        }
        if (p.isVirtual || !p.hasParent)
        {
            if (!descendantsFit(depth))
            {
                code = L"ETOODEEP:relative placement chain too deep";
                return std::nullopt;
            }
            return immediateAnchor; // reached a leaf (virtual or a normal root); chain is valid
        }
        key = { p.parentImageId, p.parentPlacementId }; // keep walking to validate the ancestry
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
        0x0305, 0x030D, 0x030E, 0x0310, 0x0312, 0x033D, 0x033E, 0x033F,
        0x0346, 0x034A, 0x034B, 0x034C, 0x0350, 0x0351, 0x0352, 0x0357,
        0x035B, 0x0363, 0x0364, 0x0365, 0x0366, 0x0367, 0x0368, 0x0369,
        0x036A, 0x036B, 0x036C, 0x036D, 0x036E, 0x036F, 0x0483, 0x0484,
        0x0485, 0x0486, 0x0487, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597,
        0x0598, 0x0599, 0x059C, 0x059D, 0x059E, 0x059F, 0x05A0, 0x05A1,
        0x05A8, 0x05A9, 0x05AB, 0x05AC, 0x05AF, 0x05C4, 0x0610, 0x0611,
        0x0612, 0x0613, 0x0614, 0x0615, 0x0616, 0x0617, 0x0657, 0x0658,
        0x0659, 0x065A, 0x065B, 0x065D, 0x065E, 0x06D6, 0x06D7, 0x06D8,
        0x06D9, 0x06DA, 0x06DB, 0x06DC, 0x06DF, 0x06E0, 0x06E1, 0x06E2,
        0x06E4, 0x06E7, 0x06E8, 0x06EB, 0x06EC, 0x0730, 0x0732, 0x0733,
        0x0735, 0x0736, 0x073A, 0x073D, 0x073F, 0x0740, 0x0741, 0x0743,
        0x0745, 0x0747, 0x0749, 0x074A, 0x07EB, 0x07EC, 0x07ED, 0x07EE,
        0x07EF, 0x07F0, 0x07F1, 0x07F3, 0x0816, 0x0817, 0x0818, 0x0819,
        0x081B, 0x081C, 0x081D, 0x081E, 0x081F, 0x0820, 0x0821, 0x0822,
        0x0823, 0x0825, 0x0826, 0x0827, 0x0829, 0x082A, 0x082B, 0x082C,
        0x082D, 0x0951, 0x0953, 0x0954, 0x0F82, 0x0F83, 0x0F86, 0x0F87,
        0x135D, 0x135E, 0x135F, 0x17DD, 0x193A, 0x1A17, 0x1A75, 0x1A76,
        0x1A77, 0x1A78, 0x1A79, 0x1A7A, 0x1A7B, 0x1A7C, 0x1B6B, 0x1B6D,
        0x1B6E, 0x1B6F, 0x1B70, 0x1B71, 0x1B72, 0x1B73, 0x1CD0, 0x1CD1,
        0x1CD2, 0x1CDA, 0x1CDB, 0x1CE0, 0x1DC0, 0x1DC1, 0x1DC3, 0x1DC4,
        0x1DC5, 0x1DC6, 0x1DC7, 0x1DC8, 0x1DC9, 0x1DCB, 0x1DCC, 0x1DD1,
        0x1DD2, 0x1DD3, 0x1DD4, 0x1DD5, 0x1DD6, 0x1DD7, 0x1DD8, 0x1DD9,
        0x1DDA, 0x1DDB, 0x1DDC, 0x1DDD, 0x1DDE, 0x1DDF, 0x1DE0, 0x1DE1,
        0x1DE2, 0x1DE3, 0x1DE4, 0x1DE5, 0x1DE6, 0x1DFE, 0x20D0, 0x20D1,
        0x20D4, 0x20D5, 0x20D6, 0x20D7, 0x20DB, 0x20DC, 0x20E1, 0x20E7,
        0x20E9, 0x20F0, 0x2CEF, 0x2CF0, 0x2CF1, 0x2DE0, 0x2DE1, 0x2DE2,
        0x2DE3, 0x2DE4, 0x2DE5, 0x2DE6, 0x2DE7, 0x2DE8, 0x2DE9, 0x2DEA,
        0x2DEB, 0x2DEC, 0x2DED, 0x2DEE, 0x2DEF, 0x2DF0, 0x2DF1, 0x2DF2,
        0x2DF3, 0x2DF4, 0x2DF5, 0x2DF6, 0x2DF7, 0x2DF8, 0x2DF9, 0x2DFA,
        0x2DFB, 0x2DFC, 0x2DFD, 0x2DFE, 0x2DFF, 0xA66F, 0xA67C, 0xA67D,
        0xA6F0, 0xA6F1, 0xA8E0, 0xA8E1, 0xA8E2, 0xA8E3, 0xA8E4, 0xA8E5,
        0xA8E6, 0xA8E7, 0xA8E8, 0xA8E9, 0xA8EA, 0xA8EB, 0xA8EC, 0xA8ED,
        0xA8EE, 0xA8EF, 0xA8F0, 0xA8F1, 0xAAB0, 0xAAB2, 0xAAB3, 0xAAB7,
        0xAAB8, 0xAABE, 0xAABF, 0xAAC1, 0xFE20, 0xFE21, 0xFE22, 0xFE23,
        0xFE24, 0xFE25, 0xFE26, 0x10A0F, 0x10A38, 0x1D185, 0x1D186, 0x1D187,
        0x1D188, 0x1D189, 0x1D1AA, 0x1D1AB, 0x1D1AC, 0x1D1AD, 0x1D242, 0x1D243,
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
    const auto frameStorage = _frameStorage(image, image.presentedFrame);
    if (!frameStorage || !*frameStorage || (*frameStorage)->empty() || image.width == 0 || image.height == 0)
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
            *frameStorage);
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
    const auto colorId = [](const TextColor color) noexcept {
        if (color.IsRgb())
        {
            const auto rgb = color.GetRGB();
            return (static_cast<uint32_t>(GetRValue(rgb)) << 16) | (static_cast<uint32_t>(GetGValue(rgb)) << 8) | GetBValue(rgb);
        }
        return color.IsIndex256() ? static_cast<uint32_t>(color.GetIndex()) : 0u;
    };
    auto column = startColumn;
    // Track the drawn placeholder cell span so ONE bounded redraw covers the whole segment/row,
    // instead of a TriggerRedraw per cell on the text-output hot path (matches _placeImage).
    auto firstDrawnCol = page.Width();
    auto lastDrawnCol = static_cast<til::CoordType>(-1);
    std::vector<ImagePlacement> fragments;
    fragments.reserve(std::min(segment.size(), static_cast<size_t>(page.Width())));
    for (size_t i = 0; i < segment.size();)
    {
        const auto next = buffer.GraphemeNext(segment, i);
        // A write may be split anywhere, including between a placeholder's two diacritics - the
        // console write path chunks long runs. The orphaned marks then open the next segment, where
        // they join the cell the previous segment already wrote, occupying no column of their own.
        // Step over them without advancing: counting them as a cell shifted every placeholder that
        // followed one column right, onto a cell carrying no image foreground, so that tile was
        // silently dropped and the grid rendered with a hole in it.
        if (i == 0 && _IsPlaceholderDiacriticRun(segment.substr(i, next - i)))
        {
            i = next;
            continue;
        }
        if (i + 1 < next && segment[i] == PlaceholderCodePointHigh && segment[i + 1] == PlaceholderCodePointLow)
        {
            // The first two recognized diacritics in the cluster give row then column; an optional
            // third gives the most significant byte of a >24-bit image id (composed below).
            // idHighByte starts at -1 (absent) so a 4th+ diacritic cannot overwrite an explicit 3rd
            // diacritic of index 0 (the spec ignores extras); absent or 0 leaves a plain 24-bit id.
            auto rowDiacritic = -1;
            auto colDiacritic = -1;
            auto idHighByte = -1;
            for (auto j = i + 2; j < next; ++j)
            {
                // A row/col diacritic may be an astral combining mark (index >= 283 in the
                // rowcolumn-diacritics table), stored as a UTF-16 surrogate pair; decode it to a
                // full codepoint before the lookup so large grids address the right row/column.
                auto cp = static_cast<char32_t>(segment[j]);
                if (til::is_leading_surrogate(segment[j]) && j + 1 < next && til::is_trailing_surrogate(segment[j + 1]))
                {
                    cp = til::combine_surrogates(segment[j], segment[j + 1]);
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
            if ((fg.IsRgb() || fg.IsIndex256()) && idHighByte <= 255)
            {
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
                        firstDrawnCol = std::min(firstDrawnCol, column);
                        lastDrawnCol = std::max(lastDrawnCol, column);
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
            }
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


// Erases every direct Kitty placement of an image.
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
        auto& images = buffer.GetMutableImages();
        images.EraseImage(ImagePlacement::Key::Protocol::Kitty, imageId);
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

// Erases one placement by its retained collection identity. The identity moves through
// scrolling, reflow, ICH, and block copies, unlike the creation-time anchor.
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
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
        auto& images = buffer.GetMutableImages();
        images.Erase(key);
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


// bytes. Returns false on any invalid character, misplaced padding, or a length
// that is not a multiple of four. An empty input decodes to zero bytes (success).
bool KittyParser::_DecodeBase64(const std::string_view input, std::vector<uint8_t>& output) noexcept
{
    output.clear();
    if (input.size() % 4 != 0)
    {
        return false;
    }

    const auto sextet = [](const char c) noexcept -> int {
        if (c >= 'A' && c <= 'Z')
        {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z')
        {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9')
        {
            return c - '0' + 52;
        }
        if (c == '+')
        {
            return 62;
        }
        if (c == '/')
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
        const auto c2 = input[i + 2];
        const auto c3 = input[i + 3];
        const auto v0 = sextet(input[i]);
        const auto v1 = sextet(input[i + 1]);
        if (v0 < 0 || v1 < 0)
        {
            return false;
        }
        // Padding ('=') is only valid in the final group, at position 3 or 2-3.
        if (c2 == '=')
        {
            if (c3 != '=' || i + 4 != input.size())
            {
                return false;
            }
            output.push_back(static_cast<uint8_t>((v0 << 2) | (v1 >> 4)));
            break;
        }
        const auto v2 = sextet(c2);
        if (v2 < 0)
        {
            return false;
        }
        if (c3 == '=')
        {
            if (i + 4 != input.size())
            {
                return false;
            }
            output.push_back(static_cast<uint8_t>((v0 << 2) | (v1 >> 4)));
            output.push_back(static_cast<uint8_t>((v1 << 4) | (v2 >> 2)));
            break;
        }
        const auto v3 = sextet(c3);
        if (v3 < 0)
        {
            return false;
        }
        output.push_back(static_cast<uint8_t>((v0 << 2) | (v1 >> 4)));
        output.push_back(static_cast<uint8_t>((v1 << 4) | (v2 >> 2)));
        output.push_back(static_cast<uint8_t>((v2 << 6) | v3));
    }

    return true;
}

// Inflate an RFC 1950 zlib stream (kitty's o=z transmission). On success `output`
// holds the decompressed bytes and the function returns true; malformed input, an
// inflated size of zero, or a size exceeding `cap` returns false (the caller maps
// that to an EINVAL ACK). The RFC 1951 DEFLATE body is inflated by inflatelib.
// noexcept: any allocation failure is caught and reported as false.
//
// Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#compression  (o=z)
// zlib container format: https://www.rfc-editor.org/rfc/rfc1950
bool KittyParser::_inflateZlib(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, const size_t cap, const bool allowTrailingZeroPadding) noexcept
try
{
    output.clear();

    // A zlib stream is a 2-byte header, the DEFLATE body, then a 4-byte big-endian
    // Adler-32 of the uncompressed data, so the smallest valid stream is 6 bytes.
    if (input.size() < 6)
    {
        return false;
    }
    const auto cmf = input[0];
    const auto flg = input[1];
    // FCHECK: the 16-bit value (CMF << 8 | FLG) must be a multiple of 31.
    if (((static_cast<unsigned>(cmf) << 8) | flg) % 31u != 0)
    {
        return false;
    }
    // CM (low nibble of CMF) must be 8 (DEFLATE) and CINFO (high nibble) must be <= 7.
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7)
    {
        return false;
    }
    // FDICT (preset dictionary) is not used by kitty and is not supported here.
    if ((flg & 0x20) != 0)
    {
        return false;
    }

    const auto deflateAvailable = input.size() - 2u;
    std::span<const std::byte> remainingInput{ reinterpret_cast<const std::byte*>(input.data() + 2), deflateAvailable };

    // Inflate in bounded chunks, growing the output only as the stream actually produces
    // bytes. A decompression bomb is rejected as soon as it crosses `cap` rather than
    // after materialising the whole expansion. We allow exactly one byte beyond `cap` so
    // "produced more than cap" is detectable without a zero-length final call, whose
    // "no progress" result would be ambiguous against a genuinely finished stream.
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
                // Still producing at limit == cap + 1, so the stream exceeds `cap`.
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

        // Neither buffer moved and the stream is not finished, so no call can ever make
        // progress -- a truncated DEFLATE body. Bail rather than spin.
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

    // inflatelib stops at the end of the DEFLATE stream, so whatever it did not consume
    // is the 4-byte Adler-32 plus any padding. Require the stream to end where that
    // trailer begins, rejecting trailing garbage and multi-member streams. When an S=0
    // shared-memory transfer includes page-rounded zero padding, only zeros may follow.
    const auto consumed = deflateAvailable - remainingInput.size();
    const auto streamEnd = 2u + consumed + 4u;
    if (streamEnd > input.size() ||
        (!allowTrailingZeroPadding && streamEnd != input.size()) ||
        (allowTrailingZeroPadding && !std::all_of(input.begin() + streamEnd, input.end(), [](const auto value) noexcept { return value == 0; })))
    {
        output.clear();
        return false;
    }

    // Verify the trailing Adler-32 (RFC 1950, big-endian) over the inflated bytes so a
    // corrupted payload is rejected rather than decoded into a garbled image.
    const auto adler32 = [](const uint8_t* p, size_t n) noexcept -> uint32_t {
        uint32_t a = 1;
        uint32_t b = 0;
        while (n != 0)
        {
            // Defer the modulo for up to NMAX (5552) bytes -- the longest run that
            // cannot overflow 32 bits -- which is how zlib's own adler32() works.
            auto k = n < 5552u ? n : size_t{ 5552 };
            n -= k;
            do
            {
                a += *p++;
                b += a;
            } while (--k != 0);
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