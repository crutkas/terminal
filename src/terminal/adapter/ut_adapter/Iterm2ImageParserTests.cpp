// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include <wextestclass.h>
#include "../../inc/consoletaeftemplates.hpp"

#include "Iterm2ImageParser.hpp"
#include "termDispatch.hpp"
#include "../../parser/ascii.hpp"
#include "../../parser/OutputStateMachineEngine.hpp"
#include "../../parser/stateMachine.hpp"

using namespace Microsoft::Console::VirtualTerminal;
using namespace WEX::Logging;
using namespace std::string_view_literals;

class Iterm2ImageParserTests final
{
    TEST_CLASS(Iterm2ImageParserTests);

    using Result = IStateMachineEngine::OscStringHandlerResult;

    struct CapturedTransfer
    {
        Iterm2ImageParser::Metadata metadata;
        std::vector<uint8_t> data;
        size_t encodedSize = 0;
        size_t decodedSize = 0;
    };

    class StreamingDispatch final : public TermDispatch
    {
    public:
        StreamingDispatch() :
            parser{ [&](const Iterm2ImageParser::Transfer& transfer) {
                data.assign(transfer.Data().begin(), transfer.Data().end());
                completions++;
            } }
        {
        }

        void Print(const wchar_t) override
        {
        }

        void PrintString(const std::wstring_view) override
        {
        }

        IStateMachineEngine::OscStringHandler Iterm2Image() override
        {
            return parser.Handler();
        }

        void DoITerm2Action(const std::wstring_view string) override
        {
            ordinaryActions.emplace_back(string);
        }

        Iterm2ImageParser parser;
        std::vector<std::wstring> ordinaryActions;
        std::vector<uint8_t> data;
        size_t completions = 0;
    };

    static Result Send(Iterm2ImageParser& parser, const std::wstring_view payload, const wchar_t terminator = AsciiChars::BEL)
    {
        auto handler = parser.Handler();
        auto result = Result::Pending;
        for (const auto ch : payload)
        {
            result = handler(ch);
            if (result == Result::Fallback || result == Result::Abort)
            {
                return result;
            }
        }
        return handler(terminator);
    }

