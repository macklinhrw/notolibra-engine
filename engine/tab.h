// Copyright 2026 Notolibra. All rights reserved.
//
// One page (ne_tab). Phase 0 renders windowless: the WebContents' NSView is
// never put in a window (as content_shell's headless web test mode does), and
// Viz's frame capturer delivers its frames as IOSurfaces, which the app shows
// in a pane (plan.md §2.5, off-screen compositing). Frames are held until the
// app releases them, so there is no copy. The plan's own
// RenderWidgetHostView (adapted from CEF's) replaces the windowless
// RenderWidgetHostViewMac later; the API doesn't change.

#ifndef NOTOLIBRA_ENGINE_TAB_H_
#define NOTOLIBRA_ENGINE_TAB_H_

#include <IOSurface/IOSurface.h>

#include <map>
#include <memory>
#include <string>

#include "base/apple/scoped_cftyperef.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "components/viz/host/client_frame_sink_video_capturer.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/file_select_listener.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "third_party/blink/public/mojom/choosers/file_chooser.mojom.h"
#include "third_party/blink/public/mojom/favicon/favicon_url.mojom.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_observer.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "ne_engine.h"
#include "services/viz/privileged/mojom/compositing/frame_sink_video_capture.mojom.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/range/range.h"

namespace content {
class BrowserContext;
class RenderWidgetHostView;
class WebContents;
}  // namespace content

