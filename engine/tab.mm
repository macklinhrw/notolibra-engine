// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/tab.h"

#import <AppKit/AppKit.h>

#include <algorithm>
#include <utility>

#include "base/apple/owned_objc.h"
#include "base/json/json_writer.h"
#include "base/compiler_specific.h"
#include "base/logging.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "components/input/native_web_keyboard_event.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/isolated_world_ids.h"
#include "media/capture/mojom/video_capture_buffer.mojom.h"
#include "media/capture/mojom/video_capture_types.mojom.h"
#include "notolibra/engine/engine.h"
#include "third_party/blink/public/common/input/web_input_event.h"
#include "third_party/blink/public/common/input/web_mouse_event.h"
#include "third_party/blink/public/common/input/web_mouse_wheel_event.h"
#include "third_party/blink/public/common/page/page_zoom.h"
#include "third_party/blink/public/mojom/frame/find_in_page.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size_conversions.h"

namespace notolibra {

namespace {

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

}  // namespace

Tab::Tab(ne_tab* handle,
         content::BrowserContext* context,
         const ne_tab_config& config)
    : handle_(handle) {
  content::WebContents::CreateParams params(context);
  params.initially_hidden = false;
  web_contents_ = content::WebContents::Create(params);
  web_contents_->SetDelegate(this);
  Observe(web_contents_.get());
  if (config.background_color_argb) {
    web_contents_->SetPageBaseBackgroundColor(config.background_color_argb);
  }
  GetEngine()->tabs.push_back(this);
  // Renderers keep producing frames although no window shows them
  // ("being captured" disables hidden-rendering optimisations).
  capture_handle_ = web_contents_->IncrementCapturerCount(
      gfx::Size(), /*stay_hidden=*/false, /*stay_awake=*/false,
      /*is_activity=*/false);
  ApplySize();
  web_contents_->WasShown();
  std::string url = ToString(config.url);
  Load(url.empty() ? "about:blank" : url);
}

Tab::~Tab() {
  GetEngine()->CancelRequestsFor(this);
  if (capturer_) {
    capturer_->StopAndResetConsumer();
  }
  capturer_.reset();
  for (auto& [id, frame] : held_frames_) {
    frame.callbacks->Done();
  }
  held_frames_.clear();
  capture_handle_.RunAndReset();
  Observe(nullptr);
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

void Tab::EnsureCapture() {
  content::RenderWidgetHostView* rwhv = view();
  if (!rwhv || !has_presenter_) {
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
  frame.surface.reset(surface, base::scoped_policy::RETAIN);

  const uint64_t id = next_frame_id_++;
  ne_frame out = {sizeof(ne_frame)};
  out.frame_id = id;
  out.kind = NE_SURFACE_IOSURFACE;
  out.surface = surface;
  out.pixel_width = info->coded_size.width();
  out.pixel_height = info->coded_size.height();
  out.scale_factor = view() ? view()->GetDeviceScaleFactor() : scale_factor_;
  out.content_rect = {static_cast<double>(content_rect.x()),
                      static_cast<double>(content_rect.y()),
                      static_cast<double>(content_rect.width()),
                      static_cast<double>(content_rect.height())};
  out.capture_time_us = info->timestamp.InMicroseconds();
  held_frames_.emplace(id, std::move(frame));
  presenter_.on_frame(presenter_.user_data, &out);
}

void Tab::ReleaseFrame(uint64_t frame_id) {
  auto it = held_frames_.find(frame_id);
  if (it == held_frames_.end()) {
    return;
  }
  it->second.callbacks->Done();
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
  content::RenderWidgetHost* host = rwhv->GetRenderWidgetHost();
  const base::TimeTicks now = base::TimeTicks::Now();
  const gfx::PointF position(event.x, event.y);
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
      host->ForwardMouseEvent(mouse);
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
      if (event.phase) {
        wheel.phase = static_cast<blink::WebMouseWheelEvent::Phase>(event.phase);
      } else {
        wheel.phase = wheel_in_progress_ ? blink::WebMouseWheelEvent::kPhaseChanged
                                         : blink::WebMouseWheelEvent::kPhaseBegan;
        wheel_in_progress_ = true;
        last_wheel_position_ = position;
        wheel_end_timer_.Start(FROM_HERE, base::Milliseconds(100),
                               base::BindOnce(&Tab::EndWheel,
                                              weak_factory_.GetWeakPtr()));
      }
      host->ForwardWheelEvent(wheel);
      break;
    }
    case NE_INPUT_KEY_DOWN:
    case NE_INPUT_KEY_UP:
    case NE_INPUT_CHAR: {
      // Build the NSEvent AppKit would have sent, and let Chromium convert it
      // (key codes, DOM codes and keys) as RenderWidgetHostViewMac does.
      NSString* text = base::SysUTF8ToNSString(ToString(event.text));
      NSEvent* ns_event = [NSEvent
                    keyEventWithType:event.kind == NE_INPUT_KEY_UP ? NSEventTypeKeyUp
                                                                  : NSEventTypeKeyDown
                            location:NSZeroPoint
                       modifierFlags:NSModifiers(event.modifiers)
                           timestamp:[[NSProcessInfo processInfo] systemUptime]
                        windowNumber:0
                             context:nil
                          characters:text
         charactersIgnoringModifiers:text
                           isARepeat:NO
                             keyCode:static_cast<unsigned short>(
                                         event.native_key_code)];
      input::NativeWebKeyboardEvent key(gfx::NativeEvent{ns_event});
      if (event.kind == NE_INPUT_CHAR) {
        key.SetType(blink::WebInputEvent::Type::kChar);
      }
      host->ForwardKeyboardEvent(key);
      // A key press that inserts text also sends a Char event, as the Mac
      // view's -insertText: does. Command-key shortcuts don't.
      if (event.kind == NE_INPUT_KEY_DOWN && text.length > 0 &&
          !(event.modifiers & (2 | 8))) {
        input::NativeWebKeyboardEvent char_event(gfx::NativeEvent{ns_event});
        char_event.SetType(blink::WebInputEvent::Type::kChar);
        host->ForwardKeyboardEvent(char_event);
      }
      break;
    }
  }
}

void Tab::EndWheel() {
  wheel_in_progress_ = false;
  content::RenderWidgetHostView* rwhv = view();
  if (!rwhv) {
    return;
  }
  blink::WebMouseWheelEvent wheel(blink::WebInputEvent::Type::kMouseWheel,
                                  blink::WebInputEvent::kNoModifiers,
                                  base::TimeTicks::Now());
  wheel.SetPositionInWidget(last_wheel_position_);
  wheel.SetPositionInScreen(last_wheel_position_);
  wheel.phase = blink::WebMouseWheelEvent::kPhaseEnded;
  wheel.delta_units = ui::ScrollGranularity::kScrollByPrecisePixel;
  rwhv->GetRenderWidgetHost()->ForwardWheelEvent(wheel);
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
  auto callback = base::BindOnce(
      [](ne_eval_fn done, void* user_data, base::Value value) {
        if (!done) {
          return;
        }
        std::string json;
        base::JSONWriter::Write(value, &json);
        done(user_data, NE_OK, json.c_str());
      },
      done, user_data);
  if (reason == NE_EVAL_APP_FEATURE) {
    frame->ExecuteJavaScriptInIsolatedWorld(
        base::UTF8ToUTF16(script), std::move(callback),
        content::ISOLATED_WORLD_ID_CONTENT_END);
  } else {
    // Phase 0: the main world through the test entry point. A real
    // main-world path (with its own policy) replaces this.
    frame->ExecuteJavaScriptForTests(base::UTF8ToUTF16(script),
                                     std::move(callback),
                                     content::ISOLATED_WORLD_ID_GLOBAL);
  }
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
  // Phase 0: new windows become a request for the app (NE_UI_NEW_TAB) and are
  // not opened.
  std::string url = target_url.spec();
  ne_ui_request request = {sizeof(ne_ui_request)};
  request.kind = NE_UI_NEW_TAB;
  request.tab = handle_;
  request.message = {url.data(), url.size()};
  GetEngine()->SendUiRequest(request, this, base::DoNothing());
  if (was_blocked) {
    *was_blocked = true;
  }
  return nullptr;
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
  LOG(INFO) << "console: " << message;
  return false;
}

content::JavaScriptDialogManager* Tab::GetJavaScriptDialogManager(
    content::WebContents* source) {
  return this;
}

void Tab::RenderViewReady() {
  EnsureCapture();
  ApplySize();
}

void Tab::PrimaryPageChanged(content::Page& page) {
  EnsureCapture();
  ApplySize();
  NotifyNavigationState();
}

void Tab::DidFirstVisuallyNonEmptyPaint() {
  if (has_presenter_ && presenter_.on_first_paint) {
    std::optional<SkColor> color = web_contents_->GetBackgroundColor();
    presenter_.on_first_paint(presenter_.user_data,
                              color.value_or(SK_ColorWHITE));
  }
}

void Tab::TitleWasSet(content::NavigationEntry* entry) {
  NotifyNavigationState();
}

void Tab::DidStopLoading() {
  NotifyNavigationState();
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
