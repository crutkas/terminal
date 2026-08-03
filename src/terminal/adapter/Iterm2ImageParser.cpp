// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "Iterm2ImageParser.hpp"
#include "../parser/ascii.hpp"

using namespace Microsoft::Console::VirtualTerminal;
using namespace std::string_view_literals;

namespace
{
    constexpr uint8_t Padding = 0xff;

    constexpr std::optional<uint8_t> base64Value(const wchar_t ch) noexcept
    {
        if (ch >= L'A' && ch <= L'Z')
        {
            return gsl::narrow_cast<uint8_t>(ch - L'A');
        }
        if (ch >= L'a' && ch <= L'z')
        {
            return gsl::narrow_cast<uint8_t>(ch - L'a' + 26);
        }
        if (ch >= L'0' && ch <= L'9')
        {
            return gsl::narrow_cast<uint8_t>(ch - L'0' + 52);
        }
        if (ch == L'+')
        {
            return uint8_t{ 62 };
        }
        if (ch == L'/')
        {
            return uint8_t{ 63 };
        }
        if (ch == L'=')
        {
            return Padding;
        }
        return std::nullopt;
    }

    constexpr bool isTerminator(const wchar_t ch) noexcept
    {
        return ch == AsciiChars::BEL || ch == AsciiChars::ESC;
    }

    constexpr bool isCancellation(const wchar_t ch) noexcept
    {
        return ch == AsciiChars::CAN || ch == AsciiChars::SUB;
    }
}

Iterm2ImageParser::Transfer::Transfer(Metadata&& metadata, std::vector<uint8_t>&& data, const size_t encodedSize, const size_t decodedSize) noexcept :
    _metadata{ std::move(metadata) },
    _data{ std::move(data) },
    _encodedSize{ encodedSize },
    _decodedSize{ decodedSize }
{
}

const Iterm2ImageParser::Metadata& Iterm2ImageParser::Transfer::GetMetadata() const noexcept
{
    return _metadata;
}

std::span<const uint8_t> Iterm2ImageParser::Transfer::Data() const noexcept
{
    return _data;
}

size_t Iterm2ImageParser::Transfer::EncodedSize() const noexcept
{
    return _encodedSize;
}

size_t Iterm2ImageParser::Transfer::DecodedSize() const noexcept
{
    return _decodedSize;
}

Iterm2ImageParser::Base64Decoder::Base64Decoder(const bool retainData, const size_t maxEncodedSize, const size_t maxDecodedSize) noexcept :
    _retainData{ retainData },
    _maxEncodedSize{ maxEncodedSize },
    _maxDecodedSize{ maxDecodedSize }
{
}

bool Iterm2ImageParser::Base64Decoder::Add(const wchar_t ch)
{
    if (!_valid || _finished || _encodedSize >= _maxEncodedSize)
    {
        _valid = false;
        return false;
    }

    const auto value = base64Value(ch);
    if (!value)
    {
        _valid = false;
        return false;
    }

    _encodedSize++;
    _quartet.at(_quartetSize++) = *value;
    if (_quartetSize == _quartet.size())
    {
        _valid = _decodeQuartet();
        _quartetSize = 0;
    }
    return _valid;
}

bool Iterm2ImageParser::Base64Decoder::Finalize() noexcept
{
    _valid = _valid && _quartetSize == 0;
    return _valid;
}

size_t Iterm2ImageParser::Base64Decoder::EncodedSize() const noexcept
{
    return _encodedSize;
}

size_t Iterm2ImageParser::Base64Decoder::DecodedSize() const noexcept
{
    return _decodedSize;
}

std::vector<uint8_t> Iterm2ImageParser::Base64Decoder::TakeData() noexcept
{
    return std::move(_data);
}

bool Iterm2ImageParser::Base64Decoder::_decodeQuartet()
{
    const auto a = _quartet.at(0);
    const auto b = _quartet.at(1);
    const auto c = _quartet.at(2);
    const auto d = _quartet.at(3);
    if (a == Padding || b == Padding)
    {
        return false;
    }

    if (c == Padding)
    {
        if (d != Padding || (b & 0x0f) != 0)
        {
            return false;
        }
        const std::array bytes{ gsl::narrow_cast<uint8_t>((a << 2) | (b >> 4)) };
        _finished = true;
        return _emit(bytes);
    }

    if (d == Padding)
    {
        if ((c & 0x03) != 0)
        {
            return false;
        }
        const std::array bytes{
            gsl::narrow_cast<uint8_t>((a << 2) | (b >> 4)),
            gsl::narrow_cast<uint8_t>((b << 4) | (c >> 2)),
        };
        _finished = true;
        return _emit(bytes);
    }

    const std::array bytes{
        gsl::narrow_cast<uint8_t>((a << 2) | (b >> 4)),
        gsl::narrow_cast<uint8_t>((b << 4) | (c >> 2)),
        gsl::narrow_cast<uint8_t>((c << 6) | d),
    };
    return _emit(bytes);
}

