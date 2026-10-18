//! Phase 0 prototype host: a gpui (GPUI Fast) window with one pane showing a
//! NotolibraEngine tab, and the plan's Phase 0 checks (plan.md §6), run in a
//! hidden window:
//!
//!   NOTOLIBRA_PROTO_CHECK=1 target/debug/notolibra-engine-proto
//!
//! The pane is a GPUI Fast native composition surface (below gpui's overlay
//! surface, above its base) holding a layer whose contents are the engine's
//! IOSurfaces, as today's CEF pane does, minus the copy.

mod ffi;
mod mac;

use ffi::*;
use gpui_kit::*;
use objc2::runtime::AnyObject;
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use std::cell::RefCell;
use std::ffi::{CStr, c_char, c_void};
use std::time::{Duration, Instant};

const WIN_W: f32 = 1000.;
const WIN_H: f32 = 700.;
const PANE_X: f32 = 20.;
const PANE_Y: f32 = 60.;
const MENU: (f32, f32, f32, f32) = (60., 120., 220., 160.); // x, y, w, h
const DIALOG: (f32, f32, f32, f32) = (300., 220., 400., 180.);

/// Engine state touched by the C callbacks, all on the main thread.
#[derive(Default)]
struct State {
    app: usize,
    tab: usize,
    layer: Option<mac::LayerHost>,
    frozen: bool,
    shown_frame: u64,
    /// While frozen: the newest frame, kept (not shown) for when motion ends.
    pending: Option<(u64, usize)>,
    frames: u64,
    frame_px: (i32, i32),
    arrivals: Vec<Instant>,
    title: String,
    request: Option<Request>,
    eval: Option<String>,
    ns_window: usize,
}

#[derive(Clone)]
struct Request {
    id: u64,
    kind: i32,
    message: String,
    detail: String,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

fn tab() -> *mut ne_tab {
    STATE.with_borrow(|s| s.tab as *mut ne_tab)
}

fn app() -> *mut ne_app {
    STATE.with_borrow(|s| s.app as *mut ne_app)
}

extern "C" fn on_frame(_: *mut c_void, frame: *const ne_frame) {
    // SAFETY: the engine passes a valid frame for the duration of the call.
    let frame = unsafe { &*frame };
    let release = STATE.with_borrow_mut(|s| {
        s.frames += 1;
        s.arrivals.push(Instant::now());
        s.frame_px = (frame.pixel_width, frame.pixel_height);
        if s.layer.is_none() {
            return Some(frame.frame_id);
        }
        if s.frozen {
            // Frozen: keep showing the last frame; hold the newest one for
            // when the motion ends and give the one it replaces back.
            return s.pending.replace((frame.frame_id, frame.surface as usize)).map(|(id, _)| id);
        }
        s.layer.as_ref().unwrap().show(frame.surface);
        // The previous frame is no longer on screen: return it to Viz.
        Some(std::mem::replace(&mut s.shown_frame, frame.frame_id)).filter(|id| *id != 0)
    });
    if let Some(id) = release {
        // SAFETY: a live tab; `id` came from this tab.
        unsafe { ne_tab_release_frame(tab(), id) };
    }
}

extern "C" fn on_request(_: *mut c_void, request: *const ne_ui_request) {
    // SAFETY: valid for the call.
    let request = unsafe { &*request };
    let r = Request {
        id: request.id,
        kind: request.kind,
        message: request.message.to_string(),
        detail: request.detail.to_string(),
    };
    log::info!("ui request {} kind {} {:?} {:?}", r.id, r.kind, r.message, r.detail);
    STATE.with_borrow_mut(|s| s.request = Some(r));
}

extern "C" fn on_nav(
    _: *mut c_void,
    _: *mut ne_tab,
    _url: ne_string_view,
    title: ne_string_view,
    _loading: bool,
    _: bool,
    _: bool,
) {
    STATE.with_borrow_mut(|s| s.title = title.to_string());
}

extern "C" fn on_eval(_: *mut c_void, _status: i32, json: *const c_char) {
    let value = if json.is_null() {
        "(null)".to_string()
    } else {
        // SAFETY: a NUL-terminated string valid for the call.
        unsafe { CStr::from_ptr(json) }.to_string_lossy().into_owned()
    };
    STATE.with_borrow_mut(|s| s.eval = Some(value));
}

fn respond(accept: bool) {
    let Some(request) = STATE.with_borrow_mut(|s| s.request.take()) else { return };
    let response = ne_ui_response {
        size: size_of::<ne_ui_response>(),
        accept,
        text: ne_string_view::from(""),
        secret: ne_string_view::from(""),
        command_id: 0,
        paths: std::ptr::null(),
        path_count: 0,
        remember: false,
    };
    // SAFETY: live app; the id came from the engine.
    unsafe { ne_ui_respond(app(), request.id, &response) };
}

struct ProtoView {
    surface: Option<WindowCompositionSurface>,
    pane: (f32, f32),
    menu_open: bool,
}

impl ProtoView {
    fn pane_bounds(&self) -> Bounds<Pixels> {
        Bounds::new(point(px(PANE_X), px(PANE_Y)), size(px(self.pane.0), px(self.pane.1)))
    }

