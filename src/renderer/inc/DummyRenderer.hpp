/*++
Copyright (c) Microsoft Corporation
Licensed under the MIT license.

Module Name:
- DummyRenderer.hpp

Abstract:
- Provides a minimal instantiation of the Renderer class.
    This is needed for some tests, where certain objects need a reference to a
    Renderer
--*/

#pragma once
#include "../base/renderer.hpp"

class DummyRenderer final : public Microsoft::Console::Render::Renderer
{
public:
    DummyRenderer(Microsoft::Console::Render::IRenderData* pData = nullptr) :
        Microsoft::Console::Render::Renderer(_renderSettings, pData) {}

    // Paints synchronously, on the calling thread. A test that starts the real render
    // thread is at the mercy of the scheduler -- on a machine busy with I/O the thread
    // can fail to run for tens of seconds -- so a test that only wants to see what the
    // engine was handed should paint itself rather than wait for a frame to arrive.
    using Microsoft::Console::Render::Renderer::PaintFrame;

    Microsoft::Console::Render::RenderSettings _renderSettings;
};
