// Copyright 2026 The Notolibra Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notolibra/engine/tab.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

#include "base/apple/owned_objc.h"
#include "base/json/json_writer.h"
#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/strings/escape.h"
#include "base/strings/stringprintf.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/back_forward_cache.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"
#include "media/capture/mojom/video_capture_buffer.mojom.h"
#include "media/capture/mojom/video_capture_types.mojom.h"
#include "content/browser/renderer_host/render_widget_host_view_mac.h"
#include "notolibra/engine/engine.h"
#include "notolibra/engine/frame_agent.mojom.h"
#include "notolibra/engine/languages.h"
#include "notolibra/engine/pdf.h"
#include "notolibra/engine/view_bridge.h"
#include "third_party/blink/public/common/renderer_preferences/renderer_preferences.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "net/base/net_errors.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"
#include "third_party/blink/public/common/input/web_gesture_event.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/public/mojom/frame/find_in_page.mojom.h"
#include "ui/base/cocoa/remote_layer_api.h"
#include "ui/base/page_transition_types.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/color_utils.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size_conversions.h"
#include "ui/latency/latency_info.h"

namespace notolibra {

namespace {

// A script's answer for `done`, given exactly once: its result as JSON, or
// NE_ERR_GONE when Chromium drops the callback without running it (the frame
// or its renderer went away first, or the page was replaced before it ran).
// Without this, a dropped callback left the app waiting until its timeout.
class EvalReply {
 public:
  EvalReply(ne_eval_fn done, void* user_data)
      : done_(done), user_data_(user_data) {}
  EvalReply(const EvalReply&) = delete;
  EvalReply& operator=(const EvalReply&) = delete;
  ~EvalReply() {
    if (done_) {
      std::exchange(done_, nullptr)(std::exchange(user_data_, nullptr),
                                    NE_ERR_GONE, nullptr);
    }
  }

  void Answer(base::Value value) {
    std::string json;
    base::JSONWriter::Write(value, &json);
    std::exchange(done_, nullptr)(std::exchange(user_data_, nullptr), NE_OK,
                                  json.c_str());
  }

  // The callback Chromium runs with the result, or drops.
  static content::RenderFrameHost::JavaScriptResultCallback Callback(
      ne_eval_fn done,
      void* user_data) {
    if (!done) {
      return {};
    }
    return base::BindOnce(
        [](std::unique_ptr<EvalReply> reply, base::Value value) {
          reply->Answer(std::move(value));
        },
        std::make_unique<EvalReply>(done, user_data));
  }

 private:
  ne_eval_fn done_;
  raw_ptr<void> user_data_;
};

int Modifiers(uint32_t ne_modifiers, int button_flags = 0) {
  int modifiers = button_flags;
  if (ne_modifiers & 1) modifiers |= blink::WebInputEvent::kShiftKey;
  if (ne_modifiers & 2) modifiers |= blink::WebInputEvent::kControlKey;
  if (ne_modifiers & 4) modifiers |= blink::WebInputEvent::kAltKey;
  if (ne_modifiers & 8) modifiers |= blink::WebInputEvent::kMetaKey;
  return modifiers;
}

NSEventModifierFlags NSModifiers(uint32_t ne_modifiers) {
  NSEventModifierFlags flags = 0;
  if (ne_modifiers & 1) flags |= NSEventModifierFlagShift;
  if (ne_modifiers & 2) flags |= NSEventModifierFlagControl;
  if (ne_modifiers & 4) flags |= NSEventModifierFlagOption;
  if (ne_modifiers & 8) flags |= NSEventModifierFlagCommand;
  return flags;
}

blink::WebMouseEvent::Button Button(int32_t button) {
  switch (button) {
    case 1:
      return blink::WebMouseEvent::Button::kMiddle;
    case 2:
      return blink::WebMouseEvent::Button::kRight;
    default:
      return blink::WebMouseEvent::Button::kLeft;
  }
}

int ButtonFlag(int32_t button) {
  switch (button) {
    case 1:
      return blink::WebInputEvent::kMiddleButtonDown;
    case 2:
      return blink::WebInputEvent::kRightButtonDown;
    default:
      return blink::WebInputEvent::kLeftButtonDown;
  }
}

std::string ToString(ne_string_view view) {
  return view.data ? std::string(view.data, view.length) : std::string();
}

// How long a navigation's frames may be held at most (BeginHold).
constexpr base::TimeDelta kHoldTimeout = base::Seconds(5);
// How long a loaded page without contentful paint waits for one before its
// frame may end the hold (MaybeEndHoldAfterLoad).
constexpr base::TimeDelta kHoldLoadGrace = base::Milliseconds(300);

std::string CssColor(uint32_t argb) {
  return base::StringPrintf("#%06x", argb & 0xFFFFFF);
}

// A copy of `source` in an IOSurface of its own, so the frame stays as it
// is while the compositor draws into its buffers again.
base::apple::ScopedCFTypeRef<IOSurfaceRef> CopySurface(IOSurfaceRef source) {
  const size_t width = IOSurfaceGetWidth(source);
  const size_t height = IOSurfaceGetHeight(source);
  const size_t bytes_per_row = IOSurfaceGetBytesPerRow(source);
  NSDictionary* properties = @{
    (id)kIOSurfaceWidth : @(width),
    (id)kIOSurfaceHeight : @(height),
    (id)kIOSurfaceBytesPerElement : @(IOSurfaceGetBytesPerElement(source)),
    (id)kIOSurfaceBytesPerRow : @(bytes_per_row),
    (id)kIOSurfacePixelFormat : @(IOSurfaceGetPixelFormat(source)),
  };
  base::apple::ScopedCFTypeRef<IOSurfaceRef> copy(
      IOSurfaceCreate((__bridge CFDictionaryRef)properties));
  if (!copy) {
    return copy;
  }
  base::apple::ScopedCFTypeRef<CFTypeRef> color_space(
      IOSurfaceCopyValue(source, kIOSurfaceColorSpace));
  if (color_space) {
    IOSurfaceSetValue(copy.get(), kIOSurfaceColorSpace, color_space.get());
  }
  IOSurfaceLock(source, kIOSurfaceLockReadOnly, nullptr);
  IOSurfaceLock(copy.get(), 0, nullptr);
  const auto* from = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(source));
  auto* to = static_cast<uint8_t*>(IOSurfaceGetBaseAddress(copy.get()));
  const size_t to_row = IOSurfaceGetBytesPerRow(copy.get());
  const size_t row = std::min(bytes_per_row, to_row);
  for (size_t y = 0; y < height; ++y) {
    // SAFETY: both surfaces are `height` rows of at least `row` bytes.
    UNSAFE_BUFFERS(std::memcpy(to + y * to_row, from + y * bytes_per_row, row));
  }
  IOSurfaceUnlock(copy.get(), 0, nullptr);
  IOSurfaceUnlock(source, kIOSurfaceLockReadOnly, nullptr);
  return copy;
}

// Whether `surface`'s `content` is blank: every second pixel of every
// second row one colour (BGRA), as a document with nothing painted yet draws
// its base or canvas colour, unless that colour is `allowed` (the app's
// surface, as about:blank and Dark Reader's empty pages draw it). Dense, so a
// form control or a thin rule on an empty page counts as painted (about 2 ms
// for a 2,700 × 1,600 frame; only while a loaded page is held).
bool Blank(IOSurfaceRef surface, const gfx::Rect& content, uint32_t allowed) {
  if (!surface || content.IsEmpty()) {
    return false;
  }
  IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
  const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
  const size_t stride = IOSurfaceGetBytesPerRow(surface);
  auto near = [](int a, int b) { return std::abs(a - b) <= 3; };
  int first[3] = {-1, -1, -1};
  bool uniform = true;
  for (int y = content.y(); y < content.bottom() && uniform; y += 2) {
    for (int x = content.x(); x < content.right() && uniform; x += 2) {
      // SAFETY: within the surface (the content rect is inside it).
      const uint8_t* pixel = UNSAFE_BUFFERS(base + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 4);
      const int bgr[3] = {UNSAFE_BUFFERS(pixel[0]), UNSAFE_BUFFERS(pixel[1]),
                          UNSAFE_BUFFERS(pixel[2])};
      if (first[0] < 0) {
        first[0] = bgr[0];
        first[1] = bgr[1];
        first[2] = bgr[2];
      } else {
        uniform = near(bgr[0], first[0]) && near(bgr[1], first[1]) &&
                  near(bgr[2], first[2]);
      }
    }
  }
  IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
  if (!uniform) {
    return false;
  }
  return !(allowed && near(first[0], allowed & 0xFF) &&
           near(first[1], (allowed >> 8) & 0xFF) &&
           near(first[2], (allowed >> 16) & 0xFF));
}

}  // namespace