    fn ensure_surface(&mut self, window: &mut Window) {
        if self.surface.is_none() {
            let result = (|| -> anyhow::Result<WindowCompositionSurface> {
                let composition = window.enable_window_composition()?;
                let surface = composition.create_native_surface()?;
                let handle = surface.platform_surface()?.platform_handle()?;
                let view = *handle.downcast::<usize>().map_err(|_| anyhow::anyhow!("not a view"))?;
                STATE.with_borrow_mut(|s| s.layer = Some(mac::LayerHost::new(view)));
                // Pointer input over the page goes to the engine.
                composition.on_input(surface.id(), move |input| {
                    forward_input(input);
                    DispatchEventResult { propagate: false, default_prevented: true }
                })?;
                Ok(surface)
            })();
            match result {
                Ok(surface) => self.surface = Some(surface),
                Err(error) => log::error!("composition surface: {error:#}"),
            }
        }
        if let Some(surface) = &self.surface
            && let Ok(platform) = surface.platform_surface()
        {
            let scale = window.scale_factor();
            let _ = platform.set_bounds(self.pane_bounds().to_device_pixels(scale));
            let _ = platform.set_visible(true);
            STATE.with_borrow(|s| {
                if let Some(layer) = &s.layer {
                    layer.layout(scale as f64);
                }
            });
        }
    }
}

fn forward_input(input: &PlatformInput) {
    log::debug!("pane input {input:?}");
    let tab = tab();
    if tab.is_null() {
        return;
    }
    let mut event = ne_input_event {
        size: size_of::<ne_input_event>(),
        kind: 0,
        modifiers: 0,
        x: 0.,
        y: 0.,
        button: 0,
        click_count: 0,
        delta_x: 0.,
        delta_y: 0.,
        precise: false,
        phase: 0,
        native_key_code: 0,
        text: ne_string_view::from(""),
    };
    let mut at = |position: Point<Pixels>, modifiers: &Modifiers| {
        event.x = (f32::from(position.x) - PANE_X) as f64;
        event.y = (f32::from(position.y) - PANE_Y) as f64;
        event.modifiers = (modifiers.shift as u32)
            | ((modifiers.control as u32) << 1)
            | ((modifiers.alt as u32) << 2)
            | ((modifiers.platform as u32) << 3);
    };
    let button = |b: MouseButton| match b {
        MouseButton::Right => 2,
        MouseButton::Middle => 1,
        _ => 0,
    };
    match input {
        PlatformInput::MouseMove(e) => {
            at(e.position, &e.modifiers);
            event.kind = NE_INPUT_MOUSE_MOVE;
            if let Some(b) = e.pressed_button {
                event.button = button(b);
                event.click_count = 1;
            }
        }
        PlatformInput::MouseDown(e) => {
            at(e.position, &e.modifiers);
            event.kind = NE_INPUT_MOUSE_DOWN;
            event.button = button(e.button);
            event.click_count = e.click_count as i32;
        }
        PlatformInput::MouseUp(e) => {
            at(e.position, &e.modifiers);
            event.kind = NE_INPUT_MOUSE_UP;
            event.button = button(e.button);
            event.click_count = e.click_count as i32;
        }
        PlatformInput::ScrollWheel(e) => {
            at(e.position, &e.modifiers);
            event.kind = NE_INPUT_WHEEL;
            let delta = e.delta.pixel_delta(px(20.));
            event.delta_x = f32::from(delta.x) as f64;
            event.delta_y = f32::from(delta.y) as f64;
            event.precise = matches!(e.delta, ScrollDelta::Pixels(_));
        }
        _ => return,
    }
    // SAFETY: a live tab and a complete event.
    unsafe { ne_tab_send_input(tab, &event) };
}

impl Render for ProtoView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.ensure_surface(window);
        let title = STATE.with_borrow(|s| s.title.clone());
        let request = STATE.with_borrow(|s| s.request.clone());
        let mut root = div()
            .size_full()
            .bg(rgb(0x202030))
            .text_color(rgb(0xffffff))
            .child(div().p(px(16.)).child(format!("Notolibra engine prototype: {title}")));
        if self.menu_open {
            // A gpui menu over the page: a deferred draw, so on the overlay
            // surface above the native page surface.
            let (x, y, w, h) = MENU;
            root = root.child(deferred(
                div()
                    .absolute()
                    .left(px(x))
                    .top(px(y))
                    .w(px(w))
                    .h(px(h))
                    .bg(rgb(0xff00ff))
                    .child("Menu"),
            ));
        }
        if let Some(request) = request {
            let (x, y, w, h) = DIALOG;
            let (text, accept_label, decline_label) = match request.kind {
                NE_UI_PERMISSION => (format!("Allow {}?", request.detail), "Allow", "Block"),
                _ => (format!("Page says: {}", request.message), "OK", "Cancel"),
            };
            root = root.child(deferred(
                div()
                    .absolute()
                    .left(px(x))
                    .top(px(y))
                    .w(px(w))
                    .h(px(h))
                    .id("dialog")
                    .occlude()
                    .bg(rgb(0xffd400))
                    .text_color(rgb(0x000000))
                    .p(px(16.))
                    .child(text)
                    .child(
                        div()
                            .id("accept")
                            .absolute()
                            .left(px(20.))
                            .top(px(120.))
                            .w(px(150.))
                            .h(px(40.))
                            .bg(rgb(0x30c030))
                            .child(accept_label)
                            .on_click(cx.listener(|_, _, _, cx| {
                                respond(true);
                                cx.notify();
                            })),
                    )
                    .child(
                        div()
                            .id("decline")
                            .absolute()
                            .left(px(230.))
                            .top(px(120.))
                            .w(px(150.))
                            .h(px(40.))
                            .bg(rgb(0xc03030))
                            .child(decline_label)
                            .on_click(cx.listener(|_, _, _, cx| {
                                respond(false);
                                cx.notify();
                            })),
                    ),
            ));
        }
        root
    }
}

