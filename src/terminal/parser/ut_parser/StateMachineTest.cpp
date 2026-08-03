// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "WexTestClass.h"
#include "../../inc/consoletaeftemplates.hpp"

#include "ascii.hpp"
#include "stateMachine.hpp"

using namespace WEX::Common;
using namespace WEX::Logging;
using namespace WEX::TestExecution;

namespace Microsoft
{
    namespace Console
    {
        namespace VirtualTerminal
        {
            class StateMachineTest;
            class TestStateMachineEngine;
        };
    };
};

using namespace Microsoft::Console::VirtualTerminal;

class Microsoft::Console::VirtualTerminal::TestStateMachineEngine : public IStateMachineEngine
{
public:
    void ResetTestState()
    {
        printed.clear();
        passedThrough.clear();
        executed.clear();
        csiId = 0;
        csiParams.clear();
        dcsId = 0;
        dcsParams.clear();
        dcsDataString.clear();
        apcAccepted = true;
        apcAcceptLimit = SIZE_MAX;
        apcDispatchCount = 0;
        apcId = 0;
        apcDataString.clear();
        oscHandler = nullptr;
        oscHandlerParameter = 0;
        oscDispatchCount = 0;
        oscDispatchParameter = 0;
        oscDispatchedString.clear();
        unknownSequenceCount = 0;
    }

    void UnknownSequence() noexcept override
    {
        unknownSequenceCount++;
    }

    bool EncounteredWin32InputModeSequence() const noexcept override
    {
        return false;
    }

    void ActionReset() noexcept override
    {
    }

    bool ActionExecute(const wchar_t wch) override
    {
        executed += wch;
        return true;
    };

    bool ActionExecuteFromEscape(const wchar_t /* wch */) override { return true; };
    bool ActionPrint(const wchar_t /* wch */) override { return true; };
    bool ActionPrintString(const std::wstring_view string) override
    {
        printed += string;
        return true;
    };

    bool ActionPassThroughString(const std::wstring_view string) override
    {
        passedThrough += string;
        return true;
    };

    bool ActionEscDispatch(const VTID /* id */) override { return true; };

    bool ActionVt52EscDispatch(const VTID /*id*/, const VTParameters /*parameters*/) override { return true; };

    IStateMachineEngine::OscStringHandler ActionOscDispatch(const size_t parameter) override
    {
        oscHandlerParameter = parameter;
        return oscHandler;
    }

    bool ActionOscDispatch(const size_t parameter, const std::wstring_view string) override
    {
        oscDispatchCount++;
        oscDispatchParameter = parameter;
        oscDispatchedString = string;
        if (pfnFlushToTerminal)
        {
            pfnFlushToTerminal();
            return true;
        }
        return true;
    };

    bool ActionSs3Dispatch(const wchar_t /* wch */, const VTParameters /* parameters */) override { return true; };

    // ActionCsiDispatch is the only method that's actually implemented.
    bool ActionCsiDispatch(const VTID id, const VTParameters parameters) override
    {
        // If flush to terminal is registered for a test, then use it.
        if (pfnFlushToTerminal)
        {
            pfnFlushToTerminal();
            return true;
        }
        else
        {
            csiId = id;
            for (size_t i = 0; i < parameters.size(); i++)
            {
                csiParams.push_back(parameters.at(i).value_or(0));
            }
            return true;
        }
    }

    IStateMachineEngine::StringHandler ActionDcsDispatch(const VTID id, const VTParameters parameters) override
    {
        dcsId = id;
        for (size_t i = 0; i < parameters.size(); i++)
        {
            dcsParams.push_back(parameters.at(i).value_or(0));
        }
        dcsDataString.clear();
        return [=](const auto ch) { dcsDataString += ch; return true; };
    }

    IStateMachineEngine::StringHandler ActionApcDispatch(const VTID id) override
    {
        apcDispatchCount++;
        apcId = id;
        apcDataString.clear();
        if (!apcAccepted)
        {
            return nullptr;
        }
        return [=](const auto ch) {
            apcDataString += ch;
            return apcDataString.size() < apcAcceptLimit;
        };
    }

    // These will only be populated if ActionCsiDispatch is called.
    uint64_t csiId = 0;
    std::vector<size_t> csiParams;

