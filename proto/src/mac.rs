//! AppKit helpers: the layer that shows a tab's IOSurfaces inside a GPUI Fast
//! composition surface, and the hidden-window tools the checks need
//! (off-screen placement, drawing a frame on request, synthetic clicks,
//! sampling the window's composited pixels). Written for this prototype after
//! the patterns in native/crates/app/src/{osr,capture}.rs.

use objc2::{msg_send, runtime::AnyObject};
use objc2_foundation::{NSPoint, NSRect, NSSize};
use std::ffi::c_void;

/// A layer-backed NSView inside the composition surface's view; its layer's
/// contents are the engine's IOSurface (no copy).
pub struct LayerHost {
    surface_view: *mut AnyObject,
    view: *mut AnyObject,
}

impl LayerHost {
    pub fn new(surface_view: usize) -> Self {
        let surface_view = surface_view as *mut AnyObject;
        // SAFETY: main thread; the surface view outlives the window's pane.
        unsafe {
            let view: *mut AnyObject = msg_send![objc2::class!(NSView), alloc];
            let view: *mut AnyObject =
                msg_send![view, initWithFrame: NSRect::new(NSPoint::new(0., 0.), NSSize::new(0., 0.))];
            let _: () = msg_send![view, setWantsLayer: true];
            // NSViewLayerContentsRedrawNever: AppKit never redraws it.
            let _: () = msg_send![view, setLayerContentsRedrawPolicy: 0isize];
            let layer: *mut AnyObject = msg_send![view, layer];
            let gravity = objc2_foundation::NSString::from_str("topLeft");
            let _: () = msg_send![layer, setContentsGravity: &*gravity];
            let _: () = msg_send![surface_view, addSubview: view];
            Self { surface_view, view }
        }
    }

    /// Sizes the layer to the surface view, in points, and sets its scale.
    pub fn layout(&self, scale: f64) {
        // SAFETY: main thread, live views.
        unsafe {
            let bounds: NSRect = msg_send![self.surface_view, bounds];
            let _: () = msg_send![self.view, setFrame: bounds];
            let layer: *mut AnyObject = msg_send![self.view, layer];
            let _: () = msg_send![layer, setContentsScale: scale];
        }
    }

    /// Shows an IOSurface (the layer retains it).
    pub fn show(&self, surface: *mut c_void) {
        // SAFETY: main thread; `surface` is a live IOSurfaceRef.
        unsafe {
            let _: () = msg_send![objc2::class!(CATransaction), begin];
            let _: () = msg_send![objc2::class!(CATransaction), setDisableActions: true];
            let layer: *mut AnyObject = msg_send![self.view, layer];
            let _: () = msg_send![layer, setContents: surface as *mut AnyObject];
            let _: () = msg_send![objc2::class!(CATransaction), commit];
        }
    }
}

pub fn move_offscreen(ns_window: *mut AnyObject) {
    // SAFETY: a live NSWindow, on the main thread.
    unsafe {
        let _: () = msg_send![ns_window, setFrameOrigin: NSPoint::new(-30000., -30000.)];
        let _: () = msg_send![ns_window, orderBack: std::ptr::null::<AnyObject>()];
    }
}

pub fn set_content_size(ns_window: *mut AnyObject, width: f64, height: f64) {
    // SAFETY: a live NSWindow, on the main thread.
    unsafe {
        let _: () = msg_send![ns_window, setContentSize: NSSize::new(width, height)];
    }
}

/// Draws one GPUI frame now: an off-screen window gets no display-link
/// callbacks, so nothing redraws until asked (as the app's checks do).
pub fn draw_frame(ns_window: *mut AnyObject) {
    // SAFETY: AppKit calls on the main thread with a live window.
    unsafe {
        let mut views: Vec<*mut AnyObject> = vec![msg_send![ns_window, contentView]];
        while let Some(view) = views.pop() {
            if view.is_null() {
                continue;
            }
            let draws: bool = msg_send![view, respondsToSelector: objc2::sel!(displayLayer:)];
            let layer: *mut AnyObject = msg_send![view, layer];
            if draws && !layer.is_null() {
                let _: () = msg_send![view, displayLayer: layer];
                return;
            }
            let subviews: *mut AnyObject = msg_send![view, subviews];
            let count: usize = msg_send![subviews, count];
            for index in 0..count {
                views.push(msg_send![subviews, objectAtIndex: index]);
            }
        }
    }
}