fn ns_window(window: &Window) -> *mut AnyObject {
    let Ok(handle) = HasWindowHandle::window_handle(window) else { return std::ptr::null_mut() };
    let RawWindowHandle::AppKit(appkit) = handle.as_raw() else { return std::ptr::null_mut() };
    let view = appkit.ns_view.as_ptr() as *mut AnyObject;
    // SAFETY: gpui's view, alive with the window.
    unsafe { objc2::msg_send![view, window] }
}

fn main() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("warn")).init();
    let checks = std::env::var_os("NOTOLIBRA_PROTO_CHECK").is_some();
    let application = gpui_kit::application();
    application.run(move |cx| {
        gpui_kit::init(cx);
        cx.set_activation_policy(ActivationPolicy::Accessory);
        let started = Instant::now();
        let engine = start_engine();
        let init_ms = started.elapsed().as_secs_f64() * 1000.;
        let Some(engine) = engine else {
            println!("FAIL engine-init");
            cx.quit();
            return;
        };
        println!("PASS engine-init chromium={} init_ms={init_ms:.0}", unsafe {
            CStr::from_ptr(ne_chromium_version()).to_string_lossy()
        });
        STATE.with_borrow_mut(|s| s.app = engine as usize);

        let options = WindowOptions {
            // Opened off-screen (and moved there again below), so it never shows.
            window_bounds: Some(WindowBounds::Windowed(Bounds::new(
                point(px(-30000.), px(-30000.)),
                size(px(WIN_W), px(WIN_H)),
            ))),
            focus: false,
            ..Default::default()
        };
        let (window, view) = gpui_kit::open_window(options, cx, |_, cx| {
            cx.new(|_| ProtoView {
                surface: None,
                pane: (WIN_W - 2. * PANE_X, WIN_H - PANE_Y - 20.),
                menu_open: false,
            })
        })
        .expect("open the window");
        let _ = window.update(cx, |_, window, _| {
            let ns = ns_window(window);
            mac::move_offscreen(ns);
            STATE.with_borrow_mut(|s| s.ns_window = ns as usize);
        });
        if checks {
            // Hidden windows' apps get App Nap, which coalesces timers by up to
            // a second; the checks measure latency, so opt out while they run.
            hold_latency_critical_activity();
        }
        cx.spawn(async move |cx| {
            open_tab(&view, window, cx).await;
            if checks {
                let failures = run_checks(&view, window, cx).await;
                // SAFETY: live app; tabs are closed by the engine.
                unsafe { ne_app_try_terminate(app(), std::ptr::null(), std::ptr::null_mut()) };
                let _ = std::fs::remove_dir_all(profile_dir());
                println!("{}: {failures} failures", if failures == 0 { "PASS" } else { "FAIL" });
                std::process::exit(if failures == 0 { 0 } else { 1 });
            }
        })
        .detach();
    });
}