    // Flush function for pass-through test.
    std::function<bool()> pfnFlushToTerminal;

    // Passed through string.
    std::wstring passedThrough;

    // Printed string.
    std::wstring printed;

    // Executed string.
    std::wstring executed;

    // These will only be populated if ActionDcsDispatch is called.
    uint64_t dcsId = 0;
    std::vector<size_t> dcsParams;
    std::wstring dcsDataString;

    // Controls how ActionApcDispatch responds, and records what it saw.
    bool apcAccepted = true;
    size_t apcAcceptLimit = SIZE_MAX;
    size_t apcDispatchCount = 0;
    uint64_t apcId = 0;
    std::wstring apcDataString;

    IStateMachineEngine::OscStringHandler oscHandler;
    size_t oscHandlerParameter = 0;
    size_t oscDispatchCount = 0;
    size_t oscDispatchParameter = 0;
    std::wstring oscDispatchedString;

    size_t unknownSequenceCount = 0;
};

class Microsoft::Console::VirtualTerminal::StateMachineTest
{
    TEST_CLASS(StateMachineTest);

    TEST_CLASS_SETUP(ClassSetup)
    {
        return true;
    }

    TEST_CLASS_CLEANUP(ClassCleanup)
    {
        return true;
    }

    TEST_METHOD(TwoStateMachinesDoNotInterfereWithEachOther);

    TEST_METHOD(PassThroughUnhandled);
    TEST_METHOD(RunStorageBeforeEscape);
    TEST_METHOD(BulkTextPrint);
    TEST_METHOD(PassThroughUnhandledSplitAcrossWrites);

    TEST_METHOD(DcsDataStringsReceivedByHandler);

    TEST_METHOD(ApcDataStringsReceivedByHandler);
    TEST_METHOD(ApcIdentifiersAreRoutedToTheEngine);
    TEST_METHOD(ApcHandlerRejectionBehavior);
    TEST_METHOD(ApcEntryRoutingBehavior);
    TEST_METHOD(ApcDataStringSplitAcrossWrites);
    TEST_METHOD(ApcDataStringIsOpaqueToTheParser);

    TEST_METHOD(OscCompletedStringCompatibility);
    TEST_METHOD(OscStreamingHandlerAndFallback);
    TEST_METHOD(OscStreamingCancellation);
    TEST_METHOD(OscFallbackBufferIsBounded);

    TEST_METHOD(VtParameterSubspanTest);
};

void StateMachineTest::TwoStateMachinesDoNotInterfereWithEachOther()
{
    auto firstEnginePtr{ std::make_unique<TestStateMachineEngine>() };
    // this dance is required because StateMachine presumes to take ownership of its engine.
    const auto& firstEngine{ *firstEnginePtr.get() };
    StateMachine firstStateMachine{ std::move(firstEnginePtr) };

    auto secondEnginePtr{ std::make_unique<TestStateMachineEngine>() };
    const auto& secondEngine{ *secondEnginePtr.get() };
    StateMachine secondStateMachine{ std::move(secondEnginePtr) };

    firstStateMachine.ProcessString(L"\x1b[12"); // partial sequence
    secondStateMachine.ProcessString(L"\x1b[3C"); // full sequence on second parser
    firstStateMachine.ProcessString(L";34m"); // completion to previous partial sequence on first parser

    std::vector<size_t> expectedFirstCsi{ 12u, 34u };
    std::vector<size_t> expectedSecondCsi{ 3u };

    VERIFY_ARE_EQUAL(expectedFirstCsi, firstEngine.csiParams);
    VERIFY_ARE_EQUAL(expectedSecondCsi, secondEngine.csiParams);
}

void StateMachineTest::PassThroughUnhandled()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    // this dance is required because StateMachine presumes to take ownership of its engine.
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    // Hook up the passthrough function.
    engine.pfnFlushToTerminal = std::bind(&StateMachine::FlushToTerminal, &machine);

    machine.ProcessString(L"\x1b[?999h 12345 Hello World");

    VERIFY_ARE_EQUAL(String(L"\x1b[?999h"), String(engine.passedThrough.c_str()));
    VERIFY_ARE_EQUAL(String(L" 12345 Hello World"), String(engine.printed.c_str()));
}

