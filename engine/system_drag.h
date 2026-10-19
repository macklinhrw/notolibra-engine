// Copyright 2026 Notolibra. All rights reserved.
//
// Whether a page's drag may become a system drag. A page starts a drag (of
// selected text, a link, an image, a draggable element) by asking the
// browser to start one; on macOS, WebContentsViewMac starts an
// NSDraggingSession, and the window server draws the drag image on the
// screen. Our pages' views are never in a window (tab.h), so the session's
// image starts at the screen's origin, the bottom-left corner. In a hidden
// verification run (the app's window off-screen) nobody is dragging: on
// October 18, 2026 a check's synthetic selection drag drew a page thumbnail
// over Macklin's windows that way.
//
// BrowserClient::IsDragAllowedByPolicy asks MayStartSystemDrag() before the
// session starts; when it refuses, EndRefusedDrag() ends the page's drag as
// a cancelled one (dragend with no drop effect), and WebContentsViewMac
// then sends SystemDragEnded, as it does for drags it can't start.

#ifndef NOTOLIBRA_ENGINE_SYSTEM_DRAG_H_
#define NOTOLIBRA_ENGINE_SYSTEM_DRAG_H_

namespace content {
class ClipboardEndpoint;
}

namespace notolibra {

// True when the left button is held and the pointer is over one of the
// app's windows that someone can see and use: visible, not occluded, on a
// screen, opaque at all, and taking mouse events. A real drag begins there;
// a hidden run's events are synthetic (the button isn't held) and its window
// is off-screen, behind every other window.
bool MayStartSystemDrag();

// Ends the drag `source` began as a drag that was cancelled where it is.
void EndRefusedDrag(const content::ClipboardEndpoint& source);

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_SYSTEM_DRAG_H_
