// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/view_bridge.h"

#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "content/browser/renderer_host/render_widget_host_view_mac.h"
#include "content/public/browser/render_widget_host.h"
#include "notolibra/engine/engine.h"
#include "notolibra/engine/tab.h"
#include "third_party/blink/public/common/input/web_coalesced_input_event.h"
#include "third_party/blink/public/mojom/choosers/popup_menu.mojom.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/cursor/mojom/cursor_type.mojom-shared.h"
#include "ui/base/ime/ime_text_span.h"

namespace notolibra {

// The header's cursor kinds are Chromium's values.
static_assert(NE_CURSOR_POINTER == static_cast<int>(ui::mojom::CursorType::kPointer));
static_assert(NE_CURSOR_HAND == static_cast<int>(ui::mojom::CursorType::kHand));
static_assert(NE_CURSOR_IBEAM == static_cast<int>(ui::mojom::CursorType::kIBeam));
static_assert(NE_CURSOR_ROW_RESIZE == static_cast<int>(ui::mojom::CursorType::kRowResize));
static_assert(NE_CURSOR_MOVE == static_cast<int>(ui::mojom::CursorType::kMove));
static_assert(NE_CURSOR_GRABBING == static_cast<int>(ui::mojom::CursorType::kGrabbing));
static_assert(NE_CURSOR_CUSTOM == static_cast<int>(ui::mojom::CursorType::kCustom));
static_assert(static_cast<int>(NE_TEXT_INPUT_PASSWORD) ==
              static_cast<int>(ui::TEXT_INPUT_TYPE_PASSWORD));

// The page's handle on an open <select> popup: Hide() withdraws it.
class ViewBridge::PopupRunner : public remote_cocoa::mojom::PopupMenuRunner {
 public:
  PopupRunner(ViewBridge* bridge,
              ne_request_id id,
              mojo::PendingReceiver<remote_cocoa::mojom::PopupMenuRunner> receiver)
      : bridge_(bridge), id_(id), receiver_(this, std::move(receiver)) {}

  ne_request_id id() const { return id_; }

  // remote_cocoa::mojom::PopupMenuRunner:
  void Hide() override { bridge_->PopupHidden(id_); }