void StateMachineTest::RunStorageBeforeEscape()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    // this dance is required because StateMachine presumes to take ownership of its engine.
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    // Hook up the passthrough function.
    engine.pfnFlushToTerminal = std::bind(&StateMachine::FlushToTerminal, &machine);

    // Print a bunch of regular text to build up the run buffer before transitioning state.
    machine.ProcessString(L"12345 Hello World\x1b[?999h");

    // Then ensure the entire buffered run was printed all at once back to us.
    VERIFY_ARE_EQUAL(String(L"12345 Hello World"), String(engine.printed.c_str()));
    VERIFY_ARE_EQUAL(String(L"\x1b[?999h"), String(engine.passedThrough.c_str()));
}

void StateMachineTest::BulkTextPrint()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    // this dance is required because StateMachine presumes to take ownership of its engine.
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    // Print a bunch of regular text to build up the run buffer before transitioning state.
    machine.ProcessString(L"12345 Hello World");

    // Then ensure the entire buffered run was printed all at once back to us.
    VERIFY_ARE_EQUAL(String(L"12345 Hello World"), String(engine.printed.c_str()));
}

void StateMachineTest::PassThroughUnhandledSplitAcrossWrites()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    // this dance is required because StateMachine presumes to take ownership of its engine.
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    // Hook up the passthrough function.
    engine.pfnFlushToTerminal = std::bind(&StateMachine::FlushToTerminal, &machine);

    // Broken in two pieces (test case from GH#3081)
    machine.ProcessString(L"\x1b[?12");
    VERIFY_ARE_EQUAL(L"", engine.passedThrough); // nothing out yet
    VERIFY_ARE_EQUAL(L"", engine.printed);

    machine.ProcessString(L"34h");
    VERIFY_ARE_EQUAL(L"\x1b[?1234h", engine.passedThrough); // whole sequence out, no other output
    VERIFY_ARE_EQUAL(L"", engine.printed);

    engine.ResetTestState();

    // Three pieces
    machine.ProcessString(L"\x1b[?2");
    VERIFY_ARE_EQUAL(L"", engine.passedThrough); // nothing out yet
    VERIFY_ARE_EQUAL(L"", engine.printed);

    machine.ProcessString(L"34");
    VERIFY_ARE_EQUAL(L"", engine.passedThrough); // nothing out yet
    VERIFY_ARE_EQUAL(L"", engine.printed);

    machine.ProcessString(L"5h");
    VERIFY_ARE_EQUAL(L"\x1b[?2345h", engine.passedThrough); // whole sequence out, no other output
    VERIFY_ARE_EQUAL(L"", engine.printed);

    engine.ResetTestState();

    // Split during OSC terminator (test case from GH#3080)
    machine.ProcessString(L"\x1b]99;foo\x1b");
    VERIFY_ARE_EQUAL(L"", engine.passedThrough); // nothing out yet
    VERIFY_ARE_EQUAL(L"", engine.printed);

    machine.ProcessString(L"\\");
    VERIFY_ARE_EQUAL(L"\x1b]99;foo\x1b\\", engine.passedThrough);
    VERIFY_ARE_EQUAL(L"", engine.printed);
}

