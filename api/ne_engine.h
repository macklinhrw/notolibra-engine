/* Copyright 2026 Notolibra. All rights reserved.
 *
 * ne_engine.h: the Notolibra engine's embedding API (Phase 0 stub).
 *
 * This header is the only interface between the Notolibra app (Rust + gpui)
 * and NotolibraEngine.framework (Chromium plus src/notolibra). The app reads
 * it through bindgen (`engine-sys`). Its shape copies ArcCore's object graph
 * (app -> profile -> window -> tab, plus a UI delegate) with C handles in
 * place of Objective-C classes; see plan.md §2.2.
 *
 * Rules (plan.md §2.2):
 *  - No Chromium type crosses this boundary: opaque handles, UTF-8 strings,
 *    plain structs and callbacks only.
 *  - Every function is called on the main thread, and every callback arrives
 *    on the main thread. Results that Chromium produces asynchronously come
 *    back through completions, so the browser process could later move out of
 *    the app process without changing this API.
 *  - The API is versioned: check ne_api_version() before using a downloaded
 *    engine. Every struct passed in starts with `size` (set it to
 *    sizeof(the struct)); fields are only ever appended.
 *  - Strings passed in are borrowed for the duration of the call. Strings
 *    passed to callbacks are valid only during the callback.
 *  - The framework exports only ne_* symbols.
 *  - The same header on every OS; only the presenter's surface handle and the
 *    window handle differ by platform.
 *
 * Status: declarations only. Nothing here is implemented yet.
 */

#ifndef NOTOLIBRA_API_NE_ENGINE_H_
#define NOTOLIBRA_API_NE_ENGINE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(NE_IMPLEMENTATION)
#define NE_EXPORT __attribute__((visibility("default")))
#else
#define NE_EXPORT
#endif

/* Bumped on every incompatible change; appended fields don't bump it. */
#define NE_API_VERSION 1

/* ------------------------------------------------------------------------ */
/* Basic types                                                               */

typedef struct ne_app ne_app;         /* the engine; one per process */
typedef struct ne_profile ne_profile; /* ArcBrowserContext: one on-disk profile */
typedef struct ne_window ne_window;   /* ArcBrowser: a normal Browser per gpui window */
typedef struct ne_tab ne_tab;         /* ArcWebContents: one page and its history */

typedef uint64_t ne_request_id;       /* a UI request awaiting ne_ui_respond() */

typedef enum ne_status {
  NE_OK = 0,
  NE_ERR_INVALID_ARGUMENT = 1,
  NE_ERR_WRONG_THREAD = 2,
  NE_ERR_NOT_INITIALIZED = 3,
  NE_ERR_API_VERSION = 4,
  NE_ERR_GONE = 5,                    /* the handle's object was destroyed */
  NE_ERR_UNSUPPORTED = 6,
  NE_ERR_FAILED = 7,
} ne_status;

typedef struct ne_string_view {
  const char* data;                   /* UTF-8, not necessarily NUL-terminated */
  size_t length;
} ne_string_view;

typedef struct ne_rect {
  double x, y, width, height;         /* in DIPs (points) */
} ne_rect;

/* Returns NE_API_VERSION of the loaded engine. */
NE_EXPORT int32_t ne_api_version(void);

/* The Chromium version the engine was built from, e.g. "156.0.8078.36". */
NE_EXPORT const char* ne_chromium_version(void);

/* ------------------------------------------------------------------------ */
/* App: initialisation, run loop, termination (plan.md §2.4)                 */

typedef struct ne_app_config {
  size_t size;
  int32_t api_version;                /* the NE_API_VERSION the app was built with */
  ne_string_view framework_dir;       /* NotolibraEngine.framework, for helpers and .pak files */
  ne_string_view user_data_dir;       /* parent of all profiles */
  ne_string_view locale;              /* e.g. "en-US" */
  ne_string_view keychain_service;    /* our own keychain item (plan.md §4.6) */
  int32_t argc;                       /* extra Chromium switches, may be 0 */
  const char* const* argv;
} ne_app_config;

/* Starts ContentMainRunner and BrowserMainRunner on the main thread. Call from
 * gpui's finish-launching callback, after NSApp exists. gpui keeps the
 * application object, its delegate and the menu bar. */
NE_EXPORT ne_status ne_app_init(const ne_app_config* config, ne_app** out_app);