bool Iterm2ImageParser::Base64Decoder::_emit(const std::span<const uint8_t> bytes)
{
    if (_decodedSize > _maxDecodedSize || bytes.size() > _maxDecodedSize - _decodedSize)
    {
        return false;
    }

    _decodedSize += bytes.size();
    if (_retainData)
    {
        _data.insert(_data.end(), bytes.begin(), bytes.end());
    }
    return true;
}

Iterm2ImageParser::TransferState::TransferState(Metadata&& metadata) :
    metadata{ std::move(metadata) },
    decoder{ this->metadata.inlineDisplay, MaxTransferEncodedSize, MaxTransferDecodedSize }
{
}

Iterm2ImageParser::Iterm2ImageParser(CompletionHandler completionHandler) :
    _completionHandler{ std::move(completionHandler) }
{
}

IStateMachineEngine::OscStringHandler Iterm2ImageParser::Handler()
{
    return [this, sequence = SequenceState{}](const wchar_t ch) mutable {
        try
        {
            return _process(sequence, ch);
        }
        catch (const std::bad_alloc&)
        {
            LOG_HR(E_OUTOFMEMORY);
            _multipartTransfer.reset();
            return IStateMachineEngine::OscStringHandlerResult::Abort;
        }
        catch (const wil::ResultException& exception)
        {
            LOG_HR(exception.GetErrorCode());
            _multipartTransfer.reset();
            return IStateMachineEngine::OscStringHandlerResult::Abort;
        }
    };
}

void Iterm2ImageParser::Reset() noexcept
{
    _multipartTransfer.reset();
}

IStateMachineEngine::OscStringHandlerResult Iterm2ImageParser::_process(SequenceState& sequence, const wchar_t ch)
{
    if (isCancellation(ch))
    {
        return _cancel(sequence);
    }
    if (isTerminator(ch))
    {
        return _finish(sequence);
    }
    if (sequence.kind == SequenceKind::Probe)
    {
        return _probe(sequence, ch);
    }

    switch (sequence.kind)
    {
    case SequenceKind::LegacyFile:
        if (!sequence.metadataEnded)
        {
            if (ch == L':')
            {
                sequence.metadataEnded = true;
                Metadata metadata;
                if (sequence.valid && _parseMetadata(sequence.metadata, metadata))
                {
                    sequence.legacyTransfer.emplace(std::move(metadata));
                }
                else
                {
                    sequence.valid = false;
                    sequence.metadata.clear();
                }
            }
            else
            {
                _appendMetadata(sequence, ch);
            }
        }
        else
        {
            _appendLegacyData(sequence, ch);
        }
        break;
    case SequenceKind::MultipartStart:
        if (ch == L':')
        {
            sequence.valid = false;
            sequence.metadata.clear();
        }
        else
        {
            _appendMetadata(sequence, ch);
        }
        break;
    case SequenceKind::FilePart:
        _appendPartData(sequence, ch);
        break;
    case SequenceKind::FileEnd:
        sequence.valid = false;
        _multipartTransfer.reset();
        break;
    default:
        break;
    }

    return IStateMachineEngine::OscStringHandlerResult::Accept;
}

IStateMachineEngine::OscStringHandlerResult Iterm2ImageParser::_probe(SequenceState& sequence, const wchar_t ch)
{
    static constexpr std::array tokens{
        std::pair{ L"File="sv, SequenceKind::LegacyFile },
        std::pair{ L"MultipartFile="sv, SequenceKind::MultipartStart },
        std::pair{ L"FilePart="sv, SequenceKind::FilePart },
        std::pair{ L"FileEnd"sv, SequenceKind::FileEnd },
    };

    sequence.probe.push_back(ch);
    auto isPrefix = false;
    for (const auto& [token, kind] : tokens)
    {
        if (token.starts_with(sequence.probe))
        {
            isPrefix = true;
            if (token.size() == sequence.probe.size())
            {
                _begin(sequence, kind);
                return IStateMachineEngine::OscStringHandlerResult::Accept;
            }
        }
    }

    return isPrefix ?
               IStateMachineEngine::OscStringHandlerResult::Pending :
               IStateMachineEngine::OscStringHandlerResult::Fallback;
}

