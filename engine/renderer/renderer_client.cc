// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/renderer/renderer_client.h"

#include "notolibra/engine/renderer/frame_agent.h"

namespace notolibra {

void RendererClient::RenderFrameCreated(content::RenderFrame* render_frame) {
  new FrameAgent(render_frame);  // Deletes itself (OnDestruct).
}

}  // namespace notolibra