gfx::Range ImeRange(int32_t start, int32_t end) {
  if (start < 0 || end < start) {
    return gfx::Range::InvalidRange();
  }
  return gfx::Range(static_cast<uint32_t>(start), static_cast<uint32_t>(end));
}

namespace {
uint64_t NextTabId() {
  static uint64_t next_id = 1;
  return next_id++;
}
}  // namespace

Tab::Tab(ne_tab* handle,
         content::BrowserContext* context,
         const ne_tab_config& config)
    : handle_(handle), id_(NextTabId()) {
  content::WebContents::CreateParams params(context);
  params.initially_hidden = false;
  web_contents_ = content::WebContents::Create(params);
  if (config.background_color_argb) {
    web_contents_->SetPageBaseBackgroundColor(config.background_color_argb);
  }
  if (config.surface_color_argb) {
    surface_color_ = config.surface_color_argb | 0xFF000000;
  }
  Init();
  std::string url = ToString(config.url);
  Load(url.empty() ? "about:blank" : url);
}

Tab::Tab(ne_tab* handle, std::unique_ptr<content::WebContents> contents)
    : handle_(handle), id_(NextTabId()), web_contents_(std::move(contents)) {
  Init();
}

void Tab::Init() {
  web_contents_->SetDelegate(this);
  // navigator.languages: the list the Accept-Language header is made from
  // (languages.h). Without it Blink reported only "en-US".
  web_contents_->GetMutableRendererPrefs()->accept_languages =
      AcceptLanguages();
  web_contents_->SyncRendererPrefs();
  Observe(web_contents_.get());
  GetEngine()->tabs.push_back(this);
  // Renderers keep producing frames although no window shows them
  // ("being captured" disables hidden-rendering optimisations).
  capture_handle_ = web_contents_->IncrementCapturerCount(
      gfx::Size(), /*stay_hidden=*/false, /*stay_awake=*/false,
      /*is_activity=*/false);
  ApplySize();
  web_contents_->WasShown();
  EnsureViewBridge();
  ApplySurfaceColor(view());
  // A new tab holds until its first page paints (BeginHold).
  hold_started_ = base::TimeTicks::Now();
  hold_timeout_.Start(FROM_HERE, kHoldTimeout,
                      base::BindOnce(&Tab::EndHold, base::Unretained(this),
                                     "timeout"));
}

Tab::~Tab() {
  GetEngine()->CancelRequestsFor(this);
  if (capturer_) {
    capturer_->StopAndResetConsumer();
  }
  capturer_.reset();
  for (auto& [id, frame] : held_frames_) {
    if (frame.callbacks) {
      frame.callbacks->Done();
    }
  }
  held_frames_.clear();
  capture_handle_.RunAndReset();
  // Views give their NSViews back before they go.
  bridges_.clear();
  Observe(nullptr);
  // The closing WebContents still calls its delegate (a tab playing sound
  // reports it stopped, NavigationStateChanged) after web_contents_ is null.
  web_contents_->SetDelegate(nullptr);
  auto& tabs = GetEngine()->tabs;
  tabs.erase(std::remove(tabs.begin(), tabs.end(), this), tabs.end());
  web_contents_.reset();
}

// static
Tab* Tab::FromWebContents(content::WebContents* contents) {
  Engine* engine = GetEngine();
  if (!engine) {
    return nullptr;
  }
  for (Tab* tab : engine->tabs) {
    if (tab->web_contents() == contents) {
      return tab;
    }
  }
  return nullptr;
}

content::RenderWidgetHostView* Tab::view() {
  return web_contents_->GetRenderWidgetHostView();
}

void Tab::DropStaleBridges() {
  for (auto it = bridges_.begin(); it != bridges_.end();) {
    ViewBridge* bridge = it->second.get();
    if (bridge->alive() && bridge->host() &&
        bridge->host()->GetView() !=
            static_cast<content::RenderWidgetHostView*>(bridge->view())) {
      bridge->ForgetView();
      it = bridges_.erase(it);
    } else {
      ++it;
    }
  }
}

void Tab::EnsureViewBridge() {
  DropStaleBridges();
  content::RenderWidgetHostView* rwhv = view();
  if (!rwhv) {
    return;
  }
  content::RenderWidgetHost* host = rwhv->GetRenderWidgetHost();
  if (bridges_.contains(host)) {
    return;
  }
  // A main frame's view on macOS is always RenderWidgetHostViewMac.
  auto* mac_view = static_cast<content::RenderWidgetHostViewMac*>(rwhv);
  auto bridge = std::make_unique<ViewBridge>(this, host, mac_view);
  if (bridge->alive()) {
    bridges_.emplace(host, std::move(bridge));
  }
}

ViewBridge* Tab::current_bridge() {
  content::RenderWidgetHostView* rwhv = view();
  if (!rwhv) {
    return nullptr;
  }
  auto it = bridges_.find(rwhv->GetRenderWidgetHost());
  // Not a bridge left on a view that went away (the widget has a new one).
  return it == bridges_.end() || !it->second->alive() ||
                 static_cast<content::RenderWidgetHostView*>(
                     it->second->view()) != rwhv
             ? nullptr
             : it->second.get();
}

void Tab::ViewBridgeGone(ViewBridge* bridge) {
  for (auto it = bridges_.begin(); it != bridges_.end(); ++it) {
    if (it->second.get() == bridge) {
      // Not while the widget is notifying its observers.
      base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
          FROM_HERE, std::move(it->second));
      bridges_.erase(it);
      return;
    }
  }
}

void Tab::NotifyCursor(int kind) {
  if (has_presenter_ && presenter_.on_cursor) {
    presenter_.on_cursor(presenter_.user_data, kind);
  }
}

void Tab::NotifyTextInputState(int32_t type, uint32_t flags) {
  if (has_presenter_ && presenter_.on_text_input_state) {
    presenter_.on_text_input_state(presenter_.user_data, type, flags);
  }
}

void Tab::NotifyImeCancel() {
  if (has_presenter_ && presenter_.on_ime_cancel) {
    presenter_.on_ime_cancel(presenter_.user_data);
  }
}

void Tab::NotifyTooltip(const std::string& text) {
  if (has_presenter_ && presenter_.on_tooltip) {
    presenter_.on_tooltip(presenter_.user_data, {text.data(), text.size()});
  }
}

void Tab::ImeSetComposition(const std::u16string& text,
                            int selection_start,
                            int selection_end,
                            const gfx::Range& replacement) {
  if (ViewBridge* bridge = current_bridge()) {
    bridge->ImeSetComposition(text, selection_start, selection_end,
                              replacement);
  } else {
    LOG(WARNING) << "IME: the page has no view bridge";
  }
}

void Tab::ImeCommit(const std::u16string& text, const gfx::Range& replacement) {
  if (ViewBridge* bridge = current_bridge()) {
    bridge->ImeCommit(text, replacement);
  }
}

void Tab::ImeFinish() {
  if (ViewBridge* bridge = current_bridge()) {
    bridge->ImeFinish();
  }
}

void Tab::ImeCancel() {
  if (ViewBridge* bridge = current_bridge()) {
    bridge->ImeCancel();
  }
}

bool Tab::ImeCaretRect(ne_rect* out_rect) {
  ViewBridge* bridge = current_bridge();
  return bridge && bridge->CaretRect(out_rect);
}