IStateMachineEngine::OscStringHandlerResult Iterm2ImageParser::_finish(SequenceState& sequence)
{
    switch (sequence.kind)
    {
    case SequenceKind::Probe:
        return IStateMachineEngine::OscStringHandlerResult::Fallback;
    case SequenceKind::LegacyFile:
        if (sequence.valid && sequence.metadataEnded && sequence.legacyTransfer)
        {
            _complete(std::move(*sequence.legacyTransfer));
        }
        return IStateMachineEngine::OscStringHandlerResult::Accept;
    case SequenceKind::MultipartStart:
    {
        Metadata metadata;
        if (sequence.valid && _parseMetadata(sequence.metadata, metadata))
        {
            _multipartTransfer.emplace(std::move(metadata));
        }
        return IStateMachineEngine::OscStringHandlerResult::Accept;
    }
    case SequenceKind::FilePart:
        if (sequence.valid && _multipartTransfer)
        {
            _multipartTransfer->partCount++;
        }
        else
        {
            _multipartTransfer.reset();
        }
        return IStateMachineEngine::OscStringHandlerResult::Accept;
    case SequenceKind::FileEnd:
        if (sequence.valid)
        {
            _completeMultipart();
        }
        else
        {
            _multipartTransfer.reset();
        }
        return IStateMachineEngine::OscStringHandlerResult::Accept;
    default:
        return IStateMachineEngine::OscStringHandlerResult::Abort;
    }
}

IStateMachineEngine::OscStringHandlerResult Iterm2ImageParser::_cancel(SequenceState& sequence) noexcept
{
    if (sequence.kind == SequenceKind::FilePart || sequence.kind == SequenceKind::FileEnd)
    {
        _multipartTransfer.reset();
    }
    return IStateMachineEngine::OscStringHandlerResult::Abort;
}

void Iterm2ImageParser::_begin(SequenceState& sequence, const SequenceKind kind) noexcept
{
    sequence.kind = kind;
    sequence.probe.clear();
    if (kind == SequenceKind::LegacyFile || kind == SequenceKind::MultipartStart)
    {
        _multipartTransfer.reset();
    }
}

void Iterm2ImageParser::_appendMetadata(SequenceState& sequence, const wchar_t ch)
{
    if (!sequence.valid)
    {
        return;
    }
    if (sequence.metadata.size() >= MaxMetadataSize)
    {
        sequence.valid = false;
        sequence.metadata.clear();
        return;
    }
    sequence.metadata.push_back(ch);
}

void Iterm2ImageParser::_appendLegacyData(SequenceState& sequence, const wchar_t ch)
{
    if (!sequence.valid || !sequence.legacyTransfer)
    {
        return;
    }
    if (!sequence.legacyTransfer->decoder.Add(ch))
    {
        sequence.valid = false;
        sequence.legacyTransfer.reset();
    }
}

void Iterm2ImageParser::_appendPartData(SequenceState& sequence, const wchar_t ch)
{
    if (sequence.partEncodedSize >= MaxPartEncodedSize)
    {
        sequence.valid = false;
        _multipartTransfer.reset();
        return;
    }
    sequence.partEncodedSize++;

    if (sequence.valid && _multipartTransfer && !_multipartTransfer->decoder.Add(ch))
    {
        sequence.valid = false;
        _multipartTransfer.reset();
    }
}

bool Iterm2ImageParser::_complete(TransferState&& transfer)
{
    if (!transfer.decoder.Finalize())
    {
        return false;
    }

    const auto decodedSize = transfer.decoder.DecodedSize();
    if (transfer.metadata.declaredSize && *transfer.metadata.declaredSize != decodedSize)
    {
        return false;
    }
    if (!transfer.metadata.inlineDisplay)
    {
        return true;
    }

    Transfer result{
        std::move(transfer.metadata),
        transfer.decoder.TakeData(),
        transfer.decoder.EncodedSize(),
        decodedSize,
    };
    if (_completionHandler)
    {
        _completionHandler(result);
    }
    return true;
}

bool Iterm2ImageParser::_completeMultipart()
{
    if (!_multipartTransfer || _multipartTransfer->partCount == 0)
    {
        _multipartTransfer.reset();
        return false;
    }

    auto transfer = std::move(*_multipartTransfer);
    _multipartTransfer.reset();
    return _complete(std::move(transfer));
}