/// A left click at window-content point (x, y), origin top-left, sent
/// through the window's normal event path.
pub fn click(ns_window: *mut AnyObject, x: f64, y: f64, post: bool) {
    for event_type in [5usize, 1, 2] {
        // moved, down, up
        // SAFETY: AppKit calls on the main thread with a live window.
        unsafe {
            let content: *mut AnyObject = msg_send![ns_window, contentView];
            let bounds: NSRect = msg_send![content, bounds];
            let location = NSPoint::new(x, bounds.size.height - y);
            let number: isize = msg_send![ns_window, windowNumber];
            let info: *mut AnyObject = msg_send![objc2::class!(NSProcessInfo), processInfo];
            let now: f64 = msg_send![info, systemUptime];
            let event: *mut AnyObject = msg_send![
                objc2::class!(NSEvent),
                mouseEventWithType: event_type,
                location: location,
                modifierFlags: 0usize,
                timestamp: now,
                windowNumber: number,
                context: std::ptr::null::<AnyObject>(),
                eventNumber: 0isize,
                clickCount: 1isize,
                pressure: 1f32
            ];
            if !event.is_null() {
                if post {
                    let app: *mut AnyObject = msg_send![objc2::class!(NSApplication), sharedApplication];
                    let _: () = msg_send![app, postEvent: event, atStart: false];
                } else {
                    let _: () = msg_send![ns_window, sendEvent: event];
                }
            }
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
struct CGRect {
    x: f64,
    y: f64,
    w: f64,
    h: f64,
}

#[link(name = "CoreGraphics", kind = "framework")]
unsafe extern "C" {
    fn CGWindowListCreateImage(rect: CGRect, options: u32, window: u32, image: u32) -> *mut c_void;
    fn CGImageGetWidth(image: *const c_void) -> usize;
    fn CGImageGetHeight(image: *const c_void) -> usize;
    fn CGImageGetBytesPerRow(image: *const c_void) -> usize;
    fn CGImageGetDataProvider(image: *const c_void) -> *const c_void;
    fn CGDataProviderCopyData(provider: *const c_void) -> *const c_void;
}

#[link(name = "CoreFoundation", kind = "framework")]
unsafe extern "C" {
    fn CFDataGetBytePtr(data: *const c_void) -> *const u8;
    fn CFRelease(object: *const c_void);
}

/// The window's composited pixels, as the window server shows them.
pub struct Snapshot {
    bytes: Vec<u8>,
    width: usize,
    height: usize,
    stride: usize,
    /// Rows above the content (the titlebar), in pixels.
    content_top: usize,
    scale: f64,
}

impl Snapshot {
    pub fn take(ns_window: *mut AnyObject) -> Option<Snapshot> {
        // SAFETY: main thread, live window; CF objects released below.
        unsafe {
            let number: isize = msg_send![ns_window, windowNumber];
            let scale: f64 = msg_send![ns_window, backingScaleFactor];
            let content: *mut AnyObject = msg_send![ns_window, contentView];
            let bounds: NSRect = msg_send![content, bounds];
            let null_rect = CGRect { x: f64::INFINITY, y: f64::INFINITY, w: 0., h: 0. };
            // kCGWindowListOptionIncludingWindow, kCGWindowImageBoundsIgnoreFraming
            let image = CGWindowListCreateImage(null_rect, 1 << 3, number as u32, 1 << 0);
            if image.is_null() {
                return None;
            }
            let width = CGImageGetWidth(image);
            let height = CGImageGetHeight(image);
            let stride = CGImageGetBytesPerRow(image);
            let data = CGDataProviderCopyData(CGImageGetDataProvider(image));
            let bytes = std::slice::from_raw_parts(CFDataGetBytePtr(data), stride * height).to_vec();
            CFRelease(data);
            CFRelease(image);
            let content_height = (bounds.size.height * scale).round() as usize;
            Some(Snapshot {
                bytes,
                width,
                height,
                stride,
                content_top: height.saturating_sub(content_height),
                scale,
            })
        }
    }

    /// The colour (RGB) at window-content point (x, y), top-left origin.
    pub fn rgb(&self, x: f64, y: f64) -> Option<[u8; 3]> {
        let px = (x * self.scale) as usize;
        let py = self.content_top + (y * self.scale) as usize;
        if px >= self.width || py >= self.height {
            return None;
        }
        let i = py * self.stride + px * 4;
        // BGRA, little-endian premultiplied-first.
        Some([self.bytes[i + 2], self.bytes[i + 1], self.bytes[i]])
    }
}

// Colour classes rather than exact values: the window server colour-manages
// the composited window (sRGB #00c000 reads back as about (86, 189, 55)).
pub fn is_green(c: Option<[u8; 3]>) -> bool {
    c.is_some_and(|[r, g, b]| g as i32 > r as i32 + 60 && g as i32 > b as i32 + 60)
}

pub fn is_magenta(c: Option<[u8; 3]>) -> bool {
    c.is_some_and(|[r, g, b]| r > 180 && b > 180 && g < 120)
}

pub fn is_yellow(c: Option<[u8; 3]>) -> bool {
    c.is_some_and(|[r, g, b]| r > 200 && g > 170 && b < 130)
}
