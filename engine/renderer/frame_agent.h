// Copyright 2026 The Notolibra Authors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The renderer's side of a frame (frame_agent.mojom): it holds the
// document-start scripts the browser sends at commit and runs them as the
// document's main-world script context is created, before the page's own
// scripts, as the CEF pane's helper did from OnContextCreated. Under the
// sandbox the renderer reads nothing itself; the scripts come with the
// commit.

#ifndef NOTOLIBRA_ENGINE_RENDERER_FRAME_AGENT_H_
#define NOTOLIBRA_ENGINE_RENDERER_FRAME_AGENT_H_

#include <string>

#include "content/public/renderer/render_frame_observer.h"
#include "mojo/public/cpp/bindings/associated_receiver_set.h"
#include "notolibra/engine/frame_agent.mojom.h"

namespace notolibra {

class FrameAgent : public content::RenderFrameObserver,
                   public mojom::FrameAgent {
 public:
  explicit FrameAgent(content::RenderFrame* render_frame);
  FrameAgent(const FrameAgent&) = delete;
  FrameAgent& operator=(const FrameAgent&) = delete;
  ~FrameAgent() override;

  // mojom::FrameAgent:
  void SetDocumentStartScripts(const std::string& script,
                               const std::string& blank_child_script) override;
  void SetErrorPage(const std::string& html) override;

  // The error page the browser sent for `render_frame`'s next error
  // commit, taken once; empty if none.
  static std::string TakeErrorPage(content::RenderFrame* render_frame);

  // content::RenderFrameObserver:
  void DidCreateScriptContext(v8::Local<v8::Context> context,
                              int32_t world_id) override;
  void OnDestruct() override;

 private:
  void Bind(mojo::PendingAssociatedReceiver<mojom::FrameAgent> receiver);
  void Run(const std::string& script);

  // For the next document's main world.
  std::string pending_;
  bool has_pending_ = false;
  // For about:blank children of the current document.
  std::string blank_child_script_;
  // For the next error page.
  std::string error_page_;
  mojo::AssociatedReceiverSet<mojom::FrameAgent> receivers_;
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_RENDERER_FRAME_AGENT_H_