namespace notolibra {

class ViewBridge;

// A replacement range from the API (-1, -1: none).
gfx::Range ImeRange(int32_t start, int32_t end);

class Tab : public content::WebContentsDelegate,
            public content::WebContentsObserver,
            public content::JavaScriptDialogManager,
            public viz::mojom::FrameSinkVideoConsumer {
 public:
  Tab(ne_tab* handle,
      content::BrowserContext* context,
      const ne_tab_config& config);
  // Adopts a page Chromium created (window.open, target=_blank).
  Tab(ne_tab* handle, std::unique_ptr<content::WebContents> contents);
  ~Tab() override;

  static Tab* FromWebContents(content::WebContents* contents);

  ne_tab* handle() { return handle_; }
  // ne_tab_id(): never reused.
  uint64_t id() const { return id_; }
  content::WebContents* web_contents() { return web_contents_.get(); }

  void Load(const std::string& url);
  void SetPresenter(const ne_presenter* presenter);
  void SetViewSize(double width, double height, double scale_factor);
  void ReleaseFrame(uint64_t frame_id);
  void SetVisible(bool visible);
  void SetFocus(bool focused);
  void SendInput(const ne_input_event& event);
  void Edit(ne_edit_command command);
  void Find(const std::string& text, uint32_t flags);
  void StopFinding(bool keep_selection);
  void Eval(const std::string& script,
            ne_eval_reason reason,
            ne_eval_fn done,
            void* user_data);
  void EvalInFrame(ne_frame_id frame,
                   const std::string& script,
                   ne_world world,
                   ne_eval_fn done,
                   void* user_data);
  void ForEachFrame(ne_frame_fn fn, void* user_data);

  // Input methods, through the page's view bridge.
  void ImeSetComposition(const std::u16string& text,
                         int selection_start,
                         int selection_end,
                         const gfx::Range& replacement);
  void ImeCommit(const std::u16string& text, const gfx::Range& replacement);
  void ImeFinish();
  void ImeCancel();
  bool ImeCaretRect(ne_rect* out_rect);

  // The page's view on screen (the primary main frame's), or null.
  content::RenderWidgetHostView* view();

  // From the view bridge (view_bridge.h), for the page on screen.
  void NotifyCursor(int kind);
  void NotifyTextInputState(int32_t type, uint32_t flags);
  void NotifyImeCancel();
  void NotifyTooltip(const std::string& text);
  // `bridge`'s widget is gone; the bridge is deleted soon.
  void ViewBridgeGone(ViewBridge* bridge);

  // content::WebContentsDelegate:
  void NavigationStateChanged(content::WebContents* source,
                              content::InvalidateTypes changed_flags) override;
  void LoadingStateChanged(content::WebContents* source,
                           bool should_show_loading_ui) override;
  content::WebContents* AddNewContents(
      content::WebContents* source,
      std::unique_ptr<content::WebContents> new_contents,
      const GURL& target_url,
      WindowOpenDisposition disposition,
      const blink::mojom::WindowFeatures& window_features,
      bool user_gesture,
      bool* was_blocked) override;
  void FindReply(content::WebContents* web_contents,
                 int request_id,
                 int number_of_matches,
                 const gfx::Rect& selection_rect,
                 int active_match_ordinal,
                 bool final_update) override;
  bool DidAddMessageToConsole(content::WebContents* source,
                              blink::mojom::ConsoleMessageLevel log_level,
                              const std::u16string& message,
                              int32_t line_no,
                              const std::u16string& source_id) override;
  content::JavaScriptDialogManager* GetJavaScriptDialogManager(
      content::WebContents* source) override;
  void RunFileChooser(content::RenderFrameHost* render_frame_host,
                      scoped_refptr<content::FileSelectListener> listener,
                      const blink::mojom::FileChooserParams& params) override;
  bool HandleContextMenu(content::RenderFrameHost& render_frame_host,
                         const content::ContextMenuParams& params) override;
  // Element fullscreen (a video's fullscreen button) fills the page's own
  // view, the app's pane, as content_shell's does its window; Escape leaves.
  void EnterFullscreenModeForTab(
      content::RenderFrameHost* requesting_frame,
      const blink::mojom::FullscreenOptions& options) override;
  void ExitFullscreenModeForTab(content::WebContents* web_contents) override;
  bool IsFullscreenForTabOrPending(
      const content::WebContents* web_contents) override;
  content::KeyboardEventProcessingResult PreHandleKeyboardEvent(
      content::WebContents* source,
      const input::NativeWebKeyboardEvent& event) override;

  // content::WebContentsObserver:
  void RenderViewReady() override;
  void RenderFrameHostChanged(content::RenderFrameHost* old_host,
                              content::RenderFrameHost* new_host) override;
  void PrimaryPageChanged(content::Page& page) override;
  void DidFirstVisuallyNonEmptyPaint() override;
  void TitleWasSet(content::NavigationEntry* entry) override;
  void DidStopLoading() override;
  void DidStartNavigation(
      content::NavigationHandle* navigation_handle) override;
  void ReadyToCommitNavigation(
      content::NavigationHandle* navigation_handle) override;
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;
  void DidFinishLoad(content::RenderFrameHost* render_frame_host,
                     const GURL& validated_url) override;
  void OnBackgroundColorChanged() override;
  void DidUpdateFaviconURL(
      content::RenderFrameHost* render_frame_host,
      const std::vector<blink::mojom::FaviconURLPtr>& candidates,
      blink::mojom::FaviconUpdateReason reason) override;
  void OnDidAddMessageToConsole(
      content::RenderFrameHost* source_frame,
      blink::mojom::ConsoleMessageLevel log_level,
      const std::u16string& message,
      int32_t line_no,
      const std::u16string& source_id,
      const std::optional<std::u16string>& untrusted_stack_trace) override;

  // content::JavaScriptDialogManager:
  void RunJavaScriptDialog(content::WebContents* web_contents,
                           content::RenderFrameHost* render_frame_host,
                           content::JavaScriptDialogType dialog_type,
                           const std::u16string& message_text,
                           const std::u16string& default_prompt_text,
                           DialogClosedCallback callback,
                           bool* did_suppress_message) override;
  void RunBeforeUnloadDialog(content::WebContents* web_contents,
                             content::RenderFrameHost* render_frame_host,
                             bool is_reload,
                             DialogClosedCallback callback) override;
  void CancelDialogs(content::WebContents* web_contents,
                     bool reset_state) override;

  // viz::mojom::FrameSinkVideoConsumer:
  void OnFrameCaptured(
      media::mojom::VideoBufferHandlePtr data,
      media::mojom::VideoFrameInfoPtr info,
      const gfx::Rect& content_rect,
      mojo::PendingRemote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks>
          callbacks) override;
  void OnNewCaptureVersion(
      const media::CaptureVersion& capture_version) override {}
  void OnFrameWithEmptyRegionCapture() override {}
  void OnStopped() override {}
  void OnLog(const std::string& message) override;

 private:
  struct HeldFrame {
    base::apple::ScopedCFTypeRef<IOSurfaceRef> surface;
    mojo::Remote<viz::mojom::FrameSinkVideoConsumerFrameCallbacks> callbacks;
  };

  // (Re)starts capture if the page's view changed (cross-process navigations
  // swap it) and a presenter is set.
  // Shared by both constructors.
  void Init();
  void RunContextCommand(int command,
                         content::GlobalRenderFrameHostId frame,
                         const content::ContextMenuParams& params);
  void FaviconDownloaded(int id,
                         int http_status_code,
                         const GURL& image_url,
                         const std::vector<SkBitmap>& bitmaps,
                         const std::vector<gfx::Size>& sizes);
  void EnsureCapture();
  void ApplySize();
  void NotifyNavigationState();
  void NotifyLoadEvent(content::RenderFrameHost* frame,
                       ne_load_event_kind kind,
                       const GURL& url);
  // Puts a view bridge on the page's view if it has none yet.
  void EnsureViewBridge();
  ViewBridge* current_bridge();
  // The frame `id` names (0: the main frame), or null.
  content::RenderFrameHost* FindFrame(ne_frame_id id);

  raw_ptr<ne_tab> handle_;
  const uint64_t id_;
  std::unique_ptr<content::WebContents> web_contents_;
  bool fullscreen_ = false;
  ne_presenter presenter_ = {};
  bool has_presenter_ = false;
  gfx::Size size_{800, 600};
  double scale_factor_ = 2.0;

  std::unique_ptr<viz::ClientFrameSinkVideoCapturer> capturer_;
  raw_ptr<content::RenderWidgetHostView> captured_view_ = nullptr;
  base::ScopedClosureRunner capture_handle_;
  uint64_t next_frame_id_ = 1;
  std::map<uint64_t, HeldFrame> held_frames_;
  bool logged_unexpected_buffer_ = false;
  // One bridge per page view still alive (pages in the back-forward cache
  // keep theirs), by widget.
  std::map<content::RenderWidgetHost*, std::unique_ptr<ViewBridge>> bridges_;
  // Whether the app consumed the console message the observer saw last; the
  // delegate's DidAddMessageToConsole, called right after, returns it.
  bool console_consumed_ = false;

  // Wheel events without phases (a mouse wheel) get Began/Changed phases
  // and an Ended one after a pause, as RenderWidgetHostViewMac's
  // MouseWheelPhaseHandler adds them; Chromium's Mac scrolling needs them.
  bool wheel_in_progress_ = false;
  gfx::PointF last_wheel_position_;
  base::OneShotTimer wheel_end_timer_;
  void EndWheel();

  base::WeakPtrFactory<Tab> weak_factory_{this};
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_TAB_H_