/* Installs Chromium's UI-thread run-loop sources on the main run loop that
 * gpui is already running (the macOS port of iOS's attach mode). After this,
 * Chromium tasks run as run-loop sources with no polling. */
NE_EXPORT ne_status ne_app_attach_run_loop(ne_app* app);

/* Fallback pump (CEF-style): runs Chromium's pending UI work, and returns the
 * delay in milliseconds until it next needs to run (-1: only when woken). */
NE_EXPORT int64_t ne_app_pump(ne_app* app);

/* Called from any thread when Chromium wants ne_app_pump() to run soon. */
typedef void (*ne_pump_wakeup_fn)(void* user_data, int64_t delay_ms);
NE_EXPORT void ne_app_set_pump_wakeup(ne_app* app, ne_pump_wakeup_fn fn,
                                      void* user_data);

typedef enum ne_terminate_reply {
  NE_TERMINATE_CANCEL = 0,            /* a beforeunload handler kept a page open */
  NE_TERMINATE_NOW = 1,
  NE_TERMINATE_LATER = 2,             /* the completion fires when done */
} ne_terminate_reply;

typedef void (*ne_terminate_fn)(void* user_data, ne_terminate_reply reply);

/* Closes tabs (running beforeunload), then BrowserMainRunner::Shutdown().
 * Mirrors ArcCore's tryToTerminate. */
NE_EXPORT ne_terminate_reply ne_app_try_terminate(ne_app* app,
                                                  ne_terminate_fn done,
                                                  void* user_data);

/* ------------------------------------------------------------------------ */
/* Profiles                                                                  */

typedef void (*ne_profile_open_fn)(void* user_data, ne_status status,
                                   ne_profile* profile);

/* Opens (creating if needed) the profile at `relative_path` under
 * user_data_dir. Asynchronous: profiles load on Chromium's threads. */
NE_EXPORT void ne_profile_open(ne_app* app, ne_string_view relative_path,
                               ne_profile_open_fn done, void* user_data);

NE_EXPORT void ne_profile_close(ne_profile* profile);

/* ------------------------------------------------------------------------ */
/* Windows and tabs                                                          */

/* One normal Browser behind our NotolibraBrowserWindow, per gpui window. The
 * native window handle is NSWindow* on macOS, HWND on Windows. */
NE_EXPORT ne_status ne_window_create(ne_profile* profile, void* native_window,
                                     ne_window** out_window);
NE_EXPORT void ne_window_destroy(ne_window* window);

typedef struct ne_tab_config {
  size_t size;
  ne_string_view url;                 /* empty: about:blank */
  bool background;                    /* don't activate */
  uint32_t background_color_argb;     /* drawn until first paint (no white flash) */
} ne_tab_config;

NE_EXPORT ne_status ne_tab_create(ne_window* window, const ne_tab_config* config,
                                  ne_tab** out_tab);
NE_EXPORT void ne_tab_close(ne_tab* tab);     /* runs beforeunload */

NE_EXPORT void ne_tab_load(ne_tab* tab, ne_string_view url);
NE_EXPORT void ne_tab_go_back(ne_tab* tab);
NE_EXPORT void ne_tab_go_forward(ne_tab* tab);
NE_EXPORT void ne_tab_reload(ne_tab* tab, bool bypass_cache);
NE_EXPORT void ne_tab_stop(ne_tab* tab);
NE_EXPORT void ne_tab_set_zoom(ne_tab* tab, double factor);   /* 1.0 = 100% */
NE_EXPORT void ne_tab_set_visible(ne_tab* tab, bool visible);
NE_EXPORT void ne_tab_discard(ne_tab* tab);   /* free the renderer, keep history */

typedef enum ne_find_flags {
  NE_FIND_FORWARD = 1 << 0,
  NE_FIND_MATCH_CASE = 1 << 1,
  NE_FIND_NEXT = 1 << 2,              /* continue the current search */
} ne_find_flags;

NE_EXPORT void ne_tab_find(ne_tab* tab, ne_string_view text, uint32_t flags);
NE_EXPORT void ne_tab_stop_finding(ne_tab* tab, bool keep_selection);

/* Why script runs in a page; logged, and gates which worlds may be used
 * (ArcCore's executeJavascript(script, reason:)). */