void StateMachineTest::DcsDataStringsReceivedByHandler()
{
    BEGIN_TEST_METHOD_PROPERTIES()
        TEST_METHOD_PROPERTY(L"Data:terminatorType", L"{ 0, 1, 2, 3 }")
    END_TEST_METHOD_PROPERTIES()

    size_t terminatorType;
    VERIFY_SUCCEEDED(TestData::TryGetValue(L"terminatorType", terminatorType));

    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    // this dance is required because StateMachine presumes to take ownership of its engine.
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    uint64_t expectedCsiId = 0;
    std::wstring expectedExecuted = L"";

    std::wstring terminatorString;
    switch (terminatorType)
    {
    case 0:
        Log::Comment(L"Data string terminated with ST");
        terminatorString = L"\033\\";
        break;
    case 1:
        Log::Comment(L"Data string terminated with CSI sequence");
        terminatorString = L"\033[m";
        expectedCsiId = VTID(L'm');
        break;
    case 2:
        Log::Comment(L"Data string terminated with CAN");
        terminatorString = L"\030";
        expectedExecuted = L"\030";
        break;
    case 3:
        Log::Comment(L"Data string terminated with SUB");
        terminatorString = L"\032";
        expectedExecuted = L"\032";
        break;
    }

    // Output a DCS sequence terminated with the current test string
    machine.ProcessString(L"\033P1;2;3|data string");
    machine.ProcessString(terminatorString);
    machine.ProcessString(L"printed text");

    // Verify the sequence ID and parameters are received.
    VERIFY_ARE_EQUAL(VTID("|"), engine.dcsId);
    VERIFY_ARE_EQUAL(std::vector<size_t>({ 1, 2, 3 }), engine.dcsParams);

    // Verify that the data string is received (ESC terminated).
    VERIFY_ARE_EQUAL(L"data string\033", engine.dcsDataString);

    // Verify the characters following the sequence are printed.
    VERIFY_ARE_EQUAL(L"printed text", engine.printed);

    // Verify the CSI sequence was received (if expected).
    VERIFY_ARE_EQUAL(expectedCsiId, engine.csiId);

    // Verify the control characters were executed (if expected).
    VERIFY_ARE_EQUAL(expectedExecuted, engine.executed);
}

void StateMachineTest::ApcDataStringsReceivedByHandler()
{
    BEGIN_TEST_METHOD_PROPERTIES()
        TEST_METHOD_PROPERTY(L"Data:terminatorType", L"{ 0, 1, 2, 3 }")
    END_TEST_METHOD_PROPERTIES()

    size_t terminatorType;
    VERIFY_SUCCEEDED(TestData::TryGetValue(L"terminatorType", terminatorType));

    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    // this dance is required because StateMachine presumes to take ownership of its engine.
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    uint64_t expectedCsiId = 0;
    std::wstring expectedExecuted = L"";
    // Unlike DCS, an APC handler is told how the string ended, so it can
    // distinguish "commit what you have" from "throw it away".
    std::wstring expectedFinalCharacter = L"\033";

    std::wstring terminatorString;
    switch (terminatorType)
    {
    case 0:
        Log::Comment(L"Data string terminated with ST");
        terminatorString = L"\033\\";
        break;
    case 1:
        Log::Comment(L"Data string terminated with CSI sequence");
        terminatorString = L"\033[m";
        expectedCsiId = VTID(L'm');
        break;
    case 2:
        Log::Comment(L"Data string cancelled with CAN");
        terminatorString = L"\030";
        expectedExecuted = L"\030";
        expectedFinalCharacter = L"\030";
        break;
    case 3:
        Log::Comment(L"Data string cancelled with SUB");
        terminatorString = L"\032";
        expectedExecuted = L"\032";
        expectedFinalCharacter = L"\030";
        break;
    }

    // Output an APC sequence terminated with the current test string
    machine.ProcessString(L"\033_Gdata string");
    machine.ProcessString(terminatorString);
    machine.ProcessString(L"printed text");

    // Verify the application identifier is received.
    VERIFY_ARE_EQUAL(VTID("G"), engine.apcId);

    // Verify that the data string is received, along with the character that
    // ended it. Note the identifier is not repeated in the data.
    VERIFY_ARE_EQUAL(L"data string" + expectedFinalCharacter, engine.apcDataString);

    // Verify the characters following the sequence are printed.
    VERIFY_ARE_EQUAL(L"printed text", engine.printed);

    // Verify the CSI sequence was received (if expected).
    VERIFY_ARE_EQUAL(expectedCsiId, engine.csiId);

    // Verify the control characters were executed (if expected).
    VERIFY_ARE_EQUAL(expectedExecuted, engine.executed);
}

void StateMachineTest::OscCompletedStringCompatibility()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    machine.ProcessString(L"\x1b]42;ordinary payload\a");
    VERIFY_ARE_EQUAL(1u, engine.oscDispatchCount);
    VERIFY_ARE_EQUAL(42u, engine.oscDispatchParameter);
    VERIFY_ARE_EQUAL(L"ordinary payload", engine.oscDispatchedString);

    engine.ResetTestState();
    machine.ProcessString(L"\x1b]1337;SetMark\x1b");
    VERIFY_ARE_EQUAL(0u, engine.oscDispatchCount);
    machine.ProcessString(L"\\");
    VERIFY_ARE_EQUAL(1u, engine.oscDispatchCount);
    VERIFY_ARE_EQUAL(1337u, engine.oscDispatchParameter);
    VERIFY_ARE_EQUAL(L"SetMark", engine.oscDispatchedString);
}