 private:
  raw_ptr<ViewBridge> bridge_;
  ne_request_id id_;
  mojo::Receiver<remote_cocoa::mojom::PopupMenuRunner> receiver_;
};

ViewBridge::ViewBridge(Tab* tab,
                       content::RenderWidgetHost* host,
                       content::RenderWidgetHostViewMac* view)
    : tab_(tab), host_(host), view_(view) {
  next_ = view_->SetEmbedderNSView(this);
  if (!next_) {
    LOG(ERROR) << "view bridge: the view refused an embedder NSView";
    view_ = nullptr;
    host_ = nullptr;
    return;
  }
  host_->AddObserver(this);
}

ViewBridge::~ViewBridge() {
  if (popup_runner_) {
    GetEngine()->CancelRequest(popup_runner_->id());
  }
  if (host_) {
    host_->RemoveObserver(this);
  }
  if (view_) {
    view_->SetEmbedderNSView(nullptr);
  }
}

void ViewBridge::RenderWidgetHostDestroyed(content::RenderWidgetHost* host) {
  // The view has shut down (its NSView bridge is gone): nothing is called
  // on it, or by it, any more.
  host_->RemoveObserver(this);
  host_ = nullptr;
  view_ = nullptr;
  next_ = nullptr;
  tab_->ViewBridgeGone(this);  // May delete |this| later.
}

bool ViewBridge::IsCurrent() const {
  return view_ && tab_->view() ==
                      static_cast<content::RenderWidgetHostView*>(view_.get());
}

void ViewBridge::SetFocus(bool focused) {
  if (!view_) {
    return;
  }
  if (focused) {
    view_->OnWindowIsKeyChanged(true);
    view_->OnFirstResponderChanged(true);
  } else {
    view_->OnFirstResponderChanged(false);
    view_->OnWindowIsKeyChanged(false);
  }
}

// Input methods ------------------------------------------------------------

void ViewBridge::ImeSetComposition(const std::u16string& text,
                                   int selection_start,
                                   int selection_end,
                                   const gfx::Range& replacement) {
  if (!view_) {
    return;
  }
  // One thin underline under the whole composition, as AppKit's views draw
  // marked text.
  std::vector<ui::ImeTextSpan> spans;
  spans.emplace_back(ui::ImeTextSpan::Type::kComposition, 0, text.size(),
                     ui::ImeTextSpan::Thickness::kThin,
                     ui::ImeTextSpan::UnderlineStyle::kSolid);
  VLOG(1) << "IME: composition " << text.size() << " units, input type "
          << text_input_type_;
  view_->ImeSetComposition(text, spans, replacement, selection_start,
                           selection_end);
}

void ViewBridge::ImeCommit(const std::u16string& text,
                           const gfx::Range& replacement) {
  if (view_) {
    view_->ImeCommitText(text, replacement);
  }
}

void ViewBridge::ImeFinish() {
  if (view_) {
    view_->ImeFinishComposingText();
  }
}

void ViewBridge::ImeCancel() {
  if (view_) {
    view_->ImeCancelCompositionFromCocoa();
  }
}

bool ViewBridge::CaretRect(ne_rect* out_rect) {
  if (!view_ || !out_rect) {
    return false;
  }
  // The composition's start, else the caret (an empty range at the
  // selection, which the view resolves).
  gfx::Range range = composition_range_.IsValid()
                         ? gfx::Range(composition_range_.start())
                         : gfx::Range::InvalidRange();
  gfx::Rect rect;
  gfx::Range actual;
  bool success = false;
  view_->SyncGetFirstRectForRange(range, &rect, &actual, &success);
  if (!success) {
    return false;
  }
  // The view reports screen coordinates; ours has no window, so its view
  // origin is its "screen" origin.
  *out_rect = {static_cast<double>(rect.x()), static_cast<double>(rect.y()),
               static_cast<double>(rect.width()),
               static_cast<double>(rect.height())};
  return true;
}

// remote_cocoa::mojom::RenderWidgetHostNSView ------------------------------

void ViewBridge::InitAsPopup(const gfx::Rect& content_rect,
                             uint64_t popup_parent_ns_view_id) {
  next_->InitAsPopup(content_rect, popup_parent_ns_view_id);
}

void ViewBridge::SetParentWebContentsNSView(uint64_t parent_ns_view_id) {
  next_->SetParentWebContentsNSView(parent_ns_view_id);
}

void ViewBridge::DisableDisplay() {
  next_->DisableDisplay();
}

void ViewBridge::MakeFirstResponder() {
  next_->MakeFirstResponder();
}

void ViewBridge::SetBounds(const gfx::Rect& rect) {
  next_->SetBounds(rect);
}

void ViewBridge::SetCALayerParams(gfx::CALayerParams ca_layer_params) {
  // With remote Core Animation off (the engine's default), each display
  // frame arrives as one IOSurface: the app shows it, so the in-process
  // NSView, never on screen, isn't given it. A page in the back-forward
  // cache shows nothing.
  if (ca_layer_params.io_surface_mach_port) {
    if (IsCurrent()) {
      tab_->OnCompositorFrame(ca_layer_params);
    }
    return;
  }
  // Null while the view hands its last frame to a new bridge (the in-process
  // view has that one already).
  if (next_) {
    next_->SetCALayerParams(std::move(ca_layer_params));
  }
}

void ViewBridge::SetBackgroundColor(uint32_t color) {
  next_->SetBackgroundColor(color);
}

void ViewBridge::SetVisible(bool visible) {
  next_->SetVisible(visible);
}

void ViewBridge::SetTooltipText(const std::u16string& display_text) {
  // Drawn by the app, never as an AppKit tooltip.
  if (IsCurrent()) {
    tab_->NotifyTooltip(base::UTF16ToUTF8(display_text));
  }
}

void ViewBridge::SetTextInputState(ui::TextInputType text_input_type,
                                   uint32_t flags) {
  text_input_type_ = text_input_type;
  if (IsCurrent()) {
    tab_->NotifyTextInputState(static_cast<int32_t>(text_input_type), flags);
  }
  next_->SetTextInputState(text_input_type, flags);
}

void ViewBridge::SetTextSelection(const std::u16string& text,
                                  uint64_t offset,
                                  const gfx::Range& range) {
  next_->SetTextSelection(text, offset, range);
}

void ViewBridge::SetCompositionRangeInfo(const gfx::Range& range) {
  composition_range_ = range;
  next_->SetCompositionRangeInfo(range);
}

void ViewBridge::CancelComposition() {
  composition_range_ = gfx::Range::InvalidRange();
  if (IsCurrent()) {
    tab_->NotifyImeCancel();
  }
  next_->CancelComposition();
}

void ViewBridge::SetShowingContextMenu(bool showing) {
  next_->SetShowingContextMenu(showing);
}

void ViewBridge::SetSupportsAutoFill(bool supports) {
  next_->SetSupportsAutoFill(supports);
}

void ViewBridge::DisplayCursor(const ui::Cursor& cursor) {
  // The app shows the cursor; the hidden NSView would do nothing with it.
  int kind = static_cast<int>(cursor.type());
  if (kind < 0) {
    kind = NE_CURSOR_POINTER;
  }
  if (kind == cursor_ || !IsCurrent()) {
    return;
  }
  cursor_ = kind;
  tab_->NotifyCursor(kind);
}

void ViewBridge::SetCursorLocked(bool locked) {
  next_->SetCursorLocked(locked);
}

void ViewBridge::SetCursorLockedUnacceleratedMovement(
    bool unaccelerated_movement) {
  next_->SetCursorLockedUnacceleratedMovement(unaccelerated_movement);
}

void ViewBridge::ShowDictionaryOverlayForSelection() {
  // AppKit's Look Up panel is a window of its own: not shown off screen.
}

void ViewBridge::ShowDictionaryOverlay(
    ui::mojom::AttributedStringPtr attributed_string,
    const gfx::Point& baseline_point) {}

void ViewBridge::LockKeyboard(
    const std::optional<std::vector<uint32_t>>& dom_codes) {
  next_->LockKeyboard(dom_codes);
}

void ViewBridge::UnlockKeyboard() {
  next_->UnlockKeyboard();
}

void ViewBridge::ShowSharingServicePicker(
    const std::string& title,
    const std::string& text,
    const GURL& url,
    const std::vector<std::string>& file_paths,
    ShowSharingServicePickerCallback callback) {
  // AppKit's share picker is a window of its own: not shown off screen.
  std::move(callback).Run(blink::mojom::ShareError::CANCELED);
}

void ViewBridge::Destroy() {
  next_->Destroy();
}

void ViewBridge::GestureScrollEventAck(
    std::unique_ptr<blink::WebCoalescedInputEvent> event,
    bool consumed) {
  next_->GestureScrollEventAck(std::move(event), consumed);
}

void ViewBridge::DidOverscroll(blink::mojom::DidOverscrollParamsPtr params) {
  next_->DidOverscroll(std::move(params));
}

void ViewBridge::DisplayPopupMenu(remote_cocoa::mojom::PopupMenuPtr menu,
                                  DisplayPopupMenuCallback callback) {
  // A <select>'s list, as data: the app draws it and answers with an index.
  if (popup_runner_) {
    GetEngine()->CancelRequest(popup_runner_->id());
    popup_runner_.reset();
  }
  std::vector<std::string> labels;
  std::vector<std::string> tooltips;
  labels.reserve(menu->items.size());
  tooltips.reserve(menu->items.size());
  for (const auto& item : menu->items) {
    labels.push_back(item->label ? *item->label : std::string());
    tooltips.push_back(item->tool_tip ? *item->tool_tip : std::string());
  }
  std::vector<ne_menu_item> items;
  items.reserve(menu->items.size());
  for (size_t i = 0; i < menu->items.size(); ++i) {
    const auto& item = menu->items[i];
    ne_menu_item out = {};
    out.command_id = static_cast<int32_t>(i);
    out.label = {labels[i].data(), labels[i].size()};
    out.tooltip = {tooltips[i].data(), tooltips[i].size()};
    out.enabled = item->enabled;
    out.checked = item->checked;
    out.submenu_index = -1;
    switch (item->type) {
      case blink::mojom::MenuItem::Type::kSeparator:
        out.kind = NE_MENU_ITEM_SEPARATOR;
        out.command_id = -1;
        break;
      case blink::mojom::MenuItem::Type::kGroup:
        out.kind = NE_MENU_ITEM_GROUP;
        break;
      case blink::mojom::MenuItem::Type::kCheckableOption:
        out.kind = NE_MENU_ITEM_CHECKABLE;
        break;
      case blink::mojom::MenuItem::Type::kSubMenu:
        out.kind = NE_MENU_ITEM_SUBMENU;
        break;
      default:
        out.kind = NE_MENU_ITEM_COMMAND;
        break;
    }
    items.push_back(out);
  }
  ne_ui_request request = {sizeof(ne_ui_request)};
  request.kind = NE_UI_SELECT_POPUP;
  request.tab = tab_->handle();
  request.anchor = {static_cast<double>(menu->bounds.x()),
                    static_cast<double>(menu->bounds.y()),
                    static_cast<double>(menu->bounds.width()),
                    static_cast<double>(menu->bounds.height())};
  request.items = items.data();
  request.item_count = items.size();
  request.selected_index = static_cast<int32_t>(menu->selected_item);
  const size_t count = items.size();
  auto receiver = std::move(menu->receiver);
  ne_request_id id = GetEngine()->SendUiRequest(
      request, tab_,
      base::BindOnce(
          [](base::WeakPtr<ViewBridge> self, size_t count,
             DisplayPopupMenuCallback callback,
             const ne_ui_response* response) {
            if (self) {
              self->popup_runner_.reset();
            }
            if (response && response->accept && response->command_id >= 0 &&
                static_cast<size_t>(response->command_id) < count) {
              std::move(callback).Run(
                  static_cast<uint32_t>(response->command_id));
            } else {
              std::move(callback).Run(std::nullopt);
            }
          },
          weak_factory_.GetWeakPtr(), count, std::move(callback)));
  // Still open (the app may have answered at once).
  if (GetEngine()->IsPending(id)) {
    popup_runner_ = std::make_unique<PopupRunner>(this, id, std::move(receiver));
  }
}

void ViewBridge::PopupHidden(ne_request_id id) {
  // The page closed its <select> (it lost focus, or navigated). Not from
  // inside the runner's own call: answering deletes the runner.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](ne_request_id id) {
                       if (Engine* engine = GetEngine()) {
                         engine->CancelRequest(id);
                       }
                     },
                     id));
}

}  // namespace notolibra