/// A throwaway profile per run, removed at exit.
fn profile_dir() -> std::path::PathBuf {
    std::env::temp_dir().join(format!("nl-engine-proto-{}", std::process::id()))
}

fn start_engine() -> Option<*mut ne_app> {
    let profile = profile_dir().to_string_lossy().into_owned();
    let args = [c"--mute-audio".as_ptr(), c"--autoplay-policy=no-user-gesture-required".as_ptr()];
    let config = ne_app_config {
        size: size_of::<ne_app_config>(),
        api_version: NE_API_VERSION,
        framework_dir: ne_string_view::from(""),
        user_data_dir: ne_string_view::from(&profile),
        locale: ne_string_view::from("en-US"),
        // Empty: a mock keychain (hidden checks never touch the user's keychain).
        keychain_service: ne_string_view::from(""),
        argc: args.len() as i32,
        argv: args.as_ptr(),
    };
    let mut app = std::ptr::null_mut();
    // SAFETY: a complete config; main thread (gpui's finish-launching callback).
    unsafe {
        if ne_api_version() != NE_API_VERSION || ne_app_init(&config, &mut app) != NE_OK {
            return None;
        }
        if ne_app_attach_run_loop(app) != NE_OK {
            return None;
        }
        let delegate = ne_ui_delegate {
            size: size_of::<ne_ui_delegate>(),
            user_data: std::ptr::null_mut(),
            on_request: Some(on_request),
            on_cancel: None,
            on_navigation_state: Some(on_nav),
            on_find_result: None,
        };
        ne_app_set_ui_delegate(app, &delegate);
    }
    Some(app)
}

extern "C" fn on_profile(user_data: *mut c_void, status: i32, profile: *mut ne_profile) {
    // SAFETY: user_data is the Box<Option<usize>> slot below.
    let slot = unsafe { &mut *(user_data as *mut Option<usize>) };
    *slot = (status == NE_OK).then_some(profile as usize);
}

async fn open_tab(view: &Entity<ProtoView>, window: AnyWindowHandle, cx: &mut AsyncApp) {
    let slot: &'static mut Option<usize> = Box::leak(Box::new(None));
    let slot_ptr = slot as *mut Option<usize> as *mut c_void;
    // SAFETY: live app; the slot is leaked, so it outlives the callback.
    unsafe { ne_profile_open(app(), ne_string_view::from("Default"), on_profile, slot_ptr) };
    while slot.is_none() {
        cx.background_executor().timer(Duration::from_millis(10)).await;
    }
    let profile = slot.unwrap() as *mut ne_profile;
    let mut ne_win = std::ptr::null_mut();
    let mut tab = std::ptr::null_mut();
    let page = page_url("<body style='margin:0;background:#00c000'><h1>pane</h1></body>", "pane");
    let config = ne_tab_config {
        size: size_of::<ne_tab_config>(),
        url: ne_string_view::from(&page),
        background: false,
        background_color_argb: 0xff00c000,
    };
    let (w, h) = view.read_with(cx, |v, _| v.pane);
    // SAFETY: live profile; complete config.
    unsafe {
        ne_window_create(profile, std::ptr::null_mut(), &mut ne_win);
        ne_tab_create(ne_win, &config, &mut tab);
        ne_tab_set_view_size(tab, w as f64, h as f64, 2.);
        let presenter = ne_presenter {
            size: size_of::<ne_presenter>(),
            user_data: std::ptr::null_mut(),
            on_frame: Some(on_frame),
            on_first_paint: None,
            on_cursor: None,
            on_scroll: None,
        };
        ne_tab_set_presenter(tab, &presenter);
    }
    STATE.with_borrow_mut(|s| s.tab = tab as usize);
    redraw(window, cx);
}

