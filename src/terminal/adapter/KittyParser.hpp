/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- KittyParser.hpp

Abstract:
- This manages the Kitty graphics protocol: the APC G sequences that transmit,
  display, and delete images, the registry of transmitted images, the placements
  that display them, and the Unicode placeholder cells that reference them.
--*/

#pragma once

#include "DispatchTypes.hpp"
#include "ITermDispatch.hpp"
#include "../../buffer/out/ImageSlice.hpp"

#include <deque>
#include <map>
#include <optional>
#include <unordered_map>

// fwdecl unittest classes
#ifdef UNIT_TESTING
class AdapterTest;
#endif

namespace Microsoft::Console::VirtualTerminal
{
    class AdaptDispatch;

    class KittyParser
    {
    public:
        // The kitty Unicode placeholder code point. A cell holding this glyph, with a
        // 24-bit RGB foreground giving the image id, draws a sub-rect of a virtual
        // (U=1) image rather than the cursor-anchored placement. The writer has to
        // recognize it before any image has been transmitted, so it does not need an
        // instance.
        static constexpr wchar_t PlaceholderCodePointHigh = 0xDBFB; // surrogate pair for U+10EEEE
        static constexpr wchar_t PlaceholderCodePointLow = 0xDEEE;

        explicit KittyParser(AdaptDispatch& dispatcher) noexcept;

        // Collects one APC G sequence. The parser has already consumed the 'G'
        // identifier and routed us here on the strength of it.
        ITermDispatch::StringHandler DefineImage();

        // Resolves the placeholder cells in a run of text the writer just placed.
        void RenderPlaceholders(const std::wstring_view segment, const til::CoordType screenRow, const til::CoordType startColumn);

        // Runs every animated image forward to the given time, then reports when it
        // next needs to be called. The host calls this when the deadline it was last
        // given comes due.
        void AdvanceAnimations(std::chrono::steady_clock::time_point now);

        // Re-pushes every placement's pixels into the active page's buffer. The
        // registry outlives the buffer it drew into, so anything that swaps the
        // buffer under it has to ask for the layers again.
        void RefreshImageLayers();

        // Drops every image, every placement, and any transfer in progress.
        void HardReset() noexcept;

        // Removes the placements but keeps the transmitted image data, so a later
        // a=p can display it again. This is what an erase-display asks for.
        void ErasePlacements();

        // The registry is per-buffer. Leaving the main buffer sets its contents
        // aside and returning restores them; whatever the alternate buffer drew is
        // discarded with it. The first two run while the outgoing buffer is still
        // active, because they erase what is on it.
        void SaveMainBufferState() noexcept;
        void DiscardBufferState() noexcept;
        void RestoreMainBufferState() noexcept;

    private:
        struct Control
        {
            wchar_t action = L't';
            wchar_t deleteTarget = L'a';
            wchar_t compression = 0;
            uint32_t imageId = 0;
            uint32_t imageNumber = 0;
            uint32_t quiet = 0;
            uint32_t format = 32;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t cols = 0;    // c=: scale the placement to this many cell columns
            uint32_t rows = 0;    // r=: scale the placement to this many cell rows
            uint32_t srcX = 0;    // x=: source/update x, or composition destination x
            uint32_t srcY = 0;    // y=: source/update y, or composition destination y
            uint32_t srcW = 0;    // w=: source crop width in pixels (0 = to right edge)
            uint32_t srcH = 0;    // h=: source crop height in pixels (0 = to bottom edge)
            uint32_t cellOffsetX = 0; // X=: x pixel offset of the image within the first cell
            uint32_t cellOffsetY = 0; // Y=: y pixel offset of the image within the first cell
            uint32_t upperX = 0;  // X=: animation replacement mode or composition source x
            uint32_t upperY = 0;  // Y=: animation background RGBA or composition source y
            bool moreChunks = false;
            bool mPresent = false;
            bool haveId = false;
            bool haveNumber = false;
            wchar_t medium = L'd';          // t=: transmission medium (only d, direct, is handled here)
            bool noCursorMovement = false;  // C=1: leave the cursor in place after a placement
            bool hasNonChunkKey = false;    // true if any key other than m/q was present
            bool hasNonChunkKeyOtherThanAction = false; // permits required a=f on frame continuations
            bool virtualPlacement = false;  // U=1: virtual placement (store only; drawn later via Unicode placeholders)
            // Relative placements (https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements).
            uint32_t placementId = 0;        // p=: placement id (one display of an image); ignored when imageId==0
            uint32_t parentImageId = 0;      // P=: parent image id this placement is positioned relative to
            uint32_t parentPlacementId = 0;  // Q=: parent placement id (with P) identifying the parent placement
            int32_t offsetH = 0;             // H=: signed horizontal cell offset from the parent anchor (+right)
            int32_t offsetV = 0;             // V=: signed vertical cell offset from the parent anchor (+down)
            int32_t zIndex = 0;              // z=: signed stacking order; negative values render under text
            bool haveZ = false;
            bool havePlacementId = false;    // true if p= was present (so p= is echoed in the ack)
            bool haveParent = false;         // true if P= was present (a relative placement was requested)
        };
        struct AnimationFrame
        {
            std::vector<RGBQUAD> pixels;
            int32_t gapMilliseconds = 0;
        };
        // A stored Kitty image. Frame 1 is the root pixel vector; additional
        // full-canvas frames share its dimensions and carry their own next-frame gap.
        struct Image
        {
            uint32_t number = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            std::vector<RGBQUAD> pixels;
            int32_t rootGapMilliseconds = 0;
            std::vector<AnimationFrame> animationFrames;
            uint32_t currentFrame = 1;
            uint32_t animationState = 1;
            uint32_t loopCount = 1;
            uint32_t loopsRemaining = UINT32_MAX;
            uint32_t presentedFrame = 1;
            bool waitingForFrames = false;
            bool hasRenderedPlacements = false;
            std::chrono::steady_clock::time_point nextFrameTime{};

