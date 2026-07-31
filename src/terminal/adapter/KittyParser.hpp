// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "DispatchTypes.hpp"
#include "ITermDispatch.hpp"
#include "../../buffer/out/Image.hpp"

#include <deque>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

// fwdecl unittest classes
#ifdef UNIT_TESTING
class AdapterTest;
#endif

class ROW;
struct ImageCellRef;

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
        bool RenderPlaceholders(const std::wstring_view segment,
                                til::CoordType screenRow,
                                til::CoordType startColumn,
                                const ImageCellRef* leadingCellMetadataBeforeWrite = nullptr);

        // True when a run of text opens with kitty rowcolumn diacritics, i.e. it may be the tail
        // of a placeholder cell whose write was split before its marks. The writer has to notice
        // such a run even though it carries no U+10EEEE of its own.
        static bool StartsWithPlaceholderDiacritic(const std::wstring_view text) noexcept;

        // Reconciles relative descendants after a text-buffer operation moved or erased
        // placeholder fragments without routing through RenderPlaceholders.
        bool SynchronizeVirtualPlacementChildren();
        bool HasRelativeVirtualDescendants() const noexcept;
        class MutationSnapshot;
        std::shared_ptr<MutationSnapshot> CreateMutationSnapshot() const;
        void RestoreMutationSnapshot(MutationSnapshot& snapshot) noexcept;

        // Runs every animated image forward to the given time, then reports when it
        // next needs to be called. The host calls this when the deadline it was last
        // given comes due.
        void AdvanceAnimations(std::chrono::steady_clock::time_point now);

        // Refreshes animated shared surfaces after a page or buffer transition.
        void RefreshImageSurfaces();

        // Drops every image, every placement, and any transfer in progress.
        void HardReset() noexcept;

        // Removes the placements but keeps the transmitted image data, so a later
        // a=p can display it again. This is what an erase-display asks for.
        void ErasePlacements();

        // The registry is per-buffer. Leaving the main buffer sets its contents
        // aside and returning restores them; whatever the alternate buffer drew is
        // discarded with it.
        void SaveMainBufferState() noexcept;
        void DiscardBufferState() noexcept;
        void RestoreMainBufferState();

    private:
        class PlacementMutationGuard;

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
            uint32_t cols = 0;
            uint32_t rows = 0;
            uint32_t srcX = 0;
            uint32_t srcY = 0;
            uint32_t srcW = 0;
            uint32_t srcH = 0;
            uint32_t cellOffsetX = 0;
            uint32_t cellOffsetY = 0;
            uint32_t upperX = 0; // X=: animation replacement mode or composition source x
            uint32_t upperY = 0; // Y=: animation background RGBA or composition source y
            uint64_t fileOffset = 0; // O=: byte offset into a file or shared-memory object
            uint64_t fileSize = 0; // S=: bytes to read (0 = to end, host-bounded)
            bool moreChunks = false;
            bool mPresent = false;
            bool haveId = false;
            bool haveNumber = false;
            wchar_t medium = L'd'; // t=: transmission medium (d/f/t/s)
            bool noCursorMovement = false;
            bool hasNonChunkKey = false;
            bool hasNonChunkKeyOtherThanAction = false;
            bool virtualPlacement = false; // U=1: virtual placement (store only; drawn later via Unicode placeholders)
            uint32_t placementId = 0;
            uint32_t parentImageId = 0;
            uint32_t parentPlacementId = 0;
            int32_t offsetH = 0;
            int32_t offsetV = 0;
            int32_t zIndex = 0;
            bool haveZ = false;
            bool havePlacementId = false;
            bool haveParent = false;
        };

        using FramePixelStorage = ::Image::PixelStorage;

        struct AnimationFrame
        {
            // Conservatively covers the retained vector object, shared-pointer control
            // block, and allocator bookkeeping that the pixel byte count cannot see.
            static constexpr size_t RetainedAllocationOverhead = 128;

            FramePixelStorage pixels;
            int32_t gapMilliseconds = 0;
        };

        struct Image
        {
            uint32_t number = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            FramePixelStorage pixels;
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
            mutable ::Image::Pointer surface;

            size_t RetainedBytes() const noexcept
            {
                auto bytes = pixels ? pixels->size() * sizeof(RGBQUAD) : 0;
                const auto add = [&](const size_t value) {
                    bytes = value > SIZE_MAX - bytes ? SIZE_MAX : bytes + value;
                };
                add(animationFrames.capacity() * sizeof(AnimationFrame));
                for (const auto& frame : animationFrames)
                {
                    add(AnimationFrame::RetainedAllocationOverhead);
                    add(frame.pixels ? frame.pixels->size() * sizeof(RGBQUAD) : 0);
                }
                return bytes;
            }
        };

        using ImageNumberMap = std::unordered_map<uint32_t, std::vector<uint32_t>>;

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

        struct Placement
        {
            uint32_t imageId = 0;
            uint32_t placementId = 0;
            uint64_t layerId = 0;
            til::CoordType anchorRow = 0;
            til::CoordType anchorCol = 0;
            til::CoordType cols = 0;
            til::CoordType rows = 0;
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
            size_t totalRetainedBytes = 0;
            std::unordered_map<uint32_t, Image> images;
            ImageNumberMap imageNumbers;
            std::deque<uint32_t> imageOrder;
            std::map<std::pair<uint32_t, uint32_t>, VirtualPlacement> virtualIds;
            std::map<std::pair<uint32_t, uint32_t>, Placement> placements;
            std::vector<Placement> anonymousPlacements;
        };

        struct ImageCacheState
        {
            uint32_t id = 0;
            bool hasRenderedPlacements = false;
            ::Image::Pointer surface;
        };

        static Control _ParseControl(std::wstring_view control) noexcept;
        bool _localMediaAllowed() const noexcept;
        void _HandleSequence(std::wstring_view control, std::string_view payload, bool controlValid, bool payloadValid, bool payloadTooLarge);
        void _ProcessCommand(const Control& command, std::string_view payload, bool payloadValid, bool payloadTooLarge);
        void _clearChunk() noexcept;
        static uint32_t _ParseUint(std::wstring_view value) noexcept;
        static int32_t _ParseInt(std::wstring_view value) noexcept;
        static uint64_t _ParseUint64(std::wstring_view value) noexcept;
        static bool _DecodeBase64(std::string_view input, std::vector<uint8_t>& output) noexcept;
        static bool _inflateZlib(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, size_t cap, bool allowTrailingZeroPadding = false) noexcept;
        static std::vector<RGBQUAD> _decodePixels(uint32_t format, const std::vector<uint8_t>& bytes);
        static RGBQUAD _rgbaColor(uint32_t rgba) noexcept;
        static void _compositePixels(std::span<RGBQUAD> destination, std::span<const RGBQUAD> source, bool replace) noexcept;
        static size_t _frameCount(const Image& image) noexcept;
        static const std::vector<RGBQUAD>* _framePixels(const Image& image, uint32_t frameNumber) noexcept;
        static FramePixelStorage* _frameStorage(Image& image, uint32_t frameNumber) noexcept;
        static const FramePixelStorage* _frameStorage(const Image& image, uint32_t frameNumber) noexcept;
        static int32_t* _frameGap(Image& image, uint32_t frameNumber) noexcept;
        void _updateImageSurface(uint32_t imageId);
        void _updateImageSurfaces(std::span<const uint32_t> imageIds);
        void _scheduleAnimation(uint32_t imageId, Image& image, std::chrono::steady_clock::time_point now, std::vector<uint32_t>* updatedImageIds = nullptr);
        void _scheduleAnimationTimer();
        std::optional<std::chrono::steady_clock::time_point> _nextAnimationDeadline(uint32_t replacementImageId = 0, const Image* replacement = nullptr, std::span<const uint32_t> omittedImageIds = {}) const noexcept;
        bool _advanceImage(uint32_t imageId, Image& image, std::chrono::steady_clock::time_point now, std::vector<uint32_t>* updatedImageIds = nullptr);
        bool _processAnimationFrame(const Control& command, const std::string_view payload, bool payloadValid, bool payloadTooLarge, uint32_t imageId, std::wstring_view& code);
        bool _processAnimationControl(const Control& command, uint32_t imageId, std::wstring_view& code);
        bool _processFrameComposition(const Control& command, uint32_t imageId, std::wstring_view& code);
        void _deleteAnimationFrames(uint32_t imageId, uint32_t frameNumber, bool freeData);
        uint32_t _assignImageId();
        bool _registerImage(uint32_t id, Image&& image);
        void _eraseImage(uint32_t id);
        void _touchImage(uint32_t id) noexcept;
        bool _isImagePlaced(uint32_t id) const;
        void _eraseImageRegistryEntry(uint32_t id) noexcept;
        void _clearImages() noexcept;
        BufferState _takeBufferState() noexcept;
        void _restoreBufferState(BufferState&& state) noexcept;
        size_t _retainedBytes() const noexcept;
        void _releaseImageSurface(Image& image) noexcept;
        std::vector<ImageCacheState> _snapshotImageCacheStates() const;
        void _restoreImageCacheStates(const std::vector<ImageCacheState>& states) noexcept;
        void _storeVirtualPlacement(const uint32_t id, uint32_t placementId, const Image& image, const uint32_t cols, const uint32_t rows, const uint32_t srcX, const uint32_t srcY, const uint32_t srcW, const uint32_t srcH, const int32_t zIndex, uint64_t layerId);
        static TargetSize _targetPixels(int64_t cropW, int64_t cropH, uint32_t cols, uint32_t rows, int64_t cellWidth, int64_t cellHeight) noexcept;
        til::size _placeImage(const Image& image, bool moveCursor, uint32_t imageId, uint64_t layerId, uint32_t cols = 0, uint32_t rows = 0, uint32_t srcX = 0, uint32_t srcY = 0, uint32_t srcW = 0, uint32_t srcH = 0, uint32_t cellOffsetX = 0, uint32_t cellOffsetY = 0, int32_t zIndex = 0, std::optional<til::point> anchor = std::nullopt);
        void _registerPlacement(const Placement& placement);
        bool _movePlacementChildren(const std::pair<uint32_t, uint32_t>& parent, std::optional<til::point> parentAnchor, bool apply, std::wstring_view& code);
        void _erasePlacementCells(const Placement& placement);
        void _erasePlacementsForImage(uint32_t imageId);
        bool _imageHasPlacements(uint32_t id) const noexcept;
        bool _imageHasRenderedPlacements(uint32_t id) const;
        void _cascadePlacementChildren(std::deque<std::pair<uint32_t, uint32_t>>& removed, uint32_t keepImageId);
        void _deletePlacement(uint32_t imageId, uint32_t placementId, bool freeData);
        void _eraseImagePlacements(uint32_t imageId);
        void _deleteAllPlacements(bool freeData);
        void _deleteImagesIntersecting(til::CoordType left, til::CoordType top, til::CoordType right, til::CoordType bottom, bool freeData);
        void _deleteImagesInIdRange(uint32_t lo, uint32_t hi, bool freeData);
        void _deletePlacementsByZ(int32_t zIndex, bool freeData, std::optional<til::point> cell = std::nullopt);
        std::optional<til::point> _resolvePlacementAnchor(uint32_t parentImageId, uint32_t parentPlacementId, std::pair<uint32_t, uint32_t> origin, std::wstring_view& code) const;
        std::optional<til::point> _derivePlacementAnchor(const Placement& placement) const;
        std::optional<til::point> _deriveVirtualPlacementAnchor(uint32_t imageId, uint32_t placementId) const;
        // Returns true if a placeholder tile was staged (the caller publishes and redraws once per segment).
        bool _placeImageCellRef(const Image& image, const uint32_t imageId, const til::CoordType column, const til::CoordType row, const uint32_t cellRow, const uint32_t cellCol, const VirtualPlacement& place, std::vector<ImagePlacement>& fragments);
        // Resolves one placeholder text cell from its complete grapheme cluster and stages the
        // tile it addresses. Returns true if a tile was staged.
        bool _renderPlaceholderCell(ROW& row, const std::wstring_view cluster, const til::CoordType column, const til::CoordType screenRow, std::vector<ImagePlacement>& fragments);
        static int _PlaceholderDiacriticIndex(const char32_t ch) noexcept;
        static bool _IsPlaceholderDiacriticRun(const std::wstring_view cluster) noexcept;

        static constexpr size_t MaxImages = 4096;
        static constexpr size_t MaxControl = 1024;
        static constexpr size_t MaxPayload = 32 * 1024 * 1024;
        static constexpr size_t MaxTotalBytes = 320 * 1024 * 1024;
        static constexpr size_t MaxFramesPerImage = 4096;
        static constexpr int MaxPlacementDepth = 8;
        static constexpr size_t MaxPlacements = MaxImages * 4;
        uint32_t _nextImageId = 1;
        uint64_t _nextLayerId = 1;
        size_t _totalRetainedBytes = 0;
        std::unordered_map<uint32_t, Image> _images;
        ImageNumberMap _imageNumbers;
        std::deque<uint32_t> _imageOrder;
        // Virtual placements keyed by the external (image id, placement id). U+10EEEE selects
        // this key through its foreground and underline colors.
        std::map<std::pair<uint32_t, uint32_t>, VirtualPlacement> _virtualIds;
        std::map<std::pair<uint32_t, uint32_t>, Placement> _placements;
        std::vector<Placement> _anonymousPlacements;
        std::optional<BufferState> _mainBufferState;

        bool _chunkActive = false;
        bool _chunkPayloadValid = true;
        bool _chunkPayloadTooLarge = false;
        Control _chunkControl;
        std::string _chunkPayload;

        AdaptDispatch& _dispatcher;

#ifdef UNIT_TESTING
        friend class ::AdapterTest;
#endif
        // Test seams. Each operation takes and clears its one-shot mutation
        // checkpoint countdown before planning, so an armed failure cannot leak
        // into another transaction.
        std::optional<size_t> _testMovePlacementFailureCountdown;
        std::optional<size_t> _testCascadeFailureAfterEraseCountdown;
        bool _testPersistentMovePlacementFailure = false;
    };
}