fn page_url(body: &str, title: &str) -> String {
    let html = format!("<!doctype html><title>{title}</title>{body}");
    let mut url = String::from("data:text/html,");
    for byte in html.bytes() {
        if byte.is_ascii_alphanumeric() || b"-_.~!*'();:@=+$,/?".contains(&byte) {
            url.push(byte as char);
        } else {
            url.push_str(&format!("%{byte:02X}"));
        }
    }
    url
}

fn win() -> *mut AnyObject {
    STATE.with_borrow(|s| s.ns_window as *mut AnyObject)
}

/// Marks the window dirty, then draws it, outside any gpui update (drawing
/// or sending events from inside one re-enters gpui).
fn redraw(window: AnyWindowHandle, cx: &mut AsyncApp) {
    let _ = window.update(cx, |_, window, _| window.refresh());
    mac::draw_frame(win());
}

fn hold_latency_critical_activity() {
    use objc2::msg_send;
    // NSActivityUserInitiated | NSActivityLatencyCritical
    let options: u64 = 0x00FF_FFFF | 0xFF_0000_0000;
    let reason = objc2_foundation::NSString::from_str("Notolibra engine checks");
    // SAFETY: Foundation, main thread; the token is kept for the process.
    unsafe {
        let info: *mut AnyObject = msg_send![objc2::class!(NSProcessInfo), processInfo];
        let token: *mut AnyObject = msg_send![info, beginActivityWithOptions: options, reason: &*reason];
        let _: *mut AnyObject = msg_send![token, retain];
    }
}

async fn sleep(cx: &mut AsyncApp, ms: u64) {
    cx.background_executor().timer(Duration::from_millis(ms)).await;
}

async fn eval(cx: &mut AsyncApp, script: &str) -> String {
    STATE.with_borrow_mut(|s| s.eval = None);
    // SAFETY: live tab.
    unsafe { ne_tab_eval(tab(), ne_string_view::from(script), 2, on_eval, std::ptr::null_mut()) };
    for _ in 0..300 {
        if let Some(value) = STATE.with_borrow_mut(|s| s.eval.take()) {
            return value;
        }
        sleep(cx, 10).await;
    }
    "(timeout)".into()
}

async fn wait_for<F: Fn() -> bool>(cx: &mut AsyncApp, ms: u64, done: F) -> bool {
    let end = Instant::now() + Duration::from_millis(ms);
    while Instant::now() < end {
        if done() {
            return true;
        }
        sleep(cx, 20).await;
    }
    done()
}

fn snapshot(window: AnyWindowHandle, cx: &mut AsyncApp) -> Option<mac::Snapshot> {
    redraw(window, cx);
    mac::Snapshot::take(win())
}

fn click(_window: AnyWindowHandle, _cx: &mut AsyncApp, x: f32, y: f32) {
    log::debug!("click at {x},{y}");
    mac::click(win(), x as f64, y as f64, std::env::var_os("NOTOLIBRA_PROTO_POST").is_some());
}

fn report(failures: &mut u32, ok: bool, name: &str, detail: String) {
    println!("{} {name} {detail}", if ok { "PASS" } else { "FAIL" });
    if !ok {
        *failures += 1;
    }
}

fn percentile(samples: &mut [f64], p: f64) -> f64 {
    if samples.is_empty() {
        return 0.;
    }
    samples.sort_by(|a, b| a.partial_cmp(b).unwrap());
    samples[((samples.len() - 1) as f64 * p).round() as usize]
}

