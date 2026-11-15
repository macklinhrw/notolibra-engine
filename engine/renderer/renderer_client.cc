// Copyright 2026 The Notolibra Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notolibra/engine/renderer/renderer_client.h"

#include "content/public/common/isolated_world_ids.h"
#include "notolibra/engine/renderer/frame_agent.h"
#include "third_party/blink/public/platform/web_isolated_world_info.h"
#include "third_party/blink/public/platform/web_security_origin.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url.h"
#include "url/gurl.h"

namespace notolibra {

void RendererClient::RenderThreadStarted() {
  // The app's world (NE_WORLD_APP) gets a policy of its own, permissive, as
  // Chrome gives extensions' content scripts one: without it it inherits the
  // page's, so a page enforcing Trusted Types (Gmail, YouTube) made the
  // app's scripts throw on their first innerHTML. A policy needs an origin;
  // an opaque one grants nothing: fetches and XHRs from the world still go
  // out as the page's origin, as before.
  blink::WebIsolatedWorldInfo info;
  info.security_origin =
      blink::WebSecurityOrigin::Create(blink::WebURL(GURL("data:,")));
  // Non-empty: an empty WebString is null, which clears the policy.
  info.content_security_policy = blink::WebString::FromUtf8(
      "default-src * data: blob: 'unsafe-inline' 'unsafe-eval'");
  info.human_readable_name = blink::WebString::FromUtf8("Notolibra");
  blink::SetIsolatedWorldInfo(content::ISOLATED_WORLD_ID_CONTENT_END, info);
}

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
