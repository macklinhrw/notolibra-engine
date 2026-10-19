// Copyright 2026 Notolibra. All rights reserved.

#ifndef NOTOLIBRA_ENGINE_RENDERER_RENDERER_CLIENT_H_
#define NOTOLIBRA_ENGINE_RENDERER_RENDERER_CLIENT_H_

#include "content/public/renderer/content_renderer_client.h"

namespace notolibra {

// The renderer process's embedder: one FrameAgent per frame, and error
// pages from the browser (FrameAgent::SetErrorPage) rather than content's
// empty white one.
class RendererClient : public content::ContentRendererClient {
 public:
  void RenderFrameCreated(content::RenderFrame* render_frame) override;
  void PrepareErrorPage(
      content::RenderFrame* render_frame,
      const blink::WebURLError& error,
      const std::string& http_method,
      content::mojom::AlternativeErrorPageOverrideInfoPtr
          alternative_error_page_info,
      std::string* error_html) override;
  void PrepareErrorPageForHttpStatusError(
      content::RenderFrame* render_frame,
      const blink::WebURLError& error,
      const std::string& http_method,
      int http_status,
      content::mojom::AlternativeErrorPageOverrideInfoPtr
          alternative_error_page_info,
      std::string* error_html) override;
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_RENDERER_RENDERER_CLIENT_H_