            size_t PixelBytes() const noexcept
            {
                auto bytes = pixels.size() * sizeof(RGBQUAD);
                for (const auto& frame : animationFrames)
                {
                    bytes += frame.pixels.size() * sizeof(RGBQUAD);
                }
                return bytes;
            }
        };
        // The target pixel size a c=/r= request maps to (one axis preserves aspect). Shared
        // by the cursor-anchored and virtual paths so a U=1 grid matches an equivalent draw.
        struct TargetSize
        {
            int64_t width = 0;
            int64_t height = 0;
        };
        // A virtual (U=1) placement's fixed grid geometry and source sampling state.
        // The grid (cols x rows) is recorded at store time so placeholder rendering slices
        // the image consistently no matter how the cells are chunked across writes.
        struct VirtualPlacement
        {
            uint32_t cols = 1;
            uint32_t rows = 1;
            // Source crop rect (pixels) captured from x/y/w/h at store time (w/h=0/past-edge
            // already resolved and clamped to the image), so placeholder rendering samples the
            // same sub-rect a direct c/r placement would instead of the whole image. cropW/cropH
            // are always > 0 after _storeVirtualPlacement (0 = unset => full image).
            uint32_t cropX = 0;
            uint32_t cropY = 0;
            uint32_t cropW = 0;
            uint32_t cropH = 0;
            // Exact scaled target pixel size (== _placeImage's targetW/targetH). Placeholder
            // cells sample this continuous scaled space so a virtual grid is pixel-identical to a
            // direct c/r placement even for non-divisible geometry; pixels past it are the
            // aspect-preserving padding (transparent). 0 => fall back to gridCols/Rows * cell size.
            // 64-bit: aspect-preserving (c-only/r-only) scaling can exceed 2^32, and truncating it
            // would diverge the placeholder render from the direct one.
            uint64_t targetW = 0;
            uint64_t targetH = 0;
            uint64_t layerId = 0;
            int32_t zIndex = 0;
        };
        // A single placement (one display) of an image, identified by the (imageId, placementId)
        // pair. anchorRow/anchorCol are the absolute top-left cell of the placement. A relative
        // placement (hasParent) is positioned at parentAnchor + (offsetH, offsetV); a virtual
        // (isVirtual) placement has no fixed anchor and derives one on demand from its on-screen
        // Unicode-placeholder cells. Re-sending the same (imageId, placementId) replaces the entry.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
        struct Placement
        {
            uint32_t imageId = 0;
            uint32_t placementId = 0;
            uint64_t layerId = 0;
            til::CoordType anchorRow = 0;
            til::CoordType anchorCol = 0;
            // The original drawn footprint in cells, retained for relative-placement geometry.
            til::CoordType cols = 0;
            til::CoordType rows = 0;
            // Original display parameters are retained so moving a parent can redraw this
            // relative placement at its new anchor without changing its sampled geometry.
            uint32_t displayCols = 0;
            uint32_t displayRows = 0;
            uint32_t srcX = 0;
            uint32_t srcY = 0;
            uint32_t srcW = 0;
            uint32_t srcH = 0;
            uint32_t cellOffsetX = 0;
            uint32_t cellOffsetY = 0;
            uint32_t parentImageId = 0;
            uint32_t parentPlacementId = 0;
            int32_t offsetH = 0;
            int32_t offsetV = 0;
            int32_t zIndex = 0;
            bool hasParent = false;
            bool isVirtual = false;
        };
        struct BufferState
        {
            uint32_t nextImageId = 1;
            uint64_t nextLayerId = 1;
            size_t totalPixelBytes = 0;
            std::unordered_map<uint32_t, Image> images;
            std::unordered_map<uint32_t, uint32_t> imageNumbers;
            std::deque<uint32_t> imageOrder;
            std::map<std::pair<uint32_t, uint32_t>, VirtualPlacement> virtualIds;
            std::map<std::pair<uint32_t, uint32_t>, Placement> placements;
            std::vector<Placement> anonymousPlacements;
        };
        static Control _ParseControl(const std::wstring_view control) noexcept;
        void _HandleSequence(const std::wstring_view control, const std::string_view payload, const bool controlValid, const bool payloadValid, const bool payloadTooLarge);
        void _ProcessCommand(const Control& command, const std::string_view payload, const bool payloadValid, const bool payloadTooLarge);
        void _clearChunk() noexcept;
        static uint32_t _ParseUint(const std::wstring_view value) noexcept;
        static int32_t _ParseInt(const std::wstring_view value) noexcept;
        static bool _DecodeBase64(const std::string_view input, std::vector<uint8_t>& output) noexcept;
        static bool _inflateZlib(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, size_t cap) noexcept;
        static std::vector<RGBQUAD> _decodePixels(const uint32_t format, const std::vector<uint8_t>& bytes);
        static RGBQUAD _rgbaColor(uint32_t rgba) noexcept;
        static void _compositePixels(std::span<RGBQUAD> destination, std::span<const RGBQUAD> source, bool replace) noexcept;
        static size_t _frameCount(const Image& image) noexcept;
        static std::vector<RGBQUAD>* _framePixels(Image& image, uint32_t frameNumber) noexcept;
        static const std::vector<RGBQUAD>* _framePixels(const Image& image, uint32_t frameNumber) noexcept;
        static int32_t* _frameGap(Image& image, uint32_t frameNumber) noexcept;
        void _updateImageLayers(uint32_t imageId, std::span<const RGBQUAD> pixels);
        void _scheduleAnimation(uint32_t imageId, Image& image, std::chrono::steady_clock::time_point now);
        void _scheduleAnimationTimer();
        bool _advanceImage(uint32_t imageId, Image& image, std::chrono::steady_clock::time_point now);
        bool _processAnimationFrame(const Control& command, const std::string_view payload, bool payloadValid, bool payloadTooLarge, uint32_t imageId, std::wstring_view& code);
        bool _processAnimationControl(const Control& command, uint32_t imageId, std::wstring_view& code);
        bool _processFrameComposition(const Control& command, uint32_t imageId, std::wstring_view& code);
        void _deleteAnimationFrames(uint32_t imageId, uint32_t frameNumber, bool freeData);
        uint32_t _assignImageId();
        bool _registerImage(const uint32_t id, Image&& image);
        void _eraseImage(const uint32_t id);
        void _eraseImageRows(const uint32_t imageId);
        void _clearImages() noexcept;
        BufferState _takeBufferState() noexcept;
        void _restoreBufferState(BufferState&& state) noexcept;
        size_t _retainedPixelBytes() const noexcept;
        void _storeVirtualPlacement(const uint32_t id, uint32_t placementId, const Image& image, const uint32_t cols, const uint32_t rows, const uint32_t srcX, const uint32_t srcY, const uint32_t srcW, const uint32_t srcH, const int32_t zIndex, uint64_t layerId);
        static TargetSize _targetPixels(const int64_t cropW, const int64_t cropH, const uint32_t cols, const uint32_t rows, const int64_t cellWidth, const int64_t cellHeight) noexcept;
        bool _placementFitsMemory(const Image& image, uint32_t imageId, uint64_t layerId, uint32_t cols, uint32_t rows, uint32_t srcX, uint32_t srcY, uint32_t srcW, uint32_t srcH, int32_t zIndex, std::optional<til::point> anchor = std::nullopt) const noexcept;
        til::size _placeImage(const Image& image, bool moveCursor, uint32_t imageId, uint64_t layerId, uint32_t cols = 0, uint32_t rows = 0, uint32_t srcX = 0, uint32_t srcY = 0, uint32_t srcW = 0, uint32_t srcH = 0, uint32_t cellOffsetX = 0, uint32_t cellOffsetY = 0, int32_t zIndex = 0, std::optional<til::point> anchor = std::nullopt);
        // Relative placement registry helpers.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
        void _registerPlacement(const Placement& placement);
        // Re-anchor and redraw every relative descendant after a registered parent moves.
        bool _movePlacementChildren(const std::pair<uint32_t, uint32_t>& parent, til::point parentAnchor, bool apply, std::wstring_view& code);
        // Erases one placement's retained layer by its internal identity. The identity follows
        // cells through scroll, reflow, and block copies, so this never depends on a stale anchor.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
        void _erasePlacementCells(const Placement& placement);
        void _erasePlacementsForImage(const uint32_t imageId);
        // True if any tracked placement (registered or anonymous) still references this image id.
        bool _imageHasPlacements(const uint32_t id) const noexcept;
        bool _imageHasRenderedLayers(uint32_t id) const;
        // Cascade-deletes the relative children of each removed placement key (registered +
        // anonymous), deleting any orphaned image except `keepImageId`, which the caller deletes.
        void _cascadePlacementChildren(std::deque<std::pair<uint32_t, uint32_t>>& removed, const uint32_t keepImageId);
        // Deletes only the (imageId, placementId) placement and its relative children. When freeData
        // is true (an uppercase selector) imageId's data is also freed if this was its last
        // placement; when false (lowercase) the image data is kept for a later a=p.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#deleting-images
        void _deletePlacement(const uint32_t imageId, const uint32_t placementId, const bool freeData);
        void _deleteAllPlacements(bool freeData);
        void _deleteImagesIntersecting(const til::CoordType left, const til::CoordType top, const til::CoordType right, const til::CoordType bottom, const bool freeData);
        void _deleteImagesInIdRange(const uint32_t lo, const uint32_t hi, const bool freeData);
        void _deletePlacementsByZ(const int32_t zIndex, const bool freeData, const std::optional<til::point> cell = std::nullopt);
        std::optional<til::point> _resolvePlacementAnchor(const uint32_t parentImageId, const uint32_t parentPlacementId, const std::pair<uint32_t, uint32_t> origin, std::wstring_view& code) const;
        std::optional<til::point> _derivePlacementAnchor(const Placement& placement) const;
        std::optional<til::point> _deriveVirtualPlacementAnchor(uint32_t imageId, uint32_t placementId) const;
        // Returns true if a placeholder tile was drawn (the caller batches one redraw per segment).
        bool _placeImageCellRef(const Image& image, const uint32_t imageId, const til::CoordType column, const til::CoordType row, const uint32_t cellRow, const uint32_t cellCol, const VirtualPlacement& place);
        static int _PlaceholderDiacriticIndex(const char32_t ch) noexcept;