void Tab::Load(const std::string& url) {
  content::NavigationController::LoadURLParams params((GURL(url)));
  params.transition_type = ui::PageTransitionFromInt(
      ui::PAGE_TRANSITION_TYPED | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  web_contents_->GetController().LoadURLWithParams(params);
}

void Tab::SetPresenter(const ne_presenter* presenter) {
  if (presenter) {
    presenter_ = {};
    // SAFETY: the caller's struct starts with its own size; at most
    // that much (and no more than ours) is copied.
    UNSAFE_BUFFERS(std::memcpy(&presenter_, presenter,
                std::min(presenter->size, sizeof(presenter_))));
    has_presenter_ = true;
    EnsureCapture();
    if (capturer_) {
      capturer_->RequestRefreshFrame();
    }
    if (holding_) {
      // The held picture (a copy of the engine's own), if there is one.
      if (last_delivered_) {
        HeldFrame frame;
        frame.surface = last_delivered_;
        Deliver(std::move(frame), last_delivered_content_,
                last_delivered_scale_, base::TimeTicks::Now());
      }
    } else {
      PresentCompositorFrame(base::TimeTicks::Now());
    }
  } else {
    has_presenter_ = false;
    presenter_ = {};
  }
}

void Tab::SetViewSize(double width, double height, double scale_factor) {
  size_ = gfx::Size(std::max(1, static_cast<int>(width)),
                    std::max(1, static_cast<int>(height)));
  scale_factor_ = scale_factor > 0 ? scale_factor : 2.0;
  ApplySize();
}

void Tab::ApplySize() {
  // As content_shell's headless web test mode: the NSView is never in a
  // window, so its frame and the "window frame in screen" are set directly.
  NSView* web_view = web_contents_->GetNativeView().GetNativeNSView();
  web_view.frame = NSMakeRect(0, 0, size_.width(), size_.height());
  if (content::RenderWidgetHostView* rwhv = view()) {
    rwhv->SetWindowFrameInScreen(gfx::Rect(size_));
    rwhv->SetSize(size_);
  }
  if (capturer_) {
    float scale = view() ? view()->GetDeviceScaleFactor() : scale_factor_;
    gfx::Size pixels = gfx::ToCeiledSize(gfx::ScaleSize(gfx::SizeF(size_), scale));
    capturer_->SetResolutionConstraints(pixels, pixels,
                                        /*use_fixed_aspect_ratio=*/true);
  }
}

// static
bool Tab::FramesFromCompositor() {
  return !ui::RemoteLayerAPISupported();
}

void Tab::OnCompositorFrame(const gfx::CALayerParams& params) {
  base::apple::ScopedCFTypeRef<IOSurfaceRef> surface(
      IOSurfaceLookupFromMachPort(params.io_surface_mach_port.get()));
  if (!surface) {
    return;
  }
  last_output_ = std::move(surface);
  last_output_size_ = params.pixel_size;
  last_output_scale_ = params.scale_factor;
  last_output_time_ = base::TimeTicks::Now();
  if (holding_) {
    if (FrameShowsPaint()) {
      EndHold("painted");
    } else {
      MaybeEndHoldAfterLoad();
    }
    return;
  }
  PresentCompositorFrame(last_output_time_);
}

void Tab::PresentCompositorFrame(base::TimeTicks produced) {
  if (!last_output_ || !has_presenter_ || !presenter_.on_frame) {
    return;
  }
  // The display's whole output; its buffer may be larger than the frame.
  const gfx::Size surface_size(
      static_cast<int>(IOSurfaceGetWidth(last_output_.get())),
      static_cast<int>(IOSurfaceGetHeight(last_output_.get())));
  gfx::Rect content(last_output_size_);
  content.Intersect(gfx::Rect(surface_size));
  HeldFrame frame;
  frame.surface = last_output_;
  Deliver(std::move(frame), content, last_output_scale_, produced);
}

void Tab::Deliver(HeldFrame frame,
                  const gfx::Rect& content_rect,
                  float scale_factor,
                  base::TimeTicks produced) {
  IOSurfaceRef surface = frame.surface.get();
  const uint64_t id = next_frame_id_++;
  ne_frame out = {sizeof(ne_frame)};
  out.frame_id = id;
  out.kind = NE_SURFACE_IOSURFACE;
  out.surface = surface;
  out.pixel_width = static_cast<int32_t>(IOSurfaceGetWidth(surface));
  out.pixel_height = static_cast<int32_t>(IOSurfaceGetHeight(surface));
  out.scale_factor = scale_factor;
  out.content_rect = {static_cast<double>(content_rect.x()),
                      static_cast<double>(content_rect.y()),
                      static_cast<double>(content_rect.width()),
                      static_cast<double>(content_rect.height())};
  out.capture_time_us = (produced - base::TimeTicks()).InMicroseconds();
  last_delivered_.reset(surface, base::scoped_policy::RETAIN);
  last_delivered_content_ = content_rect;
  last_delivered_scale_ = scale_factor;
  held_frames_.emplace(id, std::move(frame));
  presenter_.on_frame(presenter_.user_data, &out);
}

void Tab::EnsureCapture() {
  content::RenderWidgetHostView* rwhv = view();
  if (!rwhv || !has_presenter_ || FramesFromCompositor()) {
    return;
  }
  if (rwhv == captured_view_ && capturer_) {
    return;
  }
  if (capturer_) {
    capturer_->StopAndResetConsumer();
  }
  captured_view_ = rwhv;
  capturer_ = rwhv->CreateVideoCapturer();
  capturer_->SetFormat(media::PIXEL_FORMAT_ARGB);
  capturer_->SetAnimationFpsLockIn(false, 0.0);
  capturer_->SetAutoThrottlingEnabled(false);
  capturer_->SetMinSizeChangePeriod(base::TimeDelta());
  capturer_->SetMinCapturePeriod(base::Seconds(1) / 120);
  ApplySize();
  capturer_->Start(
      this, viz::mojom::BufferFormatPreference::kPreferMappableSharedImage);
}

void Tab::OnFrameCaptured(
    media::mojom::VideoBufferHandlePtr data,
    media::mojom::VideoFrameInfoPtr info,
    const gfx::Rect& content_rect,
    mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
        callbacks) {
  HeldFrame frame;
  frame.callbacks.Bind(std::move(callbacks));
  if (!data->is_gpu_memory_buffer_handle()) {
    if (!logged_unexpected_buffer_) {
      LOG(ERROR) << "capturer sent a non-IOSurface buffer (tag "
                 << static_cast<int>(data->which()) << "); dropping frames";
      logged_unexpected_buffer_ = true;
    }
    frame.callbacks->Done();
    return;
  }
  IOSurfaceRef surface = data->get_gpu_memory_buffer_handle().io_surface().get();
  if (!surface || !has_presenter_ || !presenter_.on_frame) {
    frame.callbacks->Done();
    return;
  }
  if (holding_) {
    frame.callbacks->Done();
    return;
  }
  frame.surface.reset(surface, base::scoped_policy::RETAIN);
  // When the capturer saw the display draw the frame (it copies it in the
  // display's next pass).
  const base::TimeTicks produced =
      info->metadata.capture_begin_time.value_or(base::TimeTicks::Now());
  Deliver(std::move(frame), content_rect,
          view() ? view()->GetDeviceScaleFactor() : scale_factor_, produced);
}

void Tab::ReleaseFrame(uint64_t frame_id) {
  auto it = held_frames_.find(frame_id);
  if (it == held_frames_.end()) {
    return;
  }
  if (it->second.callbacks) {
    it->second.callbacks->Done();
  }
  held_frames_.erase(it);
}

void Tab::OnLog(const std::string& message) {
  VLOG(1) << "capturer: " << message;
}

void Tab::SetVisible(bool visible) {
  if (visible) {
    web_contents_->WasShown();
  } else {
    web_contents_->WasHidden();
  }
}

void Tab::SetFocus(bool focused) {
  content::RenderWidgetHostView* rwhv = view();
  if (!rwhv) {
    return;
  }
  if (ViewBridge* bridge = current_bridge()) {
    bridge->SetFocus(focused);
    return;
  }
  // As headless web tests do: there is no window to make the view first
  // responder, so tell the widget directly.
  content::RenderWidgetHost* host = rwhv->GetRenderWidgetHost();
  host->SetActive(focused);
  if (focused) {
    host->Focus();
  } else {
    host->Blur();
  }
}

void Tab::SendInput(const ne_input_event& event) {
  content::RenderWidgetHostView* rwhv = view();
  if (!rwhv) {
    return;
  }
  // Input goes in through the main frame's view, as AppKit's events do in
  // Chrome: the view routes pointer and wheel events by hit testing to the
  // widget under the pointer (a cross-site frame renders in another
  // process, with a widget of its own) and keys to the focused widget.
  // Forwarding to the main frame's widget directly left cross-site frames
  // without input: the main frame's renderer only sees the frame's element.
  // A main frame's view on macOS is always RenderWidgetHostViewMac.
  auto* mac_view = static_cast<content::RenderWidgetHostViewMac*>(rwhv);
  const base::TimeTicks now = base::TimeTicks::Now();
  const gfx::PointF position(event.x, event.y);
  if (event.kind != NE_INPUT_KEY_DOWN && event.kind != NE_INPUT_KEY_UP &&
      event.kind != NE_INPUT_CHAR) {
    last_pointer_ = position;
  }
  switch (event.kind) {
    case NE_INPUT_MOUSE_DOWN:
    case NE_INPUT_MOUSE_UP:
    case NE_INPUT_MOUSE_MOVE:
    case NE_INPUT_MOUSE_LEAVE: {
      blink::WebInputEvent::Type type =
          event.kind == NE_INPUT_MOUSE_DOWN ? blink::WebInputEvent::Type::kMouseDown
          : event.kind == NE_INPUT_MOUSE_UP ? blink::WebInputEvent::Type::kMouseUp
          : event.kind == NE_INPUT_MOUSE_MOVE
              ? blink::WebInputEvent::Type::kMouseMove
              : blink::WebInputEvent::Type::kMouseLeave;
      int buttons = 0;
      if (event.kind == NE_INPUT_MOUSE_DOWN ||
          (event.kind == NE_INPUT_MOUSE_MOVE && event.click_count > 0)) {
        buttons = ButtonFlag(event.button);
      }
      blink::WebMouseEvent mouse(type, Modifiers(event.modifiers, buttons), now);
      mouse.SetPositionInWidget(position);
      mouse.SetPositionInScreen(position);
      if (event.kind == NE_INPUT_MOUSE_DOWN || event.kind == NE_INPUT_MOUSE_UP ||
          buttons) {
        mouse.button = Button(event.button);
      } else {
        mouse.button = blink::WebMouseEvent::Button::kNoButton;
      }
      mouse.click_count = event.click_count;
      mac_view->RouteOrProcessMouseEvent(mouse);
      break;
    }
    case NE_INPUT_WHEEL: {
      blink::WebMouseWheelEvent wheel(blink::WebInputEvent::Type::kMouseWheel,
                                      Modifiers(event.modifiers), now);
      wheel.SetPositionInWidget(position);
      wheel.SetPositionInScreen(position);
      wheel.delta_x = event.delta_x;
      wheel.delta_y = event.delta_y;
      wheel.wheel_ticks_x = event.delta_x / 40.f;
      wheel.wheel_ticks_y = event.delta_y / 40.f;
      wheel.delta_units = event.precise
                              ? ui::ScrollGranularity::kScrollByPrecisePixel
                              : ui::ScrollGranularity::kScrollByPixel;
      wheel.phase = static_cast<blink::WebMouseWheelEvent::Phase>(event.phase);
      wheel.momentum_phase =
          static_cast<blink::WebMouseWheelEvent::Phase>(event.momentum_phase);
      // The view's MouseWheelPhaseHandler adds phases to a wheel without
      // them (a mouse wheel) and ends a scroll after a pause, latched to
      // the widget it began over.
      mac_view->RouteOrProcessWheelEvent(wheel);
      break;
    }
    case NE_INPUT_PINCH: {
      // As RenderWidgetHostViewCocoa's -magnifyWithEvent: builds it
      // (WebGestureEventBuilder): a touchpad pinch the page first sees as
      // ctrl+wheel. The view applies Chrome's threshold before zooming.
      blink::WebInputEvent::Type type =
          event.phase == 1   ? blink::WebInputEvent::Type::kGesturePinchBegin
          : event.phase == 2 ? blink::WebInputEvent::Type::kGesturePinchUpdate
                             : blink::WebInputEvent::Type::kGesturePinchEnd;
      blink::WebGestureEvent pinch(type, Modifiers(event.modifiers), now,
                                   blink::WebGestureDevice::kTouchpad);
      pinch.SetPositionInWidget(position);
      pinch.SetPositionInScreen(position);
      if (type == blink::WebInputEvent::Type::kGesturePinchUpdate) {
        pinch.data.pinch_update.scale = event.delta_x;
      }
      pinch.SetNeedsWheelEvent(true);
      mac_view->PinchEvent(pinch, /*is_synthetically_injected=*/false);
      break;
    }
    case NE_INPUT_KEY_DOWN:
    case NE_INPUT_KEY_UP:
    case NE_INPUT_CHAR: {
      // Build the NSEvent AppKit would have sent, and let Chromium convert it
      // (key codes, DOM codes and keys) as RenderWidgetHostViewMac does.
      NSString* text = base::SysUTF8ToNSString(ToString(event.text));
      NSString* unmodified =
          event.unmodified_text.length
              ? base::SysUTF8ToNSString(ToString(event.unmodified_text))
              : text;
      NSEvent* ns_event = [NSEvent
                    keyEventWithType:event.kind == NE_INPUT_KEY_UP ? NSEventTypeKeyUp
                                                                  : NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:NSModifiers(event.modifiers)
                           timestamp:[[NSProcessInfo processInfo] systemUptime]
                        windowNumber:0
                             context:nil
                          characters:text
         charactersIgnoringModifiers:unmodified
                           isARepeat:NO
                             keyCode:static_cast<unsigned short>(
                                         event.native_key_code)];
      input::NativeWebKeyboardEvent key(gfx::NativeEvent{ns_event});
      if (event.kind == NE_INPUT_CHAR) {
        key.SetType(blink::WebInputEvent::Type::kChar);
      }
      // To the focused widget, as the Mac view's -keyEvent: sends them.
      ui::LatencyInfo latency;
      latency.AddLatencyNumber(ui::INPUT_EVENT_LATENCY_UI_COMPONENT);
      mac_view->BeginKeyboardEvent();
      mac_view->ForwardKeyboardEvent(key, latency);
      // A key press that inserts text also sends a Char event, as the Mac
      // view's -insertText: does. Command-key shortcuts don't.
      // Function keys (arrows and the like) carry AppKit's private-use
      // characters and insert nothing.
      const unichar first = text.length > 0 ? [text characterAtIndex:0] : 0;
      const bool function_key = first >= 0xF700 && first <= 0xF8FF;
      if (event.kind == NE_INPUT_KEY_DOWN && text.length > 0 && !function_key &&
          !(event.modifiers & (2 | 8)) &&
          !(event.flags & NE_INPUT_FLAG_NO_CHAR)) {
        input::NativeWebKeyboardEvent char_event(gfx::NativeEvent{ns_event});
        char_event.SetType(blink::WebInputEvent::Type::kChar);
        mac_view->ForwardKeyboardEvent(char_event, latency);
      }
      mac_view->EndKeyboardEvent();
      break;
    }
  }
}

void Tab::Edit(ne_edit_command command) {
  switch (command) {
    case NE_EDIT_UNDO:
      web_contents_->Undo();
      break;
    case NE_EDIT_REDO:
      web_contents_->Redo();
      break;
    case NE_EDIT_CUT:
      web_contents_->Cut();
      break;
    case NE_EDIT_COPY:
      web_contents_->Copy();
      break;
    case NE_EDIT_PASTE:
      web_contents_->Paste();
      break;
    case NE_EDIT_PASTE_AND_MATCH_STYLE:
      web_contents_->PasteAndMatchStyle();
      break;
    case NE_EDIT_SELECT_ALL:
      web_contents_->SelectAll();
      break;
    case NE_EDIT_DELETE:
      web_contents_->Delete();
      break;
  }
}

void Tab::Find(const std::string& text, uint32_t flags) {
  auto options = blink::mojom::FindOptions::New();
  options->forward = flags & NE_FIND_FORWARD;
  options->match_case = flags & NE_FIND_MATCH_CASE;
  options->new_session = !(flags & NE_FIND_NEXT);
  web_contents_->Find(base::UTF8ToUTF16(text), std::move(options),
                      /*skip_delay=*/true, [](int) {});
}

void Tab::StopFinding(bool keep_selection) {
  web_contents_->StopFinding(keep_selection
                                 ? content::STOP_FIND_ACTION_KEEP_SELECTION
                                 : content::STOP_FIND_ACTION_CLEAR_SELECTION);
}

void Tab::Eval(const std::string& script,
               ne_eval_reason reason,
               ne_eval_fn done,
               void* user_data) {
  content::RenderFrameHost* frame = web_contents_->GetPrimaryMainFrame();
  auto callback = EvalReply::Callback(done, user_data);
  if (reason == NE_EVAL_APP_FEATURE) {
    frame->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(script), std::move(callback),
        content::ISOLATED_WORLD_ID_CONTENT_END);
  } else {
    // The main world (ne_app_init allows injecting JavaScript).
    frame->ExecuteJavaScript(base::UTF8ToUTF16(script), std::move(callback));
  }
}

