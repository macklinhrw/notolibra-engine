// Copyright 2026 Notolibra. All rights reserved.
//
// The off-screen view's stand-in for RenderWidgetHostViewMac's NSView
// (plan.md §2.5). A tab's page renders through Chromium's own Mac view, with
// its compositor, display link and input conversions, but that view's NSView
// is never in a window. Chromium talks to the NSView through one interface,
// remote_cocoa::mojom::RenderWidgetHostNSView (the seam it uses for NSViews
// in app shims); RenderWidgetHostViewMac::SetEmbedderNSView (patches/
// embedder-ns-view) puts this bridge on it. The bridge turns the calls that
// would show native UI into data for the app (cursors, <select> popups,
// tooltips, text input state for input methods) and passes the rest on to
// the in-process NSView. Input methods go the other way, to the view's IME
// calls.

#ifndef NOTOLIBRA_ENGINE_VIEW_BRIDGE_H_
#define NOTOLIBRA_ENGINE_VIEW_BRIDGE_H_

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "content/common/render_widget_host_ns_view.mojom.h"
#include "content/public/browser/render_frame_metadata_provider.h"
#include "content/public/browser/render_widget_host_observer.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ne_engine.h"
#include "ui/base/ime/text_input_type.h"
#include "ui/gfx/range/range.h"

namespace content {
class RenderWidgetHost;
class RenderWidgetHostViewMac;
}  // namespace content

namespace notolibra {

class Tab;

class ViewBridge : public remote_cocoa::mojom::RenderWidgetHostNSView,
                   public content::RenderWidgetHostObserver,
                   public content::RenderFrameMetadataProvider::Observer {
 public:
  // Installs itself on `view`, which `host` owns.
  ViewBridge(Tab* tab,
             content::RenderWidgetHost* host,
             content::RenderWidgetHostViewMac* view);
  ViewBridge(const ViewBridge&) = delete;
  ViewBridge& operator=(const ViewBridge&) = delete;
  // Gives the view its own NSView back, if the view is still alive.
  ~ViewBridge() override;

  content::RenderWidgetHostViewMac* view() const { return view_; }
  content::RenderWidgetHost* host() const { return host_; }
  // The view went away while its widget stayed (a crashed renderer's):
  // forget it without calling it.
  void ForgetView();
  bool alive() const { return view_ != nullptr; }
  ui::TextInputType text_input_type() const { return text_input_type_; }

  // Input methods (ne_tab_ime_*).
  void ImeSetComposition(const std::u16string& text,
                         int selection_start,
                         int selection_end,
                         const gfx::Range& replacement);
  void ImeCommit(const std::u16string& text, const gfx::Range& replacement);
  void ImeFinish();
  void ImeCancel();
  bool CaretRect(ne_rect* out_rect);

  // Focus as AppKit would report it for a view in the key window: the
  // window is key and the view first responder (or neither). This is what
  // gives the page focus (document.hasFocus()) and input methods a target.
  void SetFocus(bool focused);

  // remote_cocoa::mojom::RenderWidgetHostNSView:
  void InitAsPopup(const gfx::Rect& content_rect,
                   uint64_t popup_parent_ns_view_id) override;
  void SetParentWebContentsNSView(uint64_t parent_ns_view_id) override;
  void DisableDisplay() override;
  void MakeFirstResponder() override;
  void SetBounds(const gfx::Rect& rect) override;
  void SetCALayerParams(gfx::CALayerParams ca_layer_params) override;
  void SetBackgroundColor(uint32_t color) override;
  void SetVisible(bool visible) override;
  void SetTooltipText(const std::u16string& display_text) override;
  void SetTextInputState(ui::TextInputType text_input_type,
                         uint32_t flags) override;
  void SetTextSelection(const std::u16string& text,
                        uint64_t offset,
                        const gfx::Range& range) override;
  void SetCompositionRangeInfo(const gfx::Range& range) override;
  void CancelComposition() override;
  void SetShowingContextMenu(bool showing) override;
  void SetSupportsAutoFill(bool supports) override;
  void DisplayCursor(const ui::Cursor& cursor) override;
  void SetCursorLocked(bool locked) override;
  void SetCursorLockedUnacceleratedMovement(
      bool unaccelerated_movement) override;
  void ShowDictionaryOverlayForSelection() override;
  void ShowDictionaryOverlay(ui::mojom::AttributedStringPtr attributed_string,
                             const gfx::Point& baseline_point) override;
  void LockKeyboard(
      const std::optional<std::vector<uint32_t>>& dom_codes) override;
  void UnlockKeyboard() override;
  void ShowSharingServicePicker(
      const std::string& title,
      const std::string& text,
      const GURL& url,
      const std::vector<std::string>& file_paths,
      ShowSharingServicePickerCallback callback) override;
  void Destroy() override;
  void GestureScrollEventAck(std::unique_ptr<blink::WebCoalescedInputEvent> event,
                             bool consumed) override;
  void DidOverscroll(blink::mojom::DidOverscrollParamsPtr params) override;
  void DisplayPopupMenu(remote_cocoa::mojom::PopupMenuPtr menu,
                        DisplayPopupMenuCallback callback) override;

  // content::RenderWidgetHostObserver:
  void RenderWidgetHostDestroyed(content::RenderWidgetHost* host) override;

  // content::RenderFrameMetadataProvider::Observer (the renderer's frames,
  // for the tab's paint holding):
  void OnRenderFrameMetadataChangedBeforeActivation(
      const cc::RenderFrameMetadata& metadata) override {}
  void OnRenderFrameMetadataChangedAfterActivation(
      base::TimeTicks activation_time) override;
  void OnRenderFrameSubmission() override {}
  void OnLocalSurfaceIdChanged(
      const cc::RenderFrameMetadata& metadata) override {}

 private:
  class PopupRunner;

  // Whether this bridge's view is the tab's page on screen (not a page in
  // the back-forward cache).
  bool IsCurrent() const;
  void PopupHidden(ne_request_id id);
  // The widget's renderer-frame metadata (`host_` must be set).
  content::RenderFrameMetadataProvider* MetadataProvider() const;

  raw_ptr<Tab> tab_;
  raw_ptr<content::RenderWidgetHost> host_;
  raw_ptr<content::RenderWidgetHostViewMac> view_;
  // The in-process NSView's bridge, which gets every call not handled here.
  raw_ptr<remote_cocoa::mojom::RenderWidgetHostNSView> next_ = nullptr;
  int cursor_ = -1;
  ui::TextInputType text_input_type_ = ui::TEXT_INPUT_TYPE_NONE;
  gfx::Range composition_range_ = gfx::Range::InvalidRange();
  std::unique_ptr<PopupRunner> popup_runner_;
  base::WeakPtrFactory<ViewBridge> weak_factory_{this};
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_VIEW_BRIDGE_H_