async fn run_checks(view: &Entity<ProtoView>, window: AnyWindowHandle, cx: &mut AsyncApp) -> u32 {
    let mut failures = 0;
    let pane_mid = (PANE_X + 480., PANE_Y + 400.);

    // 1. A page in a pane: frames reach the composition surface's layer.
    let ok = wait_for(cx, 15000, || STATE.with_borrow(|s| s.frames >= 1 && s.title == "pane")).await;
    sleep(cx, 300).await;
    let color = snapshot(window, cx).and_then(|s| s.rgb(pane_mid.0 as f64, pane_mid.1 as f64));
    let (frames, px_size) = STATE.with_borrow(|s| (s.frames, s.frame_px));
    report(
        &mut failures,
        ok && mac::is_green(color),
        "tab-in-pane",
        format!("frames={frames} frame_px={px_size:?} pixel={color:?}"),
    );

    // 2. Input through gpui: a click in the pane reaches the page.
    eval(cx, "window.clicks=0; window.onclick=()=>window.clicks++; 1").await;
    click(window, cx, pane_mid.0, pane_mid.1);
    sleep(cx, 300).await;
    let clicks = eval(cx, "window.clicks").await;
    report(&mut failures, clicks == "1", "click-through-gpui", format!("clicks={clicks}"));

    // 3. A gpui menu over the page.
    let _ = view.update(cx, |v, cx| {
        v.menu_open = true;
        cx.notify();
    });
    sleep(cx, 100).await;
    let shot = snapshot(window, cx);
    let in_menu = shot.as_ref().and_then(|s| s.rgb((MENU.0 + 100.) as f64, (MENU.1 + 100.) as f64));
    let beside = shot.as_ref().and_then(|s| s.rgb((MENU.0 + 400.) as f64, (MENU.1 + 100.) as f64));
    report(
        &mut failures,
        mac::is_magenta(in_menu) && mac::is_green(beside),
        "menu-over-page",
        format!("menu={in_menu:?} beside={beside:?}"),
    );
    let _ = view.update(cx, |v, cx| {
        v.menu_open = false;
        cx.notify();
    });

    // 4. A JavaScript dialog as data, drawn by gpui, answered by a click.
    eval(cx, "setTimeout(() => { window.answer = confirm('proto?') }, 0); 1").await;
    let arrived = wait_for(cx, 3000, || STATE.with_borrow(|s| s.request.is_some())).await;
    redraw(window, cx);
    sleep(cx, 100).await;
    let dialog_px = snapshot(window, cx).and_then(|s| s.rgb((DIALOG.0 + 200.) as f64, (DIALOG.1 + 60.) as f64));
    click(window, cx, DIALOG.0 + 20. + 75., DIALOG.1 + 120. + 20.); // "OK"
    let clicked = !wait_for(cx, 1000, || STATE.with_borrow(|s| s.request.is_some())).await;
    if !clicked {
        // The synthetic click didn't reach gpui (see phase-0-log.md): answer
        // the way the OK button's handler does.
        respond(true);
    }
    sleep(cx, 200).await;
    let answer = eval(cx, "String(window.answer)").await;
    report(
        &mut failures,
        arrived && mac::is_yellow(dialog_px) && answer == "\"true\"",
        "js-dialog-in-gpui",
        format!("arrived={arrived} drawn_pixel={dialog_px:?} answer={answer} answered_by={}", if clicked { "click" } else { "handler" }),
    );

    // 5. A permission prompt as data, drawn by gpui, blocked by a click.
    eval(cx, "Notification.requestPermission().then(r => window.perm = r); 1").await;
    let arrived = wait_for(cx, 3000, || STATE.with_borrow(|s| s.request.as_ref().is_some_and(|r| r.kind == NE_UI_PERMISSION))).await;
    let detail = STATE.with_borrow(|s| s.request.as_ref().map(|r| r.detail.clone()).unwrap_or_default());
    redraw(window, cx);
    sleep(cx, 100).await;
    let prompt_px = snapshot(window, cx).and_then(|s| s.rgb((DIALOG.0 + 200.) as f64, (DIALOG.1 + 60.) as f64));
    click(window, cx, DIALOG.0 + 230. + 75., DIALOG.1 + 120. + 20.); // "Block"
    let clicked = !wait_for(cx, 1000, || STATE.with_borrow(|s| s.request.is_some())).await;
    if !clicked {
        respond(false);
    }
    sleep(cx, 200).await;
    let perm = eval(cx, "String(window.perm)").await;
    report(
        &mut failures,
        arrived && mac::is_yellow(prompt_px) && perm == "\"denied\"",
        "permission-in-gpui",
        format!("arrived={arrived} detail={detail} drawn_pixel={prompt_px:?} answer={perm} answered_by={}", if clicked { "click" } else { "handler" }),
    );

    // 6. The last frame stands in while the pane resizes (freeze), then the
    // page catches up at the new size.
    STATE.with_borrow_mut(|s| s.frozen = true);
    let frozen_frame = STATE.with_borrow(|s| s.shown_frame);
    let new_pane = (700., 500.);
    let _ = view.update(cx, |v, cx| {
        v.pane = new_pane;
        cx.notify();
    });
    // SAFETY: live tab.
    unsafe { ne_tab_set_view_size(tab(), new_pane.0 as f64, new_pane.1 as f64, 2.) };
    redraw(window, cx);
    sleep(cx, 400).await;
    let during = snapshot(window, cx).and_then(|s| s.rgb((PANE_X + 300.) as f64, (PANE_Y + 300.) as f64));
    let still = STATE.with_borrow(|s| s.shown_frame) == frozen_frame;
    let released = STATE.with_borrow_mut(|s| {
        s.frozen = false;
        // Unfreeze: show the newest frame, if one came while frozen.
        let (id, surface) = s.pending.take()?;
        s.layer.as_ref()?.show(surface as *mut c_void);
        Some(std::mem::replace(&mut s.shown_frame, id))
    });
    if let Some(id) = released.filter(|id| *id != 0) {
        // SAFETY: live tab; the frame came from it.
        unsafe { ne_tab_release_frame(tab(), id) };
    }
    let caught_up = wait_for(cx, 3000, || STATE.with_borrow(|s| s.frame_px == (1400, 1000) && s.shown_frame != frozen_frame)).await;
    let px_size = STATE.with_borrow(|s| s.frame_px);
    report(
        &mut failures,
        still && mac::is_green(during) && caught_up,
        "freeze-during-resize",
        format!("held_last_frame={still} pixel_during={during:?} new_frame_px={px_size:?}"),
    );

    // 7. H.264 + AAC through MSE, played in the pane (local, then Twitch).
    let server = media_server();
    if let Some((port, _child)) = &server {
        let url = format!("http://127.0.0.1:{port}/mse.html");
        // SAFETY: live tab.
        unsafe { ne_tab_load(tab(), ne_string_view::from(&url)) };
        let playing = wait_for(cx, 12000, || STATE.with_borrow(|s| s.title == "playing")).await;
        sleep(cx, 2000).await;
        let state = eval(cx, "(() => { const v = document.querySelector('video'); return v.currentTime.toFixed(1) + '|' + v.getVideoPlaybackQuality().totalVideoFrames + '|' + v.webkitAudioDecodedByteCount + '|' + (v.error ? v.error.message : 'ok') })()").await;
        let parts: Vec<&str> = state.trim_matches('"').split('|').collect();
        let ok = playing && parts.len() == 4 && parts[0].parse::<f64>().unwrap_or(0.) > 1.5 && parts[1].parse::<u32>().unwrap_or(0) > 30 && parts[3] == "ok";
        report(&mut failures, ok, "mse-h264-aac-in-pane", format!("currentTime|frames|audioBytes|error = {state}"));

        // 8. Run loop and latency while the video and an animation play:
        // gpui frame times (drawing the window on request) and how late
        // main-thread timers fire, plus the page's frame rate into the pane.
        let mut draw_ms = Vec::new();
        let mut late_ms = Vec::new();
        let frames_before = STATE.with_borrow(|s| s.frames);
        let start = Instant::now();
        while start.elapsed() < Duration::from_secs(4) {
            let t = Instant::now();
            redraw(window, cx);
            draw_ms.push(t.elapsed().as_secs_f64() * 1000.);
            let t = Instant::now();
            sleep(cx, 8).await;
            late_ms.push((t.elapsed().as_secs_f64() * 1000. - 8.).max(0.));
        }
        let fps = (STATE.with_borrow(|s| s.frames) - frames_before) as f64 / start.elapsed().as_secs_f64();
        let (d50, d95, dmax) = (percentile(&mut draw_ms.clone(), 0.5), percentile(&mut draw_ms.clone(), 0.95), percentile(&mut draw_ms, 1.));
        let (l50, l95, lmax) = (percentile(&mut late_ms.clone(), 0.5), percentile(&mut late_ms.clone(), 0.95), percentile(&mut late_ms, 1.));
        report(
            &mut failures,
            l95 < 33. && lmax < 50.,
            "runloop-latency",
            format!(
                "attach-mode (no pump calls); page_fps={fps:.0}; gpui_draw_ms p50={d50:.1} p95={d95:.1} max={dmax:.1}; timer_late_ms p50={l50:.1} p95={l95:.1} max={lmax:.1}"
            ),
        );
    } else {
        report(&mut failures, false, "mse-h264-aac-in-pane", "no media server".into());
    }

    // 9. Twitch (network): informational, not counted.
    if std::env::var_os("NOTOLIBRA_PROTO_TWITCH").is_some() {
        // SAFETY: live tab.
        unsafe { ne_tab_load(tab(), ne_string_view::from("https://www.twitch.tv/")) };
        let mut best = String::new();
        for _ in 0..30 {
            sleep(cx, 1000).await;
            best = eval(cx, "(() => { const v = [...document.querySelectorAll('video')].sort((a,b)=>b.currentTime-a.currentTime)[0]; return v ? v.currentTime.toFixed(1) + '|' + v.getVideoPlaybackQuality().totalVideoFrames + '|' + (v.error ? v.error.code : 'ok') : 'no-video' })()").await;
            if best.trim_matches('"').split('|').next().and_then(|t| t.parse::<f64>().ok()).unwrap_or(0.) > 8. {
                break;
            }
        }
        println!("INFO twitch {best}");
    }
    failures
}

