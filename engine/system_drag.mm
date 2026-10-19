// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/system_drag.h"

#import <AppKit/AppKit.h>

#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "content/browser/renderer_host/render_widget_host_view_base.h"
#include "content/public/browser/clipboard_types.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host.h"
#include "content/public/browser/web_contents.h"
#include "notolibra/engine/tab.h"
#include "ui/base/dragdrop/mojom/drag_drop_types.mojom.h"
#include "ui/gfx/geometry/point_f.h"

namespace notolibra {

namespace {

// How far outside a window's frame the pointer may be when the drag
// starts: the renderer starts a drag a few points after the press, and the
// message takes a moment, so a fast drag from a page's edge can already
// have left the window.
constexpr CGFloat kSlop = 48;

}  // namespace

bool MayStartSystemDrag() {
  // A person dragging holds the button (a trackpad's tap-drag and
  // three-finger drag hold it too); a check's synthetic events don't.
  if (!(NSEvent.pressedMouseButtons & 1)) {
    return false;
  }
  const NSPoint pointer = NSEvent.mouseLocation;
  for (NSWindow* window in NSApp.windows) {
    if (!window.visible || window.screen == nil || window.alphaValue <= 0 ||
        window.ignoresMouseEvents ||
        !(window.occlusionState & NSWindowOcclusionStateVisible)) {
      continue;
    }
    if (NSPointInRect(pointer, NSInsetRect(window.frame, -kSlop, -kSlop))) {
      return true;
    }
  }
  return false;
}

void EndRefusedDrag(const content::ClipboardEndpoint& source) {
  content::RenderFrameHost* frame = source.render_frame_host();
  content::WebContents* contents = source.web_contents();
  if (!frame || !contents) {
    return;
  }
  content::RenderWidgetHost* widget = frame->GetRenderWidgetHost();
  // Where the pointer last was in the page, in the source widget's
  // coordinates, as WebContentsViewMac::PerformEndDrag transforms them.
  Tab* tab = Tab::FromWebContents(contents);
  gfx::PointF point = tab ? tab->last_pointer() : gfx::PointF();
  gfx::PointF transformed = point;
  auto* root = static_cast<content::RenderWidgetHostViewBase*>(
      contents->GetRenderWidgetHostView());
  auto* view =
      static_cast<content::RenderWidgetHostViewBase*>(widget->GetView());
  if (root && view) {
    root->TransformPointToCoordSpaceForView(point, view, &transformed);
  }
  LOG(WARNING) << "Refused a page's system drag: the button isn't held over "
                  "a window of the app on screen";
  // As WebContentsImpl::DragSourceEndedAt sends it.
  widget->DragSourceEndedAt(transformed, transformed,
                            ui::mojom::DragOperation::kNone,
                            base::DoNothing());
}

}  // namespace notolibra
