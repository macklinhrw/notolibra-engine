// Copyright 2026 The Notolibra Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notolibra/engine/renderer/renderer_client.h"

#include "notolibra/engine/renderer/frame_agent.h"

namespace notolibra {

void RendererClient::RenderFrameCreated(content::RenderFrame* render_frame) {
  new FrameAgent(render_frame);  // Deletes itself (OnDestruct).
}

void RendererClient::PrepareErrorPage(
    content::RenderFrame* render_frame,
    const blink::WebURLError& error,
    const std::string& http_method,
    content::mojom::AlternativeErrorPageOverrideInfoPtr
        alternative_error_page_info,
    std::string* error_html) {
  std::string html = FrameAgent::TakeErrorPage(render_frame);
  if (error_html && !html.empty()) {
    *error_html = std::move(html);
  }
}

void RendererClient::PrepareErrorPageForHttpStatusError(
    content::RenderFrame* render_frame,
    const blink::WebURLError& error,
    const std::string& http_method,
    int http_status,
    content::mojom::AlternativeErrorPageOverrideInfoPtr
        alternative_error_page_info,
    std::string* error_html) {
  PrepareErrorPage(render_frame, error, http_method,
                   std::move(alternative_error_page_info), error_html);
}

}  // namespace notolibra