content::RenderFrameHost* Tab::FindFrame(ne_frame_id id) {
  if (id == 0) {
    return web_contents_->GetPrimaryMainFrame();
  }
  content::RenderFrameHost* frame =
      web_contents_->UnsafeFindFrameByFrameTreeNodeId(
          content::FrameTreeNodeId(static_cast<int>(id)));
  // Only frames of the page on screen (not prerendered or cached ones).
  if (!frame || !frame->IsActive() ||
      &frame->GetPage() != &web_contents_->GetPrimaryPage()) {
    return nullptr;
  }
  return frame;
}

void Tab::EvalInFrame(ne_frame_id id,
                      const std::string& script,
                      ne_world world,
                      ne_eval_fn done,
                      void* user_data) {
  content::RenderFrameHost* frame = FindFrame(id);
  if (!frame || !frame->IsRenderFrameLive() ||
      !frame->GetLastCommittedURL().is_valid()) {
    if (done) {
      done(user_data, NE_ERR_GONE, nullptr);
    }
    return;
  }
  auto callback = EvalReply::Callback(done, user_data);
  if (world == NE_WORLD_APP) {
    frame->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(script), std::move(callback),
        content::ISOLATED_WORLD_ID_CONTENT_END);
  } else {
    frame->ExecuteJavaScript(base::UTF8ToUTF16(script), std::move(callback));
  }
}