void StateMachineTest::OscStreamingHandlerAndFallback()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    std::wstring streamed;
    engine.oscHandler = [&](const wchar_t ch) {
        streamed += ch;
        return streamed.size() >= 5 ?
                   IStateMachineEngine::OscStringHandlerResult::Accept :
                   IStateMachineEngine::OscStringHandlerResult::Pending;
    };

    machine.ProcessString(L"\x1b]1337;Fi");
    VERIFY_ARE_EQUAL(1337u, engine.oscHandlerParameter);
    VERIFY_ARE_EQUAL(L"Fi", machine._oscString);
    VERIFY_IS_TRUE(machine._cachedSequence.has_value());

    machine.ProcessString(L"le=payload");
    VERIFY_IS_TRUE(machine._oscString.empty());
    VERIFY_IS_FALSE(machine._cachedSequence.has_value());

    machine.ProcessString(L"\x1b\\");
    VERIFY_ARE_EQUAL(L"File=payload\x1b", streamed);
    VERIFY_ARE_EQUAL(0u, engine.oscDispatchCount);

    engine.ResetTestState();
    std::wstring probed;
    engine.oscHandler = [&](const wchar_t ch) {
        probed += ch;
        return probed.size() == 3 ?
                   IStateMachineEngine::OscStringHandlerResult::Fallback :
                   IStateMachineEngine::OscStringHandlerResult::Pending;
    };

    machine.ProcessString(L"\x1b]1337;Set");
    machine.ProcessString(L"Mark\a");
    VERIFY_ARE_EQUAL(L"Set", probed);
    VERIFY_ARE_EQUAL(1u, engine.oscDispatchCount);
    VERIFY_ARE_EQUAL(L"SetMark", engine.oscDispatchedString);
}

void StateMachineTest::OscStreamingCancellation()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    std::wstring streamed;
    engine.oscHandler = [&](const wchar_t ch) {
        streamed += ch;
        return IStateMachineEngine::OscStringHandlerResult::Accept;
    };

    machine.ProcessString(L"\x1b]1337;x\x18");
    VERIFY_ARE_EQUAL(L"x\x18", streamed);
    VERIFY_ARE_EQUAL(0u, engine.oscDispatchCount);

    streamed.clear();
    machine.ProcessString(L"\x1b]1337;x\x1b[31m");
    VERIFY_ARE_EQUAL(L"x\x18", streamed);
    VERIFY_ARE_EQUAL(0u, engine.oscDispatchCount);

    streamed.clear();
    machine.ProcessString(L"\x1b]1337;x");
    machine.ResetState();
    VERIFY_ARE_EQUAL(L"x\x18", streamed);
    VERIFY_ARE_EQUAL(0u, engine.oscDispatchCount);
}

void StateMachineTest::OscFallbackBufferIsBounded()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    std::wstring streamed;
    engine.oscHandler = [&](const wchar_t ch) {
        streamed += ch;
        return IStateMachineEngine::OscStringHandlerResult::Pending;
    };

    machine.ProcessString(L"\x1b]1337;");
    machine._oscString.assign(StateMachine::MaxOscStringLength, L'x');
    machine._cachedSequence.emplace(L"bounded prefix");
    machine.ProcessCharacter(L'x');

    VERIFY_IS_TRUE(machine._oscStringDiscarded);
    VERIFY_IS_TRUE(machine._oscString.empty());
    VERIFY_IS_FALSE(machine._cachedSequence.has_value());
    VERIFY_ARE_EQUAL(std::wstring(1, AsciiChars::CAN), streamed);

    machine.ProcessCharacter(AsciiChars::BEL);
    VERIFY_ARE_EQUAL(0u, engine.oscDispatchCount);

    engine.ResetTestState();
    machine.ProcessString(L"\x1b]42;ok\a");
    VERIFY_ARE_EQUAL(1u, engine.oscDispatchCount);
    VERIFY_ARE_EQUAL(L"ok", engine.oscDispatchedString);
}

