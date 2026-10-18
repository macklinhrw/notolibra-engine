//! Hand-written bindings for the part of api/ne_engine.h the prototype uses.
//! Sizes are checked against the C layout (see the asserts at the bottom).
#![allow(non_camel_case_types, dead_code)]

use std::ffi::{c_char, c_void};

pub const NE_API_VERSION: i32 = 1;
pub const NE_OK: i32 = 0;

#[repr(C)]
pub struct ne_app(c_void);
#[repr(C)]
pub struct ne_profile(c_void);
#[repr(C)]
pub struct ne_window(c_void);
#[repr(C)]
pub struct ne_tab(c_void);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ne_string_view {
    pub data: *const c_char,
    pub length: usize,
}

impl ne_string_view {
    pub fn from(s: &str) -> Self {
        Self { data: s.as_ptr() as *const c_char, length: s.len() }
    }
    pub fn to_string(&self) -> String {
        if self.data.is_null() {
            return String::new();
        }
        // SAFETY: the engine passes `length` bytes of UTF-8, valid during the callback.
        let bytes = unsafe { std::slice::from_raw_parts(self.data as *const u8, self.length) };
        String::from_utf8_lossy(bytes).into_owned()
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct ne_rect {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
}

#[repr(C)]
pub struct ne_app_config {
    pub size: usize,
    pub api_version: i32,
    pub framework_dir: ne_string_view,
    pub user_data_dir: ne_string_view,
    pub locale: ne_string_view,
    pub keychain_service: ne_string_view,
    pub argc: i32,
    pub argv: *const *const c_char,
}

#[repr(C)]
pub struct ne_tab_config {
    pub size: usize,
    pub url: ne_string_view,
    pub background: bool,
    pub background_color_argb: u32,
}

#[repr(C)]
pub struct ne_frame {
    pub size: usize,
    pub frame_id: u64,
    pub kind: i32,
    pub surface: *mut c_void,
    pub pixel_width: i32,
    pub pixel_height: i32,
    pub scale_factor: f64,
    pub content_rect: ne_rect,
    pub capture_time_us: i64,
}

#[repr(C)]
pub struct ne_presenter {
    pub size: usize,
    pub user_data: *mut c_void,
    pub on_frame: Option<extern "C" fn(*mut c_void, *const ne_frame)>,
    pub on_first_paint: Option<extern "C" fn(*mut c_void, u32)>,
    pub on_cursor: Option<extern "C" fn(*mut c_void, i32)>,
    pub on_scroll: Option<extern "C" fn(*mut c_void, f64, f64, f64, f64)>,
}

pub const NE_INPUT_MOUSE_DOWN: i32 = 1;
pub const NE_INPUT_MOUSE_UP: i32 = 2;
pub const NE_INPUT_MOUSE_MOVE: i32 = 3;
pub const NE_INPUT_MOUSE_LEAVE: i32 = 4;
pub const NE_INPUT_WHEEL: i32 = 5;

#[repr(C)]
pub struct ne_input_event {
    pub size: usize,
    pub kind: i32,
    pub modifiers: u32,
    pub x: f64,
    pub y: f64,
    pub button: i32,
    pub click_count: i32,
    pub delta_x: f64,
    pub delta_y: f64,
    pub precise: bool,
    pub phase: i32,
    pub native_key_code: i32,
    pub text: ne_string_view,
}

pub const NE_UI_JS_ALERT: i32 = 1;
pub const NE_UI_JS_CONFIRM: i32 = 2;
pub const NE_UI_JS_PROMPT: i32 = 3;
pub const NE_UI_PERMISSION: i32 = 5;

#[repr(C)]
pub struct ne_ui_request {
    pub size: usize,
    pub id: u64,
    pub kind: i32,
    pub tab: *mut ne_tab,
    pub origin: ne_string_view,
    pub message: ne_string_view,
    pub detail: ne_string_view,
    pub anchor: ne_rect,
    pub items: *const c_void,
    pub item_count: usize,
    pub selected_index: i32,
    pub multiple: bool,
}

#[repr(C)]
pub struct ne_ui_delegate {
    pub size: usize,
    pub user_data: *mut c_void,
    pub on_request: Option<extern "C" fn(*mut c_void, *const ne_ui_request)>,
    pub on_cancel: Option<extern "C" fn(*mut c_void, u64)>,
    pub on_navigation_state: Option<
        extern "C" fn(*mut c_void, *mut ne_tab, ne_string_view, ne_string_view, bool, bool, bool),
    >,
    pub on_find_result: Option<extern "C" fn(*mut c_void, *mut ne_tab, i32, i32, bool)>,
}

#[repr(C)]
pub struct ne_ui_response {
    pub size: usize,
    pub accept: bool,
    pub text: ne_string_view,
    pub secret: ne_string_view,
    pub command_id: i32,
    pub paths: *const ne_string_view,
    pub path_count: usize,
    pub remember: bool,
}

pub type ne_profile_open_fn = extern "C" fn(*mut c_void, i32, *mut ne_profile);
pub type ne_eval_fn = extern "C" fn(*mut c_void, i32, *const c_char);

unsafe extern "C" {
    pub fn ne_api_version() -> i32;
    pub fn ne_chromium_version() -> *const c_char;
    pub fn ne_app_init(config: *const ne_app_config, out: *mut *mut ne_app) -> i32;
    pub fn ne_app_attach_run_loop(app: *mut ne_app) -> i32;
    pub fn ne_app_try_terminate(app: *mut ne_app, done: *const c_void, user_data: *mut c_void) -> i32;
    pub fn ne_app_set_ui_delegate(app: *mut ne_app, delegate: *const ne_ui_delegate);
    pub fn ne_ui_respond(app: *mut ne_app, id: u64, response: *const ne_ui_response);
    pub fn ne_profile_open(app: *mut ne_app, path: ne_string_view, done: ne_profile_open_fn, user_data: *mut c_void);
    pub fn ne_window_create(profile: *mut ne_profile, native_window: *mut c_void, out: *mut *mut ne_window) -> i32;
    pub fn ne_tab_create(window: *mut ne_window, config: *const ne_tab_config, out: *mut *mut ne_tab) -> i32;
    pub fn ne_tab_load(tab: *mut ne_tab, url: ne_string_view);
    pub fn ne_tab_set_presenter(tab: *mut ne_tab, presenter: *const ne_presenter);
    pub fn ne_tab_set_view_size(tab: *mut ne_tab, width: f64, height: f64, scale: f64);
    pub fn ne_tab_release_frame(tab: *mut ne_tab, frame_id: u64);
    pub fn ne_tab_send_input(tab: *mut ne_tab, event: *const ne_input_event);
    pub fn ne_tab_set_focus(tab: *mut ne_tab, focused: bool);
    pub fn ne_tab_eval(tab: *mut ne_tab, script: ne_string_view, reason: i32, done: ne_eval_fn, user_data: *mut c_void);
}

// The C layout, printed by a C probe of ne_engine.h (October 18, 2026).
const _: () = {
    assert!(size_of::<ne_string_view>() == 16);
    assert!(size_of::<ne_rect>() == 32);
    assert!(size_of::<ne_app_config>() == 96);
    assert!(size_of::<ne_tab_config>() == 32);
    assert!(size_of::<ne_frame>() == 88);
    assert!(size_of::<ne_presenter>() == 48);
    assert!(size_of::<ne_input_event>() == 88);
    assert!(std::mem::offset_of!(ne_input_event, text) == 72);
    assert!(size_of::<ne_ui_request>() == 136);
    assert!(std::mem::offset_of!(ne_ui_request, items) == 112);
    assert!(size_of::<ne_ui_delegate>() == 48);
    assert!(size_of::<ne_ui_response>() == 80);
    assert!(std::mem::offset_of!(ne_ui_response, paths) == 56);
};