void Tab::ForEachFrame(ne_frame_fn fn, void* user_data) {
  if (!fn) {
    return;
  }
  web_contents_->GetPrimaryMainFrame()->ForEachRenderFrameHost(
      [&](content::RenderFrameHost* frame) {
        if (!frame->IsRenderFrameLive() ||
            !frame->GetLastCommittedURL().is_valid()) {
          return;
        }
        std::string url = frame->GetLastCommittedURL().spec();
        fn(user_data, frame->GetFrameTreeNodeId().value(),
           {url.data(), url.size()}, frame->GetParentOrOuterDocument() == nullptr);
      });
}

void Tab::NotifyNavigationState() {
  Engine* engine = GetEngine();
  if (!engine->has_ui_delegate || !engine->ui_delegate.on_navigation_state) {
    return;
  }
  std::string url = web_contents_->GetVisibleURL().spec();
  std::string title = base::UTF16ToUTF8(web_contents_->GetTitle());
  auto& controller = web_contents_->GetController();
  engine->ui_delegate.on_navigation_state(
      engine->ui_delegate.user_data, handle_, {url.data(), url.size()},
      {title.data(), title.size()}, web_contents_->IsLoading(),
      controller.CanGoBack(), controller.CanGoForward());
}

void Tab::NavigationStateChanged(content::WebContents* source,
                                 content::InvalidateTypes changed_flags) {
  NotifyNavigationState();
}

void Tab::LoadingStateChanged(content::WebContents* source,
                              bool should_show_loading_ui) {
  NotifyNavigationState();
}

content::WebContents* Tab::AddNewContents(
    content::WebContents* source,
    std::unique_ptr<content::WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& window_features,
    bool user_gesture,
    bool* was_blocked) {
  // The new page is offered to the app as a tab of its own; the app adopts
  // it or declines (and may load the URL itself, as the CEF pane did).
  auto* new_handle = new ne_tab;
  new_handle->impl = std::make_unique<Tab>(new_handle, std::move(new_contents));
  content::WebContents* contents = new_handle->impl->web_contents();
  OfferNewTab(new_handle, target_url, disposition);
  if (was_blocked) {
    *was_blocked = false;
  }
  return contents;
}

content::WebContents* Tab::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)>
        navigation_handle_callback) {
  // As content_shell does. Links opened elsewhere (⌘-click, a middle click,
  // shift-click, a form targeting a new window) come here rather than to
  // AddNewContents: they get a new page, loaded here with the request's
  // referrer and body, offered to the app as window.open's are.
  content::WebContents* target = nullptr;
  switch (params.disposition) {
    case WindowOpenDisposition::CURRENT_TAB:
      target = source;
      break;
    case WindowOpenDisposition::NEW_FOREGROUND_TAB:
    case WindowOpenDisposition::NEW_BACKGROUND_TAB:
    case WindowOpenDisposition::NEW_POPUP:
    case WindowOpenDisposition::NEW_WINDOW: {
      content::WebContents::CreateParams create(source->GetBrowserContext(),
                                                params.source_site_instance);
      create.initially_hidden = false;
      auto* new_handle = new ne_tab;
      new_handle->impl = std::make_unique<Tab>(
          new_handle, content::WebContents::Create(create));
      target = new_handle->impl->web_contents();
      base::WeakPtr<content::NavigationHandle> navigation =
          target->GetController().LoadURLWithParams(
              content::NavigationController::LoadURLParams(params));
      if (navigation_handle_callback && navigation) {
        std::move(navigation_handle_callback).Run(*navigation);
      }
      OfferNewTab(new_handle, params.url, params.disposition);
      return target;
    }
    default:
      return nullptr;
  }
  base::WeakPtr<content::NavigationHandle> navigation =
      target->GetController().LoadURLWithParams(
          content::NavigationController::LoadURLParams(params));
  if (navigation_handle_callback && navigation) {
    std::move(navigation_handle_callback).Run(*navigation);
  }
  return target;
}

void Tab::OfferNewTab(ne_tab* new_handle,
                      const GURL& url,
                      WindowOpenDisposition disposition) {
  std::string spec = url.spec();
  ne_ui_request request = {sizeof(ne_ui_request)};
  request.kind = NE_UI_NEW_TAB;
  request.tab = handle_;
  request.message = {spec.data(), spec.size()};
  request.url = {spec.data(), spec.size()};
  request.new_tab = new_handle;
  switch (disposition) {
    case WindowOpenDisposition::NEW_BACKGROUND_TAB:
      request.disposition = NE_DISPOSITION_BACKGROUND_TAB;
      break;
    case WindowOpenDisposition::NEW_POPUP:
      request.disposition = NE_DISPOSITION_POPUP;
      break;
    case WindowOpenDisposition::NEW_WINDOW:
      request.disposition = NE_DISPOSITION_WINDOW;
      break;
    default:
      request.disposition = NE_DISPOSITION_FOREGROUND_TAB;
      break;
  }
  GetEngine()->SendUiRequest(
      request, this,
      base::BindOnce(
          [](ne_tab* new_handle, const ne_ui_response* response) {
            if (!(response && response->accept)) {
              // Declined: the page goes, after this call returns.
              base::SequencedTaskRunner::GetCurrentDefault()->DeleteSoon(
                  FROM_HERE, new_handle);
            }
          },
          new_handle));
}

void Tab::FindReply(content::WebContents* web_contents,
                    int request_id,
                    int number_of_matches,
                    const gfx::Rect& selection_rect,
                    int active_match_ordinal,
                    bool final_update) {
  Engine* engine = GetEngine();
  if (engine->has_ui_delegate && engine->ui_delegate.on_find_result) {
    engine->ui_delegate.on_find_result(engine->ui_delegate.user_data, handle_,
                                       active_match_ordinal, number_of_matches,
                                       final_update);
  }
}

bool Tab::DidAddMessageToConsole(content::WebContents* source,
                                 blink::mojom::ConsoleMessageLevel log_level,
                                 const std::u16string& message,
                                 int32_t line_no,
                                 const std::u16string& source_id) {
  // The observer (OnDidAddMessageToConsole) ran just before and asked the app.
  bool consumed = console_consumed_;
  console_consumed_ = false;
  if (!consumed) {
    VLOG(1) << "console: " << message;
  }
  return true;
}

void Tab::OnDidAddMessageToConsole(
    content::RenderFrameHost* source_frame,
    blink::mojom::ConsoleMessageLevel log_level,
    const std::u16string& message,
    int32_t line_no,
    const std::u16string& source_id,
    const std::optional<std::u16string>& untrusted_stack_trace) {
  console_consumed_ = false;
  Engine* engine = GetEngine();
  if (!engine->has_ui_delegate || !engine->ui_delegate.on_console_message) {
    return;
  }
  std::string text = base::UTF16ToUTF8(message);
  std::string source = base::UTF16ToUTF8(source_id);
  const ne_frame_id frame =
      source_frame ? source_frame->GetFrameTreeNodeId().value() : 0;
  console_consumed_ = engine->ui_delegate.on_console_message(
      engine->ui_delegate.user_data, handle_, frame,
      static_cast<int32_t>(log_level), {text.data(), text.size()},
      {source.data(), source.size()}, line_no);
}