typedef enum ne_eval_reason {
  NE_EVAL_APP_FEATURE = 0,            /* our own feature, isolated world */
  NE_EVAL_USER_REQUEST = 1,           /* e.g. a user-run bookmarklet, main world */
  NE_EVAL_TEST = 2,                   /* hidden checks only */
} ne_eval_reason;

/* `result_json` is the JSON of the completion value, or NULL on error. */
typedef void (*ne_eval_fn)(void* user_data, ne_status status,
                           const char* result_json);
NE_EXPORT void ne_tab_eval(ne_tab* tab, ne_string_view script,
                           ne_eval_reason reason, ne_eval_fn done,
                           void* user_data);

/* ------------------------------------------------------------------------ */
/* Presenting a tab in a pane: off-screen compositing (plan.md §2.5)         */

typedef enum ne_surface_kind {
  NE_SURFACE_IOSURFACE = 1,           /* macOS: IOSurfaceRef */
  NE_SURFACE_DXGI_HANDLE = 2,         /* Windows: shared NT handle */
  NE_SURFACE_DMABUF = 3,              /* Linux: dma-buf fd */
} ne_surface_kind;

typedef struct ne_frame {
  size_t size;
  uint64_t frame_id;                  /* pass to ne_tab_release_frame() */
  ne_surface_kind kind;
  void* surface;                      /* retained until released */
  int32_t pixel_width, pixel_height;
  double scale_factor;
  ne_rect content_rect;               /* the part of the surface with content */
  int64_t capture_time_us;
} ne_frame;

typedef struct ne_presenter {
  size_t size;
  void* user_data;
  /* A new frame. The engine keeps the buffer until ne_tab_release_frame(),
   * so the app can present it without a CPU copy, and freezing a pane is
   * simply not releasing (plan.md §4.6). */
  void (*on_frame)(void* user_data, const ne_frame* frame);
  /* First visually non-empty paint, and the page's background colour. */
  void (*on_first_paint)(void* user_data, uint32_t background_color_argb);
  void (*on_cursor)(void* user_data, int32_t cursor_kind);
  void (*on_scroll)(void* user_data, double x, double y,
                    double content_width, double content_height);
} ne_presenter;

/* Attach or detach (presenter == NULL) a tab's frames to a pane. Moving a tab
 * between panes is detach then attach; the page doesn't reload. */
NE_EXPORT void ne_tab_set_presenter(ne_tab* tab, const ne_presenter* presenter);
NE_EXPORT void ne_tab_set_view_size(ne_tab* tab, double width, double height,
                                    double scale_factor);
NE_EXPORT void ne_tab_release_frame(ne_tab* tab, uint64_t frame_id);

/* Input, forwarded by hand (plan.md §2.5). */
typedef enum ne_input_kind {
  NE_INPUT_MOUSE_DOWN = 1,
  NE_INPUT_MOUSE_UP = 2,
  NE_INPUT_MOUSE_MOVE = 3,
  NE_INPUT_MOUSE_LEAVE = 4,
  NE_INPUT_WHEEL = 5,
  NE_INPUT_KEY_DOWN = 6,
  NE_INPUT_KEY_UP = 7,
  NE_INPUT_CHAR = 8,
} ne_input_kind;

typedef struct ne_input_event {
  size_t size;
  ne_input_kind kind;
  uint32_t modifiers;                 /* shift 1, control 2, alt 4, command 8 */
  double x, y;                        /* DIPs, relative to the view */
  int32_t button;                     /* 0 left, 1 middle, 2 right */
  int32_t click_count;
  double delta_x, delta_y;            /* wheel, in DIPs */
  bool precise;                       /* trackpad (pixel) scrolling */
  int32_t phase;                      /* wheel phase, platform values */
  int32_t native_key_code;            /* virtual key code */
  ne_string_view text;                /* for NE_INPUT_CHAR and key events */
} ne_input_event;

NE_EXPORT void ne_tab_send_input(ne_tab* tab, const ne_input_event* event);
NE_EXPORT void ne_tab_set_focus(ne_tab* tab, bool focused);

/* Edit commands from the app's menu (⌘C, ⌘V, ⌘A, undo...). */
typedef enum ne_edit_command {
  NE_EDIT_UNDO = 1, NE_EDIT_REDO, NE_EDIT_CUT, NE_EDIT_COPY, NE_EDIT_PASTE,
  NE_EDIT_PASTE_AND_MATCH_STYLE, NE_EDIT_SELECT_ALL, NE_EDIT_DELETE,
} ne_edit_command;
NE_EXPORT void ne_tab_edit(ne_tab* tab, ne_edit_command command);