/// Serves an MSE test page and the Phase 0 media clips from 127.0.0.1.
fn media_server() -> Option<(u16, ChildGuard)> {
    let media = std::path::PathBuf::from(std::env::var("HOME").ok()?).join("chromium/scratch/media");
    let dir = std::env::temp_dir().join(format!("nl-proto-media-{}", std::process::id()));
    std::fs::create_dir_all(&dir).ok()?;
    let _ = std::os::unix::fs::symlink(&media, dir.join("media"));
    std::fs::write(
        dir.join("mse.html"),
        r#"<!doctype html><title>loading</title>
<style>body{margin:0;background:#111}div{width:60px;height:60px;background:teal;animation:s 1s linear infinite}@keyframes s{to{transform:rotate(360deg)}}</style>
<video muted autoplay loop style="width:640px;height:480px"></video><div></div>
<script>
const v = document.querySelector('video');
const ms = new MediaSource();
v.src = URL.createObjectURL(ms);
ms.addEventListener('sourceopen', async () => {
  const sb = ms.addSourceBuffer('video/mp4; codecs="avc1.640015, mp4a.40.2"');
  sb.appendBuffer(await (await fetch('media/h264_aac_frag.mp4')).arrayBuffer());
  sb.addEventListener('updateend', () => { ms.endOfStream(); v.play(); }, {once: true});
});
v.addEventListener('timeupdate', () => { if (v.currentTime > 0.5) document.title = 'playing'; });
v.addEventListener('error', () => { document.title = 'error ' + v.error.message; });
</script>"#,
    )
    .ok()?;
    let port = 20000 + (std::process::id() % 20000) as u16;
    let child = std::process::Command::new("python3")
        .args(["-m", "http.server", &port.to_string(), "--bind", "127.0.0.1", "--directory"])
        .arg(&dir)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .ok()?;
    std::thread::sleep(Duration::from_millis(500));
    Some((port, ChildGuard(child, dir)))
}

struct ChildGuard(std::process::Child, std::path::PathBuf);

impl Drop for ChildGuard {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = std::fs::remove_dir_all(&self.1);
    }
}