void Tab::NotifyLoadEvent(content::RenderFrameHost* frame,
                          ne_load_event_kind kind,
                          const GURL& url) {
  Engine* engine = GetEngine();
  if (!engine->has_ui_delegate || !engine->ui_delegate.on_load_event) {
    return;
  }
  std::string spec = url.spec();
  const bool is_main = !frame || frame->GetParentOrOuterDocument() == nullptr;
  const ne_frame_id id = frame ? frame->GetFrameTreeNodeId().value() : 0;
  engine->ui_delegate.on_load_event(engine->ui_delegate.user_data, handle_, id,
                                    is_main, kind, {spec.data(), spec.size()});
}

void Tab::DidStartNavigation(content::NavigationHandle* navigation_handle) {
  if (navigation_handle->IsInPrimaryMainFrame() &&
      !navigation_handle->IsSameDocument()) {
    NotifyLoadEvent(nullptr, NE_LOAD_NAVIGATION_STARTED,
                    navigation_handle->GetURL());
  }
}

void Tab::ReadyToCommitNavigation(
    content::NavigationHandle* navigation_handle) {
  if (navigation_handle->IsSameDocument()) {
    return;
  }
  const bool is_main = navigation_handle->IsInPrimaryMainFrame();
  // A new document in the page: its frames wait for its first contentful
  // paint. Back-forward cache restores and prerendered pages have painted.
  if (is_main && !navigation_handle->IsPageActivation()) {
    BeginHold(navigation_handle->GetNavigationId());
  }
  content::RenderFrameHost* frame = navigation_handle->GetRenderFrameHost();
  if (!frame || !(frame->GetPage().IsPrimary() ||
                  frame->GetLifecycleState() ==
                      content::RenderFrameHost::LifecycleState::kPendingCommit)) {
    return;
  }
  // The frame's messages go with the commit (frame_agent.mojom):
  // channel-associated, so they arrive before the document does.
  // (IsErrorPage() is only true once committed.)
  if (navigation_handle->GetNetErrorCode() != net::OK) {
    // Content's error page is an empty white document; ours is in the
    // app's surface colour. No document-start scripts: it isn't a site's.
    if (is_main) {
      mojo::AssociatedRemote<mojom::FrameAgent> agent;
      frame->GetRemoteAssociatedInterfaces()->GetInterface(&agent);
      agent->SetErrorPage(ErrorPageHtml(navigation_handle));
    }
    return;
  }
  // about:blank the app opened (typed, or a new tab's) is in its surface
  // colour, not white. Pages' own about:blank documents are left as they are.
  std::string blank_style;
  if (is_main && surface_color_ && navigation_handle->GetURL().IsAboutBlank() &&
      !navigation_handle->IsRendererInitiated()) {
    blank_style = BlankPageScript();
  }
  Engine* engine = GetEngine();
  if (!engine->document_start && blank_style.empty()) {
    return;
  }
  const std::string url = navigation_handle->GetURL().spec();
  const std::string top =
      is_main ? url
              : web_contents_->GetPrimaryMainFrame()->GetLastCommittedURL().spec();
  const ne_frame_id id = navigation_handle->GetFrameTreeNodeId().value();
  auto ask = [&](ne_frame_id frame_id, const std::string& frame_url,
                 bool main) {
    if (!engine->document_start) {
      return std::string();
    }
    const char* script = engine->document_start(
        engine->document_start_data.get(), handle_, frame_id,
        {frame_url.data(), frame_url.size()}, {top.data(), top.size()}, main);
    return script ? std::string(script) : std::string();
  };
  // The surface style and the app's scripts run as separate parts.
  std::string script = ask(id, url, is_main);
  if (!blank_style.empty()) {
    script = script.empty() ? blank_style : blank_style + "\n//\x1e\n" + script;
  }
  std::string blank_child = ask(-1, "about:blank", false);
  if (script.empty() && blank_child.empty()) {
    return;
  }
  mojo::AssociatedRemote<mojom::FrameAgent> agent;
  frame->GetRemoteAssociatedInterfaces()->GetInterface(&agent);
  agent->SetDocumentStartScripts(script, blank_child);
}

void Tab::DidFinishNavigation(content::NavigationHandle* navigation_handle) {
  if (holding_ && hold_navigation_ &&
      navigation_handle->GetNavigationId() == hold_navigation_) {
    if (navigation_handle->HasCommitted()) {
      hold_committed_ = true;
    } else {
      // It never replaced the document on screen.
      EndHold("not committed");
    }
  }
  if (!navigation_handle->HasCommitted() ||
      navigation_handle->IsSameDocument() ||
      !navigation_handle->GetRenderFrameHost() ||
      !navigation_handle->GetRenderFrameHost()->GetPage().IsPrimary()) {
    return;
  }
  if (navigation_handle->IsInPrimaryMainFrame()) {
    // A PDF's document: the app hears it before the commit (it shows the
    // PDF over it), and it never enters the back-forward cache (going back
    // to it fetches the PDF again, so the app always has its file).
    uint64_t shown_pdf = 0;
    auto opened = pdf_navigations_.find(navigation_handle->GetNavigationId());
    if (opened != pdf_navigations_.end()) {
      shown_pdf = opened->second;
      content::BackForwardCache::DisableForRenderFrameHost(
          navigation_handle->GetRenderFrameHost(),
          content::BackForwardCache::DisabledReason(
              content::BackForwardCache::DisabledSource::kEmbedder, 1,
              "A PDF shown by the app", "", "pdf"));
    }
    PdfDocumentCommitted(shown_pdf);
  }
  NotifyLoadEvent(navigation_handle->GetRenderFrameHost(), NE_LOAD_COMMITTED,
                  navigation_handle->GetURL());
  // A page from the back-forward cache (or prerendered) has painted
  // already: no hold, and the app hears so after the commit.
  if (navigation_handle->IsInPrimaryMainFrame() &&
      navigation_handle->IsPageActivation()) {
    if (holding_) {
      EndHold("page restored");
    } else {
      NotifyFirstPaint();
    }
  }
}

void Tab::DidFinishLoad(content::RenderFrameHost* render_frame_host,
                        const GURL& validated_url) {
  if (!render_frame_host->GetPage().IsPrimary()) {
    return;
  }
  NotifyLoadEvent(render_frame_host, NE_LOAD_FINISHED, validated_url);
}

void Tab::OnBackgroundColorChanged() {
  if (has_presenter_ && presenter_.on_background_color) {
    std::optional<SkColor> color = web_contents_->GetBackgroundColor();
    presenter_.on_background_color(presenter_.user_data,
                                   color.value_or(SK_ColorWHITE));
  }
}

content::JavaScriptDialogManager* Tab::GetJavaScriptDialogManager(
    content::WebContents* source) {
  return this;
}

void Tab::RenderViewReady() {
  EnsureViewBridge();
  EnsureCapture();
  ApplySize();
  ApplySurfaceColor(view());
}

void Tab::RenderFrameHostChanged(content::RenderFrameHost* old_host,
                                 content::RenderFrameHost* new_host) {
  if (new_host && !new_host->GetParentOrOuterDocument()) {
    EnsureViewBridge();
    ApplySurfaceColor(new_host->GetView());
  }
}

void Tab::PrimaryPageChanged(content::Page& page) {
  EnsureViewBridge();
  EnsureCapture();
  ApplySize();
  ApplySurfaceColor(view());
  NotifyNavigationState();
}

void Tab::OnFirstContentfulPaintInPrimaryMainFrame(
    base::TimeTicks presentation_time) {
  // Only the held document's paint counts (not the old one's, late).
  if (!holding_ || !(hold_committed_ || !hold_navigation_)) {
    return;
  }
  hold_paint_time_ = presentation_time;
  if (FrameShowsPaint()) {
    EndHold("painted");
    return;
  }
  // Its frame hasn't reached the view yet: the next frame ends the hold
  // (OnCompositorFrame), or this does.
  hold_paint_wait_.Start(FROM_HERE, base::Milliseconds(150),
                         base::BindOnce(&Tab::EndHold, base::Unretained(this),
                                        "painted, no frame"));
}

void Tab::OnRendererFrame(base::TimeTicks activation_time) {
  renderer_frame_time_ = activation_time;
}

bool Tab::RendererDrawnInto(base::TimeTicks frame_time) const {
  // The display drew this frame after a frame of the page's renderer
  // activated since the hold began: not the view's background or the
  // previous page's content standing in for it.
  return !renderer_frame_time_.is_null() &&
         renderer_frame_time_ >= hold_started_ &&
         frame_time > renderer_frame_time_;
}

bool Tab::FrameShowsPaint() const {
  // A frame reaches the view as the GPU process swaps it, up to about a
  // display refresh before the presentation time the paint reports (16 ms
  // measured at 120 Hz); one drawn earlier than that is from before it.
  return hold_paint_time_ && last_output_ &&
         RendererDrawnInto(last_output_time_) &&
         last_output_time_ >= *hold_paint_time_ - base::Milliseconds(25);
}