/* ------------------------------------------------------------------------ */
/* Chrome's interface surfaces as data (plan.md §2.6)                        */

typedef enum ne_ui_request_kind {
  NE_UI_JS_ALERT = 1,
  NE_UI_JS_CONFIRM = 2,
  NE_UI_JS_PROMPT = 3,
  NE_UI_JS_BEFOREUNLOAD = 4,
  NE_UI_PERMISSION = 5,
  NE_UI_CONTEXT_MENU = 6,
  NE_UI_FILE_CHOOSER = 7,
  NE_UI_SELECT_POPUP = 8,
  NE_UI_HTTP_AUTH = 9,
  NE_UI_CERTIFICATE_ERROR = 10,
  NE_UI_EXTERNAL_PROTOCOL = 11,
  NE_UI_DOWNLOAD = 12,
  NE_UI_NEW_TAB = 13,                 /* window.open / target=_blank */
} ne_ui_request_kind;

typedef struct ne_menu_item {
  int32_t command_id;                 /* -1 for a separator */
  ne_string_view label;
  bool enabled;
  bool checked;
  int32_t submenu_index;              /* index into the request's items, or -1 */
} ne_menu_item;

/* One request from a tab. Which fields are meaningful depends on `kind`;
 * unused ones are zero. The app answers each request exactly once with
 * ne_ui_respond(); a request the app can't show is answered as declined. */
typedef struct ne_ui_request {
  size_t size;
  ne_request_id id;
  ne_ui_request_kind kind;
  ne_tab* tab;
  ne_string_view origin;              /* requesting origin, for display */
  ne_string_view message;             /* dialog text, prompt default, auth realm... */
  ne_string_view detail;              /* permission name, file accept types, cert error... */
  ne_rect anchor;                     /* context menu point or <select> bounds, DIPs */
  const ne_menu_item* items;          /* context menu and <select> items */
  size_t item_count;
  int32_t selected_index;             /* <select> */
  bool multiple;                      /* file chooser: multiple files */
} ne_ui_request;

typedef struct ne_ui_delegate {
  size_t size;
  void* user_data;
  void (*on_request)(void* user_data, const ne_ui_request* request);
  /* The request was withdrawn (navigation, tab closed): dismiss its UI. */
  void (*on_cancel)(void* user_data, ne_request_id id);
  /* Tab state for the toolbar. */
  void (*on_navigation_state)(void* user_data, ne_tab* tab, ne_string_view url,
                              ne_string_view title, bool loading,
                              bool can_go_back, bool can_go_forward);
  void (*on_find_result)(void* user_data, ne_tab* tab, int32_t ordinal,
                         int32_t match_count, bool final_update);
} ne_ui_delegate;

NE_EXPORT void ne_app_set_ui_delegate(ne_app* app, const ne_ui_delegate* delegate);

typedef struct ne_ui_response {
  size_t size;
  bool accept;                        /* false: decline / cancel / dismiss */
  ne_string_view text;                /* prompt text, auth user name */
  ne_string_view secret;              /* auth password */
  int32_t command_id;                 /* chosen menu item or <select> index */
  const ne_string_view* paths;        /* chosen files */
  size_t path_count;
  bool remember;                      /* permission: persist the decision */
} ne_ui_response;

NE_EXPORT void ne_ui_respond(ne_app* app, ne_request_id id,
                             const ne_ui_response* response);

/* ------------------------------------------------------------------------ */
/* Features and per-site effects (ArcFeature, ArcBrowserContextEffects)       */

typedef enum ne_feature {
  NE_FEATURE_AD_BLOCK = 1,
  NE_FEATURE_DARK_MODE = 2,           /* Dark Reader */
} ne_feature;

NE_EXPORT void ne_profile_set_feature(ne_profile* profile, ne_feature feature,
                                      bool enabled);
/* Per-site switch; `host` empty means the default for all sites. */
NE_EXPORT void ne_profile_set_site_feature(ne_profile* profile,
                                           ne_string_view host,
                                           ne_feature feature, bool enabled);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* NOTOLIBRA_API_NE_ENGINE_H_ */
