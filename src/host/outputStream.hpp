/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- outputStream.hpp

Abstract:
- Classes to process text written into the console on the attached application's output stream (usually STDOUT).

Author:
- Michael Niksa <miniksa> July 27 2015
--*/

#pragma once

#include "../terminal/adapter/ITerminalApi.hpp"
#include "../types/inc/IInputEvent.hpp"
#include "../inc/conattrs.hpp"
#include "IIoProvider.hpp"
#include "../renderer/inc/IRenderData.hpp"

// The ConhostInternalGetSet is for the Conhost process to call the entrypoints for its own Get/Set APIs.
// Normally, these APIs are accessible from the outside of the conhost process (like by the process being "hosted") through
// the kernelbase/32 exposed public APIs and routed by the console driver (condrv) to this console host.
// But since we're trying to call them from *inside* the console host itself, we need to get in the way and route them straight to the
// v-table inside this process instance.
class ConhostInternalGetSet final : public Microsoft::Console::VirtualTerminal::ITerminalApi
{
public:
    ConhostInternalGetSet(_In_ Microsoft::Console::IIoProvider& io);

    void UnknownSequence() noexcept override;
    void ReturnResponse(const std::wstring_view response) override;

    bool IsConPTY() const noexcept override;
    Microsoft::Console::VirtualTerminal::StateMachine& GetStateMachine() override;
    BufferState GetBufferAndViewport() override;
    void SetViewportPosition(const til::point position) override;
    void SetViewportPositionInternal(const til::point position) override;
    void RestoreViewportPositionInternal(const til::point position) override;

    void SetSystemMode(const Mode mode, const bool enabled) override;
    bool GetSystemMode(const Mode mode) const override;

    void ReturnAnswerback() override;
    void WarningBell() override;

    void SetWindowTitle(const std::wstring_view title) override;

    void UseAlternateScreenBuffer(const TextAttribute& attrs) override;

    void UseMainScreenBuffer() override;

    CursorType GetUserDefaultCursorStyle() const override;

    void ShowWindow(bool showOrHide) override;

    bool ResizeWindow(const til::CoordType width, const til::CoordType height) override;

    void SetCodePage(const unsigned int codepage) override;
    void ResetCodePage() override;
    unsigned int GetOutputCodePage() const override;
    unsigned int GetInputCodePage() const override;

    void CopyToClipboard(const wil::zwstring_view content) override;
    void SetTaskbarProgress(const ::Microsoft::Console::VirtualTerminal::DispatchTypes::TaskbarState state, const size_t progress) override;
    void SetWorkingDirectory(const std::wstring_view uri) override;
    void PlayMidiNote(const int noteNumber, const int velocity, const std::chrono::microseconds duration) override;

    bool IsVtInputEnabled() const override;

    void NotifyBufferRotation(const int delta) override;
    void NotifyShellIntegrationMark() override;

    void InvokeCompletions(std::wstring_view menuJson, unsigned int replaceLength) override;

    void SearchMissingCommand(std::wstring_view missingCommand) override;

    void ShowNotification(std::wstring_view title, std::wstring_view body) override;

    bool DecodeImageToBgra(const std::span<const uint8_t> data, std::vector<RGBQUAD>& pixels, til::size& size) noexcept override;
    til::size GetCellSize() const noexcept override;
    void SetTimedContentHandler(std::function<void()> handler) override;
    void RequestTimedContentUpdate(const std::optional<std::chrono::steady_clock::time_point> deadline) override;

private:
    Microsoft::Console::IIoProvider& _io;

    // Timed content state. The handler runs on the render thread, so the mutex
    // guards it against a concurrent SetTimedContentHandler on the writing thread.
    std::mutex _timedContentMutex;
    std::function<void()> _timedContentHandler;
    Microsoft::Console::Render::TimerHandle _timedContentTimer{};
    std::shared_ptr<int> _timedContentLifetime = std::make_shared<int>(0);
};