void StateMachineTest::ApcIdentifiersAreRoutedToTheEngine()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    // The character after the APC introducer names the application the string
    // is addressed to, in the same way a DCS is identified by its final byte.
    // Two different applications must not be confused for each other.
    machine.ProcessString(L"\033_Gfirst\033\\");
    VERIFY_ARE_EQUAL(VTID("G"), engine.apcId);
    VERIFY_ARE_EQUAL(L"first\033", engine.apcDataString);

    machine.ProcessString(L"\033_Qsecond\033\\");
    VERIFY_ARE_EQUAL(VTID("Q"), engine.apcId);
    VERIFY_ARE_EQUAL(L"second\033", engine.apcDataString);

    VERIFY_ARE_EQUAL(size_t{ 2 }, engine.apcDispatchCount);
    VERIFY_ARE_EQUAL(L"", engine.printed);
}

void StateMachineTest::ApcHandlerRejectionBehavior()
{
    // A handler can bail out of a string in two ways, and they report different
    // things back to the app. Declining outright leaves the sequence unclaimed -
    // an unknown sequence, just as before APC was dispatched at all. Claiming and
    // then stopping early keeps ownership, so nothing is reported.
    {
        auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
        auto& engine{ *enginePtr.get() };
        StateMachine machine{ std::move(enginePtr) };

        // An engine that doesn't recognise the application returns no handler. The
        // string then behaves exactly as it did before APC was dispatched at all:
        // swallowed up to its terminator, with nothing printed.
        engine.apcAccepted = false;
        machine.ProcessString(L"\033_Zdata string\033\\printed text");

        VERIFY_ARE_EQUAL(VTID("Z"), engine.apcId);
        VERIFY_ARE_EQUAL(size_t{ 1 }, engine.apcDispatchCount);
        VERIFY_ARE_EQUAL(L"", engine.apcDataString);
        VERIFY_ARE_EQUAL(L"printed text", engine.printed);
        // Nobody claimed it, so it really was an unknown sequence - the same thing
        // that was reported before APC strings were dispatched at all.
        VERIFY_ARE_EQUAL(size_t{ 1 }, engine.unknownSequenceCount);

        // And the parser is genuinely back in a good state afterwards.
        engine.apcAccepted = true;
        machine.ProcessString(L"\033_Gagain\033\\");
        VERIFY_ARE_EQUAL(VTID("G"), engine.apcId);
        VERIFY_ARE_EQUAL(L"again\033", engine.apcDataString);
    }

    {
        auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
        auto& engine{ *enginePtr.get() };
        StateMachine machine{ std::move(enginePtr) };

        // A handler that returns false has given up on the rest of the string. It
        // must not be called again, and the remainder must not reach the screen.
        engine.apcAcceptLimit = 4;
        machine.ProcessString(L"\033_Gdata string\033\\printed text");

        VERIFY_ARE_EQUAL(L"data", engine.apcDataString);
        VERIFY_ARE_EQUAL(L"printed text", engine.printed);
        // The string was recognised and claimed, so it is not an unknown sequence.
        // Reporting one would tell conhost its cursor position may be wrong and
        // force a needless ConPTY resync.
        VERIFY_ARE_EQUAL(size_t{ 0 }, engine.unknownSequenceCount);
    }
}

void StateMachineTest::ApcDataStringSplitAcrossWrites()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    // A payload can be arbitrarily long, so it arrives across several writes.
    // Each one must reach the handler as it is received rather than being
    // buffered up in the state machine waiting for a terminator.
    machine.ProcessString(L"\033_Gdata");
    VERIFY_ARE_EQUAL(VTID("G"), engine.apcId);
    VERIFY_ARE_EQUAL(L"data", engine.apcDataString);

    machine.ProcessString(L" and more");
    VERIFY_ARE_EQUAL(L"data and more", engine.apcDataString);

    machine.ProcessString(L"\033\\printed text");
    VERIFY_ARE_EQUAL(L"data and more\033", engine.apcDataString);

    VERIFY_ARE_EQUAL(size_t{ 1 }, engine.apcDispatchCount);
    VERIFY_ARE_EQUAL(L"printed text", engine.printed);
}