void Tab::PrimaryMainFrameRenderProcessGone(base::TerminationStatus status) {
  // The crashed view goes; a reload gives the widget a new one.
  DropStaleBridges();
  EndHold("renderer gone");
  // The app shows its crashed page (Chrome's sad tab shows for the same
  // statuses, SadTab::ShouldShow).
  int32_t reason = 0;
  switch (status) {
    case base::TERMINATION_STATUS_ABNORMAL_TERMINATION:
    case base::TERMINATION_STATUS_PROCESS_CRASHED:
      reason = NE_RENDERER_CRASHED;
      break;
    case base::TERMINATION_STATUS_PROCESS_WAS_KILLED:
      reason = NE_RENDERER_KILLED;
      break;
    case base::TERMINATION_STATUS_OOM:
      reason = NE_RENDERER_OUT_OF_MEMORY;
      break;
    case base::TERMINATION_STATUS_LAUNCH_FAILED:
      reason = NE_RENDERER_LAUNCH_FAILED;
      break;
    case base::TERMINATION_STATUS_NORMAL_TERMINATION:
    case base::TERMINATION_STATUS_STILL_RUNNING:
      break;
    default:
      reason = NE_RENDERER_OTHER;
      break;
  }
  VLOG(1) << "renderer gone, status " << static_cast<int>(status);
  Engine* engine = GetEngine();
  if (reason && engine->has_ui_delegate && engine->ui_delegate.on_renderer_gone) {
    engine->ui_delegate.on_renderer_gone(engine->ui_delegate.user_data,
                                         handle_, reason);
  }
}

void Tab::BeginHold(int64_t navigation_id) {
  hold_navigation_ = navigation_id;
  hold_committed_ = false;
  hold_paint_time_.reset();
  hold_paint_wait_.Stop();
  hold_loaded_at_ = base::TimeTicks();
  hold_load_grace_.Stop();
  hold_timeout_.Start(FROM_HERE, kHoldTimeout,
                      base::BindOnce(&Tab::EndHold, base::Unretained(this),
                                     "timeout"));
  if (holding_) {
    // Already held (a new tab, or a navigation before the last one
    // painted): the app keeps what it shows.
    return;
  }
  holding_ = true;
  hold_started_ = base::TimeTicks::Now();
  // The app keeps the frame it shows, but that is a buffer of a compositor
  // that may draw into it again (a recycled one, for the new view): it gets
  // a copy that stays as it is.
  if (last_delivered_ && has_presenter_ && presenter_.on_frame) {
    base::apple::ScopedCFTypeRef<IOSurfaceRef> copy =
        CopySurface(last_delivered_.get());
    if (copy) {
      HeldFrame frame;
      frame.surface = std::move(copy);
      Deliver(std::move(frame), last_delivered_content_, last_delivered_scale_,
              hold_started_);
    }
  }
}

void Tab::EndHold(const char* why) {
  if (!holding_) {
    return;
  }
  holding_ = false;
  hold_timeout_.Stop();
  hold_paint_wait_.Stop();
  hold_paint_time_.reset();
  hold_loaded_at_ = base::TimeTicks();
  hold_load_grace_.Stop();
  VLOG(1) << "hold ended (" << why << ") after "
          << (base::TimeTicks::Now() - hold_started_).InMilliseconds()
          << " ms";
  // The new document's latest frame, if it drew one meanwhile (else its
  // next frame comes as usual), then the app hears the hold ended.
  if (last_output_ && last_output_time_ >= hold_started_) {
    PresentCompositorFrame(last_output_time_);
  }
  NotifyFirstPaint();
}

void Tab::NotifyFirstPaint() {
  if (has_presenter_ && presenter_.on_first_paint) {
    std::optional<SkColor> color = web_contents_->GetBackgroundColor();
    presenter_.on_first_paint(presenter_.user_data,
                              color.value_or(SK_ColorWHITE));
  }
}

void Tab::MaybeEndHoldAfterLoad() {
  if (!holding_ || !(hold_committed_ || !hold_navigation_) ||
      web_contents_->IsLoading() || hold_loaded_at_.is_null() ||
      base::TimeTicks::Now() - hold_loaded_at_ < kHoldLoadGrace ||
      !last_output_ || !RendererDrawnInto(last_output_time_)) {
    return;
  }
  gfx::Rect content(last_output_size_);
  content.Intersect(
      gfx::Rect(static_cast<int>(IOSurfaceGetWidth(last_output_.get())),
                static_cast<int>(IOSurfaceGetHeight(last_output_.get()))));
  // Loaded, but blank: it may still paint (a page its scripts build, an
  // error page drawing its canvas before its style).
  if (!Blank(last_output_.get(), content, surface_color_)) {
    EndHold("loaded");
  }
}

void Tab::SetSurfaceColor(uint32_t argb) {
  surface_color_ = argb ? (argb | 0xFF000000) : 0;
  ApplySurfaceColor(view());
  content::RenderFrameHost* main = web_contents_->GetPrimaryMainFrame();
  if (surface_color_ && main && main->IsRenderFrameLive() &&
      main->GetLastCommittedURL().IsAboutBlank()) {
    main->ExecuteJavaScript(base::UTF8ToUTF16(BlankPageScript()),
                            base::NullCallback());
  }
}

void Tab::ApplySurfaceColor(content::RenderWidgetHostView* view) {
  // Shown wherever the view has no page content yet (Chrome sets its
  // window's colour the same way, ContentsWebView::UpdateBackgroundColor).
  if (view && surface_color_) {
    view->SetBackgroundColor(surface_color_);
  }
}

std::string Tab::BlankPageScript() const {
  // Runs as the document's script context is created, before it has a
  // root element.
  return base::StringPrintf(
      "(() => { const apply = () => { let s = "
      "document.getElementById('__notolibra_surface'); if (!s) { s = "
      "document.createElement('style'); s.id = '__notolibra_surface'; "
      "(document.head || document.documentElement).appendChild(s); } "
      "s.textContent = ':root { background: %s; color-scheme: %s }'; }; if "
      "(document.documentElement) apply(); else new MutationObserver((_, o) "
      "=> { if (document.documentElement) { o.disconnect(); apply(); } "
      "}).observe(document, { childList: true }); })();\n",
      CssColor(surface_color_).c_str(),
      color_utils::IsDark(surface_color_) ? "dark" : "light");
}

std::string Tab::ErrorPageHtml(content::NavigationHandle* navigation) const {
  const SkColor background = surface_color_ ? surface_color_ : SK_ColorWHITE;
  const bool dark = color_utils::IsDark(background);
  const std::string url = base::EscapeForHTML(navigation->GetURL().spec());
  const std::string host = base::EscapeForHTML(navigation->GetURL().host());
  const std::string error = net::ErrorToShortString(navigation->GetNetErrorCode());
  return base::StringPrintf(
      "<!doctype html><html><head><meta charset=\"utf-8\"><meta "
      "name=\"color-scheme\" content=\"%s\"><title>%s</title><style>html "
      "{ background: %s; color: %s; font: 15px -apple-system, "
      "BlinkMacSystemFont, sans-serif } body { margin: 0; padding: 72px 48px; "
      "max-width: 640px } h1 { font-size: 20px; font-weight: 600; margin: 0 0 "
      "12px } p { margin: 6px 0; opacity: .72; overflow-wrap: anywhere } code "
      "{ font: 13px ui-monospace, Menlo, monospace; opacity: .6 "
      "}</style></head><body><h1>This page couldn&rsquo;t be "
      "loaded</h1><p>%s</p><p><code>%s</code></p></body></html>",
      dark ? "dark" : "light", host.empty() ? url.c_str() : host.c_str(),
      CssColor(background).c_str(), dark ? "#e6e6e6" : "#1f1f1f", url.c_str(),
      error.c_str());
}

Tab::Pdf::Pdf() = default;
Tab::Pdf::Pdf(Pdf&&) = default;
Tab::Pdf& Tab::Pdf::operator=(Pdf&&) = default;
Tab::Pdf::~Pdf() = default;

