// Copyright 2026 Notolibra. All rights reserved.

#ifndef NOTOLIBRA_ENGINE_RENDERER_RENDERER_CLIENT_H_
#define NOTOLIBRA_ENGINE_RENDERER_RENDERER_CLIENT_H_

#include "content/public/renderer/content_renderer_client.h"

namespace notolibra {

// The renderer process's embedder: one FrameAgent per frame.
class RendererClient : public content::ContentRendererClient {
 public:
  void RenderFrameCreated(content::RenderFrame* render_frame) override;
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_RENDERER_RENDERER_CLIENT_H_