        // Kitty graphics image registry. Each id maps to an Image (number +
        // decoded BGRA pixels); a reverse number -> id map and FIFO/LRU eviction
        // bound the registry to MaxImages entries and MaxTotalBytes of
        // decoded pixels. The pixel cell size comes from the host (ITerminalApi).
        static constexpr size_t MaxImages = 4096;
        static constexpr size_t MaxControl = 1024;
        static constexpr size_t MaxPayload = 32 * 1024 * 1024;
        static constexpr size_t MaxTotalBytes = 320 * 1024 * 1024;
        static constexpr size_t MaxFramesPerImage = 4096;
        uint32_t _nextImageId = 1;
        uint64_t _nextLayerId = 1;
        size_t _totalPixelBytes = 0;
        std::unordered_map<uint32_t, Image> _images;
        std::unordered_map<uint32_t, uint32_t> _imageNumbers;
        std::deque<uint32_t> _imageOrder;

        // Virtual placements keyed by the external (image id, placement id). U+10EEEE selects
        // this key through its foreground and underline colors.
        std::map<std::pair<uint32_t, uint32_t>, VirtualPlacement> _virtualIds;
        // Placement registry, keyed by (imageId, placementId). Tracks each display of an image so
        // a relative child (P=/Q=) can be positioned against its parent and so a parent's deletion
        // cascades to its relative children. Bounded in proportion to MaxImages, to match the image LRU.
        // A relative chain may be at least MaxPlacementDepth deep; exceeding it is ETOODEEP.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
        static constexpr int MaxPlacementDepth = 8;
        static constexpr size_t MaxPlacements = MaxImages * 4;
        std::map<std::pair<uint32_t, uint32_t>, Placement> _placements;
        // Every anonymous placement (external p=0) is tracked here. Its unique internal layer id
        // makes otherwise-identical placements independently erasable while relative anonymous
        // placements can still cascade with their parent. Anonymous placements are always leaves.
        // Protocol: https://sw.kovidgoyal.net/kitty/graphics-protocol/#relative-placements
        std::vector<Placement> _anonymousPlacements;
        std::optional<BufferState> _mainBufferState;

        // Chunked transmission (m=): accumulates the base64 payload across sequences;
        // only one transfer runs at a time, processed on the final chunk (m=0).
        bool _chunkActive = false;
        bool _chunkPayloadValid = true;
        bool _chunkPayloadTooLarge = false;
        Control _chunkControl;
        std::string _chunkPayload;

        AdaptDispatch& _dispatcher;

#ifdef UNIT_TESTING
        friend class ::AdapterTest;
#endif
    };
}