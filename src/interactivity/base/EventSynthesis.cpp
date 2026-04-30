// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"
#include "../inc/EventSynthesis.hpp"
#include "../../types/inc/CodepointWidthDetector.hpp"

// TODO: MSFT 14150722 - can these const values be generated at
// runtime without breaking compatibility?
static constexpr WORD altScanCode = 0x38;
static constexpr WORD leftShiftScanCode = 0x2A;

// Routine Description:
// - Determines the width of a single UCS-2 wchar by routing through the
//   canonical CodepointWidthDetector. Previously this carried a hand-rolled
//   Unicode 9.0 fullwidth table that drifted from the renderer's view of
//   width and produced split-brain results for any Emoji_Presentation=Yes
//   codepoint not on the legacy list (e.g. U+231A WATCH). Going through the
//   singleton guarantees the input-event-synthesis path agrees with the
//   rendering / measurement path under whatever TextMeasurementMode the
//   detector is currently configured for. (See AC-W9 in the WIDE/CJK plan.)
#pragma warning(suppress : 4505) // this function will be deleted if numpad events are disabled
static bool IsCharFullWidth(const wchar_t wch) noexcept
{
    GraphemeState state;
    CodepointWidthDetector::Singleton().GraphemeNext(state, std::wstring_view{ &wch, 1 });
    return state.width >= 2;
}

void Microsoft::Console::Interactivity::CharToKeyEvents(const wchar_t wch, const unsigned int codepage, InputEventQueue& keyEvents)
{
    static constexpr short invalidKey = -1;
    auto keyState = OneCoreSafeVkKeyScanW(wch);

    if (keyState == invalidKey)
    {
        if constexpr (Feature_UseNumpadEventsForClipboardInput::IsEnabled())
        {
            // Determine DBCS character because these character does not know by VkKeyScan.
            // GetStringTypeW(CT_CTYPE3) & C3_ALPHA can determine all linguistic characters. However, this is
            // not include symbolic character for DBCS.
            WORD CharType = 0;
            GetStringTypeW(CT_CTYPE3, &wch, 1, &CharType);

            if (WI_IsFlagClear(CharType, C3_ALPHA) && !IsCharFullWidth(wch))
            {
                // It wasn't alphanumeric or determined to be wide by the old algorithm
                // if VkKeyScanW fails (char is not in kbd layout), we must
                // emulate the key being input through the numpad
                SynthesizeNumpadEvents(wch, codepage, keyEvents);
                return;
            }
        }
        keyState = 0; // SynthesizeKeyboardEvents would rather get 0 than -1
    }

    SynthesizeKeyboardEvents(wch, keyState, keyEvents);
}

// Routine Description:
// - converts a wchar_t into a series of KeyEvents as if it was typed
// using the keyboard
// Arguments:
// - wch - the wchar_t to convert
// Return Value:
// - deque of KeyEvents that represent the wchar_t being typed
// Note:
// - will throw exception on error
void Microsoft::Console::Interactivity::SynthesizeKeyboardEvents(const wchar_t wch, const short keyState, InputEventQueue& keyEvents)
{
    const auto vk = LOBYTE(keyState);
    const auto sc = gsl::narrow<WORD>(OneCoreSafeMapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    // The caller provides us with the result of VkKeyScanW() in keyState.
    // The magic constants below are the expected (documented) return values from VkKeyScanW().
    const auto modifierState = HIBYTE(keyState);
    const auto shiftSet = WI_IsFlagSet(modifierState, 1);
    const auto ctrlSet = WI_IsFlagSet(modifierState, 2);
    const auto altSet = WI_IsFlagSet(modifierState, 4);
    const auto altGrSet = WI_AreAllFlagsSet(modifierState, 4 | 2);

    if (altGrSet)
    {
        keyEvents.push_back(SynthesizeKeyEvent(true, 1, VK_MENU, altScanCode, 0, ENHANCED_KEY | LEFT_CTRL_PRESSED | RIGHT_ALT_PRESSED));
    }
    else if (shiftSet)
    {
        keyEvents.push_back(SynthesizeKeyEvent(true, 1, VK_SHIFT, leftShiftScanCode, 0, SHIFT_PRESSED));
    }

    auto keyEvent = SynthesizeKeyEvent(true, 1, vk, sc, wch, 0);
    WI_SetFlagIf(keyEvent.Event.KeyEvent.dwControlKeyState, SHIFT_PRESSED, shiftSet);
    WI_SetFlagIf(keyEvent.Event.KeyEvent.dwControlKeyState, LEFT_CTRL_PRESSED, ctrlSet);
    WI_SetFlagIf(keyEvent.Event.KeyEvent.dwControlKeyState, RIGHT_ALT_PRESSED, altSet);

    keyEvents.push_back(keyEvent);
    keyEvent.Event.KeyEvent.bKeyDown = FALSE;
    keyEvents.push_back(keyEvent);

    // handle yucky alt-gr keys
    if (altGrSet)
    {
        keyEvents.push_back(SynthesizeKeyEvent(false, 1, VK_MENU, altScanCode, 0, ENHANCED_KEY));
    }
    else if (shiftSet)
    {
        keyEvents.push_back(SynthesizeKeyEvent(false, 1, VK_SHIFT, leftShiftScanCode, 0, 0));
    }
}

// Routine Description:
// - converts a wchar_t into a series of KeyEvents as if it was typed
// using Alt + numpad
// Arguments:
// - wch - the wchar_t to convert
// Return Value:
// - deque of KeyEvents that represent the wchar_t being typed using
// alt + numpad
// Note:
// - will throw exception on error
void Microsoft::Console::Interactivity::SynthesizeNumpadEvents(const wchar_t wch, const unsigned int codepage, InputEventQueue& keyEvents)
{
    char converted = 0;
    const auto result = WideCharToMultiByte(codepage, 0, &wch, 1, &converted, 1, nullptr, nullptr);

    // alt keydown
    keyEvents.push_back(SynthesizeKeyEvent(true, 1, VK_MENU, altScanCode, 0, LEFT_ALT_PRESSED));

    if (result == 1)
    {
        // It is OK if the char is "signed -1", we want to interpret that as "unsigned 255" for the
        // "integer to character" conversion below with ::to_string, thus the static_cast.
        // Prime example is nonbreaking space U+00A0 will convert to OEM by codepage 437 to 0xFF which is -1 signed.
        // But it is absolutely valid as 0xFF or 255 unsigned as the correct CP437 character.
        // We need to treat it as unsigned because we're going to pretend it was a keypad entry
        // and you don't enter negative numbers on the keypad.
        const auto charString = std::to_string(static_cast<unsigned char>(converted));

        for (const auto& ch : charString)
        {
            const WORD vk = ch - '0' + VK_NUMPAD0;
            const auto sc = gsl::narrow<WORD>(OneCoreSafeMapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
            auto keyEvent = SynthesizeKeyEvent(true, 1, vk, sc, 0, LEFT_ALT_PRESSED);
            keyEvents.push_back(keyEvent);
            keyEvent.Event.KeyEvent.bKeyDown = FALSE;
            keyEvents.push_back(keyEvent);
        }
    }

    // alt keyup
    keyEvents.push_back(SynthesizeKeyEvent(false, 1, VK_MENU, altScanCode, wch, 0));
}
