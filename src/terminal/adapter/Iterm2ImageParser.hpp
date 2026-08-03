// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include "../parser/IStateMachineEngine.hpp"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifdef UNIT_TESTING
class Iterm2ImageParserTests;
#endif

namespace Microsoft::Console::VirtualTerminal
{
    class Iterm2ImageParser final
    {
    public:
        enum class DimensionUnit : uint8_t
        {
            Auto,
            Cells,
            Pixels,
            Percent,
        };

        struct Dimension
        {
            uint32_t value = 0;
            DimensionUnit unit = DimensionUnit::Auto;

            constexpr bool operator==(const Dimension&) const noexcept = default;
        };

        struct Metadata
        {
            std::wstring name;
            std::optional<size_t> declaredSize;
            Dimension width;
            Dimension height;
            bool preserveAspectRatio = true;
            bool inlineDisplay = false;
        };

        class Transfer final
        {
        public:
            const Metadata& GetMetadata() const noexcept;
            std::span<const uint8_t> Data() const noexcept;
            size_t EncodedSize() const noexcept;
            size_t DecodedSize() const noexcept;

        private:
            friend class Iterm2ImageParser;
            Transfer(Metadata&& metadata, std::vector<uint8_t>&& data, size_t encodedSize, size_t decodedSize) noexcept;

            Metadata _metadata;
            std::vector<uint8_t> _data;
            size_t _encodedSize = 0;
            size_t _decodedSize = 0;
        };

        using CompletionHandler = std::function<void(const Transfer&)>;

        static constexpr size_t MaxPartEncodedSize = 1 * 1024 * 1024;
        static constexpr size_t MaxTransferEncodedSize = 32 * 1024 * 1024;
        static constexpr size_t MaxTransferDecodedSize = 32 * 1024 * 1024;
        static constexpr size_t MaxMetadataSize = 64 * 1024;
        static constexpr size_t MaxNameDecodedSize = 4 * 1024;

        explicit Iterm2ImageParser(CompletionHandler completionHandler = {});

        IStateMachineEngine::OscStringHandler Handler();
        void Reset() noexcept;

    private:
        class Base64Decoder final
        {
        public:
            Base64Decoder(bool retainData, size_t maxEncodedSize, size_t maxDecodedSize) noexcept;

            bool Add(wchar_t ch);
            bool Finalize() noexcept;
            size_t EncodedSize() const noexcept;
            size_t DecodedSize() const noexcept;
            std::vector<uint8_t> TakeData() noexcept;

        private:
            bool _decodeQuartet();
            bool _emit(std::span<const uint8_t> bytes);

            bool _retainData = false;
            bool _valid = true;
            bool _finished = false;
            size_t _maxEncodedSize = 0;
            size_t _maxDecodedSize = 0;
            size_t _encodedSize = 0;
            size_t _decodedSize = 0;
            std::array<uint8_t, 4> _quartet{};
            size_t _quartetSize = 0;
            std::vector<uint8_t> _data;

#ifdef UNIT_TESTING
            friend class ::Iterm2ImageParserTests;
#endif
        };

        enum class SequenceKind : uint8_t
        {
            Probe,
            LegacyFile,
            MultipartStart,
            FilePart,
            FileEnd,
        };

        struct TransferState
        {
            explicit TransferState(Metadata&& metadata);

            Metadata metadata;
            Base64Decoder decoder;
            size_t partCount = 0;
        };

        struct SequenceState
        {
            SequenceKind kind = SequenceKind::Probe;
            std::wstring probe;
            std::wstring metadata;
            std::optional<TransferState> legacyTransfer;
            size_t partEncodedSize = 0;
            bool metadataEnded = false;
            bool valid = true;
        };

        IStateMachineEngine::OscStringHandlerResult _process(SequenceState& sequence, wchar_t ch);
        IStateMachineEngine::OscStringHandlerResult _probe(SequenceState& sequence, wchar_t ch);
        IStateMachineEngine::OscStringHandlerResult _finish(SequenceState& sequence);
        IStateMachineEngine::OscStringHandlerResult _cancel(SequenceState& sequence) noexcept;

        void _begin(SequenceState& sequence, SequenceKind kind) noexcept;
        void _appendMetadata(SequenceState& sequence, wchar_t ch);
        void _appendLegacyData(SequenceState& sequence, wchar_t ch);
        void _appendPartData(SequenceState& sequence, wchar_t ch);
        bool _complete(TransferState&& transfer);
        bool _completeMultipart();

        static bool _parseMetadata(std::wstring_view value, Metadata& metadata);
        static bool _parseDimension(std::wstring_view value, Dimension& dimension) noexcept;
        static std::optional<uint64_t> _parseUnsigned(std::wstring_view value, uint64_t maximum) noexcept;
        static bool _decodeName(std::wstring_view value, std::wstring& name);

        CompletionHandler _completionHandler;
        std::optional<TransferState> _multipartTransfer;

#ifdef UNIT_TESTING
        friend class ::Iterm2ImageParserTests;
#endif
    };
}