    static std::wstring EncodeBase64(const std::string_view value)
    {
        static constexpr std::wstring_view alphabet{ L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/" };
        std::wstring result;
        result.reserve((value.size() + 2) / 3 * 4);
        for (size_t offset = 0; offset < value.size(); offset += 3)
        {
            const auto remaining = value.size() - offset;
            const auto a = gsl::narrow_cast<uint8_t>(value.at(offset));
            const auto b = remaining > 1 ? gsl::narrow_cast<uint8_t>(value.at(offset + 1)) : 0;
            const auto c = remaining > 2 ? gsl::narrow_cast<uint8_t>(value.at(offset + 2)) : 0;
            result.push_back(alphabet.at(a >> 2));
            result.push_back(alphabet.at(((a & 0x03) << 4) | (b >> 4)));
            result.push_back(remaining > 1 ? alphabet.at(((b & 0x0f) << 2) | (c >> 6)) : L'=');
            result.push_back(remaining > 2 ? alphabet.at(c & 0x3f) : L'=');
        }
        return result;
    }

    static Iterm2ImageParser MakeParser(std::vector<CapturedTransfer>& transfers)
    {
        return Iterm2ImageParser{ [&](const Iterm2ImageParser::Transfer& transfer) {
            const auto data = transfer.Data();
            transfers.push_back({
                .metadata = transfer.GetMetadata(),
                .data = { data.begin(), data.end() },
                .encodedSize = transfer.EncodedSize(),
                .decodedSize = transfer.DecodedSize(),
            });
        } };
    }

    TEST_METHOD(LegacyTransferParsesMetadata)
    {
        std::vector<CapturedTransfer> transfers;
        auto parser = MakeParser(transfers);

        VERIFY_IS_TRUE(Send(parser, L"File=name=dGVzdC5wbmc=;size=3;width=2;height=3px;preserveAspectRatio=0;inline=1:YWJj", AsciiChars::ESC) == Result::Accept);
        VERIFY_ARE_EQUAL(1u, transfers.size());
        const auto& transfer = transfers.front();
        VERIFY_ARE_EQUAL(L"test.png", transfer.metadata.name);
        VERIFY_IS_TRUE(transfer.metadata.declaredSize == 3);
        VERIFY_IS_TRUE((transfer.metadata.width == Iterm2ImageParser::Dimension{ 2, Iterm2ImageParser::DimensionUnit::Cells }));
        VERIFY_IS_TRUE((transfer.metadata.height == Iterm2ImageParser::Dimension{ 3, Iterm2ImageParser::DimensionUnit::Pixels }));
        VERIFY_IS_FALSE(transfer.metadata.preserveAspectRatio);
        VERIFY_IS_TRUE(transfer.metadata.inlineDisplay);
        VERIFY_ARE_EQUAL(4u, transfer.encodedSize);
        VERIFY_ARE_EQUAL(3u, transfer.decodedSize);
        VERIFY_ARE_EQUAL(std::vector<uint8_t>({ 'a', 'b', 'c' }), transfer.data);
    }

    TEST_METHOD(StateMachineRoutingPreservesOrdinaryOsc)
    {
        auto dispatch = std::make_unique<StreamingDispatch>();
        auto& result = *dispatch;
        auto engine = std::make_unique<OutputStateMachineEngine>(std::move(dispatch));
        StateMachine stateMachine{ std::move(engine) };

        stateMachine.ProcessString(L"\x1b]1337;Set");
        stateMachine.ProcessString(L"Mark\a");
        VERIFY_ARE_EQUAL(1u, result.ordinaryActions.size());
        VERIFY_ARE_EQUAL(L"SetMark", result.ordinaryActions.front());

        stateMachine.ProcessString(L"\x1b]1337;File=inline=1:Y");
        stateMachine.ProcessString(L"WJj\x1b");
        VERIFY_ARE_EQUAL(0u, result.completions);
        stateMachine.ProcessString(L"\\");
        VERIFY_ARE_EQUAL(1u, result.completions);
        VERIFY_ARE_EQUAL(std::vector<uint8_t>({ 'a', 'b', 'c' }), result.data);
        VERIFY_ARE_EQUAL(1u, result.ordinaryActions.size());
    }

    TEST_METHOD(MultipartTransferDecodesAcrossParts)
    {
        std::vector<CapturedTransfer> transfers;
        auto parser = MakeParser(transfers);

        VERIFY_IS_TRUE(Send(parser, L"MultipartFile=size=3;width=100%;height=auto;inline=1;future=value") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FilePart=Y") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FilePart=W") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FilePart=Jj", AsciiChars::ESC) == Result::Accept);
        VERIFY_ARE_EQUAL(0u, transfers.size());
        VERIFY_IS_TRUE(Send(parser, L"FileEnd") == Result::Accept);

        VERIFY_ARE_EQUAL(1u, transfers.size());
        const auto& transfer = transfers.front();
        VERIFY_IS_TRUE((transfer.metadata.width == Iterm2ImageParser::Dimension{ 100, Iterm2ImageParser::DimensionUnit::Percent }));
        VERIFY_IS_TRUE((transfer.metadata.height == Iterm2ImageParser::Dimension{}));
        VERIFY_ARE_EQUAL(std::vector<uint8_t>({ 'a', 'b', 'c' }), transfer.data);
    }

    TEST_METHOD(OrdinaryActionsFallbackAndDownloadsDrain)
    {
        std::vector<CapturedTransfer> transfers;
        auto parser = MakeParser(transfers);

        auto handler = parser.Handler();
        VERIFY_IS_TRUE(handler(L'S') == Result::Fallback);

        VERIFY_IS_TRUE(Send(parser, L"File=:YWJj") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"MultipartFile=size=3") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FilePart=YWJj") == Result::Accept);
        VERIFY_IS_TRUE(parser._multipartTransfer.has_value());
        VERIFY_IS_TRUE(parser._multipartTransfer->decoder._data.empty());
        VERIFY_ARE_EQUAL(3u, parser._multipartTransfer->decoder.DecodedSize());
        VERIFY_IS_TRUE(Send(parser, L"FileEnd") == Result::Accept);
        VERIFY_ARE_EQUAL(0u, transfers.size());
    }

    TEST_METHOD(MalformedMetadataIsRejected)
    {
        static constexpr std::array malformed{
            L"inline=2"sv,
            L"preserveAspectRatio=true"sv,
            L"size=-1"sv,
            L"width=px"sv,
            L"height=1PX"sv,
            L"name=%%%"sv,
            L"name=/w=="sv,
            L"inline=1;inline=1"sv,
            L"width=1;width=2"sv,
            L"size"sv,
        };

        std::vector<CapturedTransfer> transfers;
        auto parser = MakeParser(transfers);
        for (const auto metadata : malformed)
        {
            const auto payload = L"File=" + std::wstring{ metadata } + L":YWJj";
            VERIFY_IS_TRUE(Send(parser, payload) == Result::Accept);
            VERIFY_ARE_EQUAL(0u, transfers.size());
        }

        VERIFY_IS_TRUE(Send(parser, L"File=inline=1;future;future=value:YWJj") == Result::Accept);
        VERIFY_ARE_EQUAL(1u, transfers.size());
    }

    TEST_METHOD(Base64AndDeclaredSizeAreStrict)
    {
        static constexpr std::array malformed{
            L"A==="sv,
            L"YW=J"sv,
            L"YQ="sv,
            L"YQ==A"sv,
            L"YR=="sv,
            L"YW Jj"sv,
            L"YW-J"sv,
        };

        std::vector<CapturedTransfer> transfers;
        auto parser = MakeParser(transfers);
        for (const auto encoded : malformed)
        {
            const auto payload = L"File=inline=1:" + std::wstring{ encoded };
            VERIFY_IS_TRUE(Send(parser, payload) == Result::Accept);
            VERIFY_ARE_EQUAL(0u, transfers.size());
        }

        VERIFY_IS_TRUE(Send(parser, L"File=size=3;inline=1:YWJj") == Result::Accept);
        VERIFY_ARE_EQUAL(1u, transfers.size());
        VERIFY_IS_TRUE(Send(parser, L"File=size=4;inline=1:YWJj") == Result::Accept);
        VERIFY_ARE_EQUAL(1u, transfers.size());
    }

    TEST_METHOD(MultipartFailuresAreAtomic)
    {
        std::vector<CapturedTransfer> transfers;
        auto parser = MakeParser(transfers);

        VERIFY_IS_TRUE(Send(parser, L"FilePart=YWJj") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FileEnd") == Result::Accept);
        VERIFY_ARE_EQUAL(0u, transfers.size());

        VERIFY_IS_TRUE(Send(parser, L"MultipartFile=size=3;inline=1") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FilePart=YW") == Result::Accept);
        VERIFY_ARE_EQUAL(0u, transfers.size());

        VERIFY_IS_TRUE(Send(parser, L"MultipartFile=size=1;inline=1") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FilePart=YQ==") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FileEnd") == Result::Accept);
        VERIFY_ARE_EQUAL(1u, transfers.size());
        VERIFY_ARE_EQUAL(std::vector<uint8_t>({ 'a' }), transfers.front().data);

        VERIFY_IS_TRUE(Send(parser, L"MultipartFile=inline=1") == Result::Accept);
        auto part = parser.Handler();
        for (const auto ch : L"FilePart=YWJj"sv)
        {
            part(ch);
        }
        VERIFY_IS_TRUE(part(AsciiChars::CAN) == Result::Abort);
        VERIFY_IS_TRUE(Send(parser, L"FileEnd") == Result::Accept);
        VERIFY_ARE_EQUAL(1u, transfers.size());

        VERIFY_IS_TRUE(Send(parser, L"MultipartFile=inline=1") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FilePart=YWJj") == Result::Accept);
        parser.Reset();
        VERIFY_IS_TRUE(Send(parser, L"FileEnd") == Result::Accept);
        VERIFY_ARE_EQUAL(1u, transfers.size());
    }

    TEST_METHOD(ResourceBoundsAreEnforced)
    {
        std::vector<CapturedTransfer> transfers;
        auto parser = MakeParser(transfers);

        const auto oversizedMetadata = L"File=" + std::wstring(Iterm2ImageParser::MaxMetadataSize + 1, L'x') + L":YWJj";
        VERIFY_IS_TRUE(Send(parser, oversizedMetadata) == Result::Accept);
        VERIFY_ARE_EQUAL(0u, transfers.size());

        const auto oversizedName = EncodeBase64(std::string(Iterm2ImageParser::MaxNameDecodedSize + 1, 'a'));
        VERIFY_IS_TRUE(Send(parser, L"File=name=" + oversizedName + L";inline=1:YWJj") == Result::Accept);
        VERIFY_ARE_EQUAL(0u, transfers.size());

        VERIFY_IS_TRUE(Send(parser, L"MultipartFile=inline=1") == Result::Accept);
        const auto oversizedPart = L"FilePart=" + std::wstring(Iterm2ImageParser::MaxPartEncodedSize + 1, L'A');
        VERIFY_IS_TRUE(Send(parser, oversizedPart) == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FileEnd") == Result::Accept);
        VERIFY_ARE_EQUAL(0u, transfers.size());

        VERIFY_IS_TRUE(Send(parser, L"MultipartFile=inline=1") == Result::Accept);
        parser._multipartTransfer->decoder._encodedSize = Iterm2ImageParser::MaxTransferEncodedSize;
        VERIFY_IS_TRUE(Send(parser, L"FilePart=A") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FileEnd") == Result::Accept);
        VERIFY_ARE_EQUAL(0u, transfers.size());

        VERIFY_IS_TRUE(Send(parser, L"MultipartFile=inline=1") == Result::Accept);
        parser._multipartTransfer->decoder._decodedSize = Iterm2ImageParser::MaxTransferDecodedSize;
        VERIFY_IS_TRUE(Send(parser, L"FilePart=AAAA") == Result::Accept);
        VERIFY_IS_TRUE(Send(parser, L"FileEnd") == Result::Accept);
        VERIFY_ARE_EQUAL(0u, transfers.size());

        VERIFY_IS_TRUE(Send(parser, L"File=size=33554433;inline=1:YWJj") == Result::Accept);
        VERIFY_ARE_EQUAL(0u, transfers.size());
    }
};