std::string Tab::PdfDocumentHtml(const std::string& file_name,
                                 const GURL& url,
                                 bool main_frame) const {
  const SkColor background = surface_color_ ? surface_color_ : SK_ColorWHITE;
  const bool dark = color_utils::IsDark(background);
  const std::string name = base::EscapeForHTML(file_name);
  // The main frame's is hidden under the app's PDF view; it paints its
  // name, so its paint holding ends at once. A subframe's links to the PDF
  // in the top frame, where the app shows it.
  const std::string body =
      main_frame
          ? "<main><svg viewBox=\"0 0 24 24\"><path d=\"M14 2H6a2 2 0 0 "
            "0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z\"/><path "
            "d=\"M14 2v6h6\"/></svg><p class=\"name\">" +
                name + "</p><p>PDF document</p></main>"
          : "<main><svg viewBox=\"0 0 24 24\"><path d=\"M14 2H6a2 2 0 0 "
            "0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z\"/><path "
            "d=\"M14 2v6h6\"/></svg><p class=\"name\">" +
                name +
                "</p><p>PDF document</p><a target=\"_top\" href=\"" +
                base::EscapeForHTML(url.spec()) + "\">Open PDF</a></main>";
  return base::StringPrintf(
      "<!doctype html><html><head><meta charset=\"utf-8\"><meta "
      "name=\"color-scheme\" content=\"%s\"><title>%s</title><style>html, "
      "body { height: 100%%; margin: 0 } html { background: %s; color: %s; "
      "font: 13px -apple-system, BlinkMacSystemFont, sans-serif } body { "
      "display: flex; align-items: center; justify-content: center } main { "
      "display: flex; flex-direction: column; align-items: center; gap: 4px; "
      "padding: 16px; max-width: 90%%; text-align: center } svg { width: "
      "28px; height: 28px; fill: none; stroke: currentColor; stroke-width: "
      "1.5; stroke-linecap: round; stroke-linejoin: round; opacity: .6; "
      "margin-bottom: 4px } p { margin: 0; opacity: .65 } .name { opacity: 1; "
      "font-weight: 600; overflow-wrap: anywhere } a { margin-top: 10px; "
      "padding: 5px 14px; border-radius: 6px; border: 1px solid %s; color: "
      "inherit; text-decoration: none; font-weight: 500 } a:hover { "
      "background: %s }</style></head><body>%s</body></html>",
      dark ? "dark" : "light", name.c_str(), CssColor(background).c_str(),
      dark ? "#e6e6e6" : "#1f1f1f",
      dark ? "rgba(255,255,255,.22)" : "rgba(0,0,0,.18)",
      dark ? "rgba(255,255,255,.08)" : "rgba(0,0,0,.05)", body.c_str());
}

void Tab::StartPdf(int64_t navigation_id,
                   const GURL& url,
                   const std::string& file_name,
                   int64_t total_bytes,
                   mojo::PendingRemote<network::mojom::URLLoader> loader,
                   mojo::PendingReceiver<network::mojom::URLLoaderClient> client,
                   mojo::ScopedDataPipeConsumerHandle body) {
  Engine* engine = GetEngine();
  const uint64_t id = engine->next_pdf_id++;
  // A folder of its own, so the file keeps the PDF's name.
  const base::FilePath path =
      engine->pdf_directory
          .AppendASCII(base::StringPrintf("%llu-%llu",
                                          static_cast<unsigned long long>(id_),
                                          static_cast<unsigned long long>(id)))
          .Append(file_name);
  Pdf pdf;
  pdf.file_name = file_name;
  pdf.saver = std::make_unique<PdfSaver>(
      id, url, path, total_bytes, std::move(loader), std::move(client),
      std::move(body),
      base::BindRepeating(&Tab::NotifyPdf, base::Unretained(this)));
  PdfSaver* saver = pdf.saver.get();
  pdfs_.emplace(id, std::move(pdf));
  pdf_navigations_[navigation_id] = id;
  NotifyPdf(*saver, NE_PDF_STARTED);
}

void Tab::NotifyPdf(const PdfSaver& saver, ne_pdf_event_kind kind) {
  Engine* engine = GetEngine();
  if (!engine->has_ui_delegate || !engine->ui_delegate.on_pdf) {
    return;
  }
  auto pdf = pdfs_.find(saver.id());
  const std::string file_name =
      pdf != pdfs_.end() ? pdf->second.file_name : std::string();
  const std::string url = saver.url().spec();
  const std::string path = saver.path().value();
  ne_pdf_event event = {sizeof(ne_pdf_event)};
  event.kind = kind;
  event.pdf_id = saver.id();
  event.state = saver.state();
  event.received_bytes = saver.received_bytes();
  event.total_bytes = saver.total_bytes();
  event.url = {url.data(), url.size()};
  event.path = {path.data(), path.size()};
  event.file_name = {file_name.data(), file_name.size()};
  engine->ui_delegate.on_pdf(engine->ui_delegate.user_data, handle_, &event);
}

void Tab::PdfDocumentCommitted(uint64_t shown_pdf) {
  if (shown_pdf) {
    auto pdf = pdfs_.find(shown_pdf);
    if (pdf != pdfs_.end()) {
      NotifyPdf(*pdf->second.saver, NE_PDF_COMMITTED);
    }
  }
  // The other PDFs are no longer shown: stop saving them (their files stay
  // for the app to clear) and forget them.
  for (auto it = pdfs_.begin(); it != pdfs_.end();) {
    if (it->first == shown_pdf) {
      ++it;
      continue;
    }
    if (!it->second.saver->finished()) {
      it->second.saver->Cancel();
      ++it;
      continue;
    }
    it = pdfs_.erase(it);
  }
  for (auto it = pdf_navigations_.begin(); it != pdf_navigations_.end();) {
    it = pdfs_.contains(it->second) ? std::next(it)
                                    : pdf_navigations_.erase(it);
  }
}

void Tab::TitleWasSet(content::NavigationEntry* entry) {
  NotifyNavigationState();
}

void Tab::DidStopLoading() {
  NotifyNavigationState();
  // A page that never paints contentful content shows once loaded, after a
  // moment for its paint, which often comes just after the load.
  if (holding_ && (hold_committed_ || !hold_navigation_)) {
    hold_loaded_at_ = base::TimeTicks::Now();
    hold_load_grace_.Start(FROM_HERE, kHoldLoadGrace,
                           base::BindOnce(&Tab::MaybeEndHoldAfterLoad,
                                          base::Unretained(this)));
  }
}

void Tab::RunJavaScriptDialog(content::WebContents* web_contents,
                              content::RenderFrameHost* render_frame_host,
                              content::JavaScriptDialogType dialog_type,
                              const std::u16string& message_text,
                              const std::u16string& default_prompt_text,
                              DialogClosedCallback callback,
                              bool* did_suppress_message) {
  std::string origin = render_frame_host->GetLastCommittedOrigin().Serialize();
  std::string message = base::UTF16ToUTF8(message_text);
  std::string default_text = base::UTF16ToUTF8(default_prompt_text);
  ne_ui_request request = {sizeof(ne_ui_request)};
  request.kind = dialog_type == content::JAVASCRIPT_DIALOG_TYPE_ALERT
                     ? NE_UI_JS_ALERT
                 : dialog_type == content::JAVASCRIPT_DIALOG_TYPE_CONFIRM
                     ? NE_UI_JS_CONFIRM
                     : NE_UI_JS_PROMPT;
  request.tab = handle_;
  request.origin = {origin.data(), origin.size()};
  request.message = {message.data(), message.size()};
  request.detail = {default_text.data(), default_text.size()};
  GetEngine()->SendUiRequest(
      request, this,
      base::BindOnce(
          [](DialogClosedCallback callback, const ne_ui_response* response) {
            bool accept = response && response->accept;
            std::string text =
                response ? ToString(response->text) : std::string();
            std::move(callback).Run(accept, base::UTF8ToUTF16(text));
          },
          std::move(callback)));
}

void Tab::RunBeforeUnloadDialog(content::WebContents* web_contents,
                                content::RenderFrameHost* render_frame_host,
                                bool is_reload,
                                DialogClosedCallback callback) {
  std::string origin = render_frame_host->GetLastCommittedOrigin().Serialize();
  ne_ui_request request = {sizeof(ne_ui_request)};
  request.kind = NE_UI_JS_BEFOREUNLOAD;
  request.tab = handle_;
  request.origin = {origin.data(), origin.size()};
  GetEngine()->SendUiRequest(
      request, this,
      base::BindOnce(
          [](DialogClosedCallback callback, const ne_ui_response* response) {
            std::move(callback).Run(response && response->accept,
                                    std::u16string());
          },
          std::move(callback)));
}

void Tab::CancelDialogs(content::WebContents* web_contents, bool reset_state) {
  GetEngine()->CancelRequestsFor(this);
}

}  // namespace notolibra