bool Iterm2ImageParser::_parseMetadata(const std::wstring_view value, Metadata& metadata)
{
    enum Seen : uint8_t
    {
        Name = 1 << 0,
        Size = 1 << 1,
        Width = 1 << 2,
        Height = 1 << 3,
        PreserveAspectRatio = 1 << 4,
        Inline = 1 << 5,
    };

    uint8_t seen = 0;
    size_t offset = 0;
    while (offset <= value.size())
    {
        const auto delimiter = value.find(L';', offset);
        const auto end = delimiter == std::wstring_view::npos ? value.size() : delimiter;
        const auto part = value.substr(offset, end - offset);
        if (!part.empty())
        {
            const auto equals = part.find(L'=');
            const auto key = part.substr(0, equals);
            const auto recognized = key == L"name" ||
                                    key == L"size" ||
                                    key == L"width" ||
                                    key == L"height" ||
                                    key == L"preserveAspectRatio" ||
                                    key == L"inline";
            if (recognized && equals == std::wstring_view::npos)
            {
                return false;
            }
            if (equals != std::wstring_view::npos)
            {
                const auto field = part.substr(equals + 1);
                const auto setOnce = [&](const Seen flag) {
                    if ((seen & flag) != 0)
                    {
                        return false;
                    }
                    seen |= flag;
                    return true;
                };

                if (key == L"name")
                {
                    if (!setOnce(Name) || !_decodeName(field, metadata.name))
                    {
                        return false;
                    }
                }
                else if (key == L"size")
                {
                    const auto parsed = _parseUnsigned(field, MaxTransferDecodedSize);
                    if (!setOnce(Size) || !parsed)
                    {
                        return false;
                    }
                    metadata.declaredSize = gsl::narrow_cast<size_t>(*parsed);
                }
                else if (key == L"width")
                {
                    if (!setOnce(Width) || !_parseDimension(field, metadata.width))
                    {
                        return false;
                    }
                }
                else if (key == L"height")
                {
                    if (!setOnce(Height) || !_parseDimension(field, metadata.height))
                    {
                        return false;
                    }
                }
                else if (key == L"preserveAspectRatio")
                {
                    if (!setOnce(PreserveAspectRatio) || (field != L"0" && field != L"1"))
                    {
                        return false;
                    }
                    metadata.preserveAspectRatio = field == L"1";
                }
                else if (key == L"inline")
                {
                    if (!setOnce(Inline) || (field != L"0" && field != L"1"))
                    {
                        return false;
                    }
                    metadata.inlineDisplay = field == L"1";
                }
            }
        }

        if (delimiter == std::wstring_view::npos)
        {
            break;
        }
        offset = delimiter + 1;
    }
    return true;
}

bool Iterm2ImageParser::_parseDimension(const std::wstring_view value, Dimension& dimension) noexcept
{
    if (value == L"auto")
    {
        dimension = {};
        return true;
    }

    auto number = value;
    auto unit = DimensionUnit::Cells;
    if (value.ends_with(L"px"))
    {
        number = value.substr(0, value.size() - 2);
        unit = DimensionUnit::Pixels;
    }
    else if (value.ends_with(L"%"))
    {
        number = value.substr(0, value.size() - 1);
        unit = DimensionUnit::Percent;
    }

    const auto parsed = _parseUnsigned(number, UINT32_MAX);
    if (!parsed)
    {
        return false;
    }
    dimension = {
        .value = gsl::narrow_cast<uint32_t>(*parsed),
        .unit = unit,
    };
    return true;
}

std::optional<uint64_t> Iterm2ImageParser::_parseUnsigned(const std::wstring_view value, const uint64_t maximum) noexcept
{
    if (value.empty())
    {
        return std::nullopt;
    }

    uint64_t result = 0;
    for (const auto ch : value)
    {
        if (ch < L'0' || ch > L'9')
        {
            return std::nullopt;
        }
        const auto digit = gsl::narrow_cast<uint64_t>(ch - L'0');
        if (result > (maximum - digit) / 10)
        {
            return std::nullopt;
        }
        result = result * 10 + digit;
    }
    return result;
}

bool Iterm2ImageParser::_decodeName(const std::wstring_view value, std::wstring& name)
{
    Base64Decoder decoder{ true, MaxMetadataSize, MaxNameDecodedSize };
    for (const auto ch : value)
    {
        if (!decoder.Add(ch))
        {
            return false;
        }
    }
    if (!decoder.Finalize())
    {
        return false;
    }

    const auto bytes = decoder.TakeData();
    const std::string_view utf8{ reinterpret_cast<const char*>(bytes.data()), bytes.size() };
    return SUCCEEDED(til::u8u16(utf8, name));
}
