// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/renderer/frame_agent.h"

#include <map>
#include <string_view>

#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_registry.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_script_source.h"
#include "url/gurl.h"

namespace notolibra {

namespace {

// Every frame's agent, so a child frame can find its parent's.
std::map<content::RenderFrame*, FrameAgent*>& Agents() {
  static base::NoDestructor<std::map<content::RenderFrame*, FrameAgent*>>
      agents;
  return *agents;
}

}  // namespace

FrameAgent::FrameAgent(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {
  Agents()[render_frame] = this;
  render_frame->GetAssociatedInterfaceRegistry()
      ->AddInterface<mojom::FrameAgent>(base::BindRepeating(
          &FrameAgent::Bind, base::Unretained(this)));
}

FrameAgent::~FrameAgent() = default;

void FrameAgent::Bind(
    mojo::PendingAssociatedReceiver<mojom::FrameAgent> receiver) {
  receivers_.Add(this, std::move(receiver));
}

void FrameAgent::SetDocumentStartScripts(const std::string& script,
                                         const std::string& blank_child_script) {
  pending_ = script;
  has_pending_ = true;
  blank_child_script_ = blank_child_script;
}

void FrameAgent::SetErrorPage(const std::string& html) {
  error_page_ = html;
}

// static
std::string FrameAgent::TakeErrorPage(content::RenderFrame* render_frame) {
  auto it = Agents().find(render_frame);
  if (it == Agents().end()) {
    return std::string();
  }
  std::string html;
  html.swap(it->second->error_page_);
  return html;
}

void FrameAgent::DidCreateScriptContext(v8::Local<v8::Context> context,
                                        int32_t world_id) {
  if (world_id != 0) {
    return;
  }
  if (has_pending_) {
    has_pending_ = false;
    std::string script;
    script.swap(pending_);
    Run(script);
    return;
  }
  // A frame its parent's document made without navigating (about:blank,
  // or the initial empty document): its parent's script for such frames.
  blink::WebLocalFrame* frame = render_frame()->GetWebFrame();
  GURL url = frame->GetDocument().Url();
  if (!url.is_empty() && !url.IsAboutBlank()) {
    return;
  }
  blink::WebFrame* parent = frame->Parent();
  if (!parent || !parent->IsWebLocalFrame()) {
    return;
  }
  content::RenderFrame* parent_frame =
      content::RenderFrame::FromWebFrame(parent->ToWebLocalFrame());
  auto it = Agents().find(parent_frame);
  if (it != Agents().end() && !it->second->blank_child_script_.empty()) {
    Run(it->second->blank_child_script_);
  }
}

void FrameAgent::Run(const std::string& script) {
  // Parts the app separated (ne_document_start_fn) run as scripts of their
  // own, in order: an exception in one doesn't stop the next, and their
  // globals are shared, as between a page's <script> elements.
  static constexpr std::string_view kSeparator = "\n//\x1e\n";
  std::string_view rest(script);
  while (!rest.empty()) {
    const size_t end = rest.find(kSeparator);
    const std::string_view part = rest.substr(0, end);
    if (!part.empty()) {
      render_frame()->GetWebFrame()->ExecuteScript(blink::WebScriptSource(
          blink::WebString::FromUtf8(part)));
    }
    if (end == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(end + kSeparator.size());
  }
}

void FrameAgent::OnDestruct() {
  Agents().erase(render_frame());
  delete this;
}

}  // namespace notolibra