void StateMachineTest::ApcDataStringIsOpaqueToTheParser()
{
    auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
    auto& engine{ *enginePtr.get() };
    StateMachine machine{ std::move(enginePtr) };

    // An APC string is application data in an encoding only its handler knows.
    // The parser must not filter it down to what looks like text: a handler that
    // is silently handed a shortened string cannot tell a malformed payload from
    // one that was never sent, and will report the wrong thing back to the app.
    machine.ProcessString(L"\033_Gok\u00FF\u2603\uFFFD\x7Fdone\033\\");
    VERIFY_ARE_EQUAL(VTID("G"), engine.apcId);
    VERIFY_ARE_EQUAL(L"ok\u00FF\u2603\uFFFD\x7Fdone\033", engine.apcDataString);
    VERIFY_ARE_EQUAL(size_t{ 1 }, engine.apcDispatchCount);
}

void StateMachineTest::ApcEntryRoutingBehavior()
{
    // ApcEntry only routes once it has an identifier byte to route on. With none
    // at all the string is dropped; a control character can't name an application
    // either, so it's skipped while we wait for one that can - the same thing
    // DcsEntry does with them.
    {
        auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
        auto& engine{ *enginePtr.get() };
        StateMachine machine{ std::move(enginePtr) };

        // There is nothing to route on, so no engine is asked, and the string is
        // dropped when its terminator arrives.
        machine.ProcessString(L"\033_\033\\printed text");

        VERIFY_ARE_EQUAL(size_t{ 0 }, engine.apcDispatchCount);
        VERIFY_ARE_EQUAL(L"printed text", engine.printed);
    }

    {
        auto enginePtr{ std::make_unique<TestStateMachineEngine>() };
        auto& engine{ *enginePtr.get() };
        StateMachine machine{ std::move(enginePtr) };

        // The control characters are skipped while we wait for an identifier that
        // can name an application, then routing proceeds as normal.
        machine.ProcessString(L"\033_\a\177Gdata string\033\\");

        VERIFY_ARE_EQUAL(VTID("G"), engine.apcId);
        VERIFY_ARE_EQUAL(size_t{ 1 }, engine.apcDispatchCount);
        VERIFY_ARE_EQUAL(L"data string\033", engine.apcDataString);
        VERIFY_ARE_EQUAL(L"", engine.printed);
        VERIFY_ARE_EQUAL(L"", engine.executed);
    }
}

void StateMachineTest::VtParameterSubspanTest()
{
    const auto parameterList = std::vector<VTParameter>{ 12, 34, 56, 78 };
    const auto parameterSpan = VTParameters{ parameterList.data(), parameterList.size() };

    {
        Log::Comment(L"Subspan from 0 gives all the parameters");
        const auto subspan = parameterSpan.subspan(0);
        VERIFY_ARE_EQUAL(4u, subspan.size());
        VERIFY_ARE_EQUAL(12, subspan.at(0));
        VERIFY_ARE_EQUAL(34, subspan.at(1));
        VERIFY_ARE_EQUAL(56, subspan.at(2));
        VERIFY_ARE_EQUAL(78, subspan.at(3));
    }
    {
        Log::Comment(L"Subspan from 2 gives the last 2 parameters");
        const auto subspan = parameterSpan.subspan(2);
        VERIFY_ARE_EQUAL(2u, subspan.size());
        VERIFY_ARE_EQUAL(56, subspan.at(0));
        VERIFY_ARE_EQUAL(78, subspan.at(1));
    }
    {
        Log::Comment(L"Subspan at the end of the range gives 1 omitted value");
        const auto subspan = parameterSpan.subspan(4);
        VERIFY_ARE_EQUAL(1u, subspan.size());
        VERIFY_IS_FALSE(subspan.at(0).has_value());
    }
    {
        Log::Comment(L"Subspan past the end of the range gives 1 omitted value");
        const auto subspan = parameterSpan.subspan(6);
        VERIFY_ARE_EQUAL(1u, subspan.size());
        VERIFY_IS_FALSE(subspan.at(0).has_value());
    }
}
