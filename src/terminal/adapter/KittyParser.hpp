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

namespace Microsoft::Console::VirtualTerminal
{
    class AdaptDispatch;

    class KittyParser
    {
    public:
        explicit KittyParser(AdaptDispatch& dispatcher) noexcept;

        // Collects one APC G sequence. The parser has already consumed the 'G'
        // identifier and routed us here on the strength of it.
        ITermDispatch::StringHandler DefineImage();

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
            uint32_t cols = 0;
            uint32_t rows = 0;
            uint32_t srcX = 0;
            uint32_t srcY = 0;
            uint32_t srcW = 0;
            uint32_t srcH = 0;
            uint32_t cellOffsetX = 0;
            uint32_t cellOffsetY = 0;
            bool moreChunks = false;
            bool mPresent = false;
            bool haveId = false;
            bool haveNumber = false;
            wchar_t medium = L'd';
            bool noCursorMovement = false;
            bool hasNonChunkKey = false;
            bool hasNonChunkKeyOtherThanAction = false;
            uint32_t placementId = 0;
            int32_t zIndex = 0;
            bool haveZ = false;
            bool havePlacementId = false;
        };

        struct Image
        {
            uint32_t number = 0;
            uint32_t width = 0;
            uint32_t height = 0;
            ::Image::PixelStorage pixels;
            bool hasRenderedPlacements = false;
            mutable ::Image::Pointer surface;

            size_t PixelBytes() const noexcept
            {
                return pixels ? pixels->size() * sizeof(RGBQUAD) : 0;
            }
        };

        using ImageNumberMap = std::unordered_map<uint32_t, std::vector<uint32_t>>;

        struct TargetSize
        {
            int64_t width = 0;
            int64_t height = 0;
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
            int32_t zIndex = 0;
        };

        struct BufferState
        {
            uint32_t nextImageId = 1;
            uint64_t nextLayerId = 1;
            size_t totalPixelBytes = 0;
            std::unordered_map<uint32_t, Image> images;
            ImageNumberMap imageNumbers;
            std::deque<uint32_t> imageOrder;
            std::map<std::pair<uint32_t, uint32_t>, Placement> placements;
            std::vector<Placement> anonymousPlacements;
        };

        static Control _ParseControl(std::wstring_view control) noexcept;
        bool _localMediaAllowed() const noexcept;
        void _HandleSequence(std::wstring_view control, std::string_view payload, bool controlValid, bool payloadValid, bool payloadTooLarge);
        void _ProcessCommand(const Control& command, std::string_view payload, bool payloadValid, bool payloadTooLarge);
        void _clearChunk() noexcept;
        static uint32_t _ParseUint(std::wstring_view value) noexcept;
        static int32_t _ParseInt(std::wstring_view value) noexcept;
        static bool _DecodeBase64(std::string_view input, std::vector<uint8_t>& output) noexcept;
        static bool _inflateZlib(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, size_t cap) noexcept;
        static std::vector<RGBQUAD> _decodePixels(uint32_t format, const std::vector<uint8_t>& bytes);
        uint32_t _assignImageId();
        bool _registerImage(uint32_t id, Image&& image);
        void _eraseImage(uint32_t id);
        void _touchImage(uint32_t id) noexcept;
        bool _isImagePlaced(uint32_t id) const;
        void _clearImages() noexcept;
        BufferState _takeBufferState() noexcept;
        void _restoreBufferState(BufferState&& state) noexcept;
        size_t _retainedPixelBytes() const noexcept;
        void _releaseImageSurface(Image& image) noexcept;
        static TargetSize _targetPixels(int64_t cropW, int64_t cropH, uint32_t cols, uint32_t rows, int64_t cellWidth, int64_t cellHeight) noexcept;
        til::size _placeImage(const Image& image, bool moveCursor, uint32_t imageId, uint64_t layerId, uint32_t cols = 0, uint32_t rows = 0, uint32_t srcX = 0, uint32_t srcY = 0, uint32_t srcW = 0, uint32_t srcH = 0, uint32_t cellOffsetX = 0, uint32_t cellOffsetY = 0, int32_t zIndex = 0);
        void _registerPlacement(const Placement& placement);
        void _erasePlacementCells(const Placement& placement);
        void _erasePlacementsForImage(uint32_t imageId);
        bool _imageHasPlacements(uint32_t id) const noexcept;
        bool _imageHasRenderedPlacements(uint32_t id) const;
        void _deletePlacement(uint32_t imageId, uint32_t placementId, bool freeData);
        void _eraseImagePlacements(uint32_t imageId);
        void _deleteAllPlacements(bool freeData);
        void _deleteImagesIntersecting(til::CoordType left, til::CoordType top, til::CoordType right, til::CoordType bottom, bool freeData);
        void _deleteImagesInIdRange(uint32_t lo, uint32_t hi, bool freeData);
        void _deletePlacementsByZ(int32_t zIndex, bool freeData, std::optional<til::point> cell = std::nullopt);

        static constexpr size_t MaxImages = 4096;
        static constexpr size_t MaxControl = 1024;
        static constexpr size_t MaxPayload = 32 * 1024 * 1024;
        static constexpr size_t MaxTotalBytes = 320 * 1024 * 1024;
        static constexpr size_t MaxPlacements = MaxImages * 4;
        uint32_t _nextImageId = 1;
        uint64_t _nextLayerId = 1;
        size_t _totalPixelBytes = 0;
        std::unordered_map<uint32_t, Image> _images;
        ImageNumberMap _imageNumbers;
        std::deque<uint32_t> _imageOrder;
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
    };
}
