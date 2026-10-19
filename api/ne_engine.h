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
 * Status: implemented by engine/ (api.mm), except where a declaration says
 * otherwise. Appended fields and functions are marked with the phase that
 * added them.
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

/* Bumped on every incompatible change; appended fields don't bump it.
 * The response to each request kind (ne_ui_respond):
 *  - JS alert/confirm/prompt/beforeunload: accept, text (prompt);
 *  - permission: accept, remember;
 *  - <select>: accept, command_id (the item's index);
 *  - context menu: accept, command_id (ne_context_command or the app's own);
 *  - file chooser: accept, paths;
 *  - HTTP auth: accept, text (user name), secret (password);
 *  - certificate error: accept (proceed);
 *  - download: accept, paths[0] (where to save);
 *  - new tab: accept (adopt request.new_tab). */
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

/* Frames (Phase 1). A frame id is Chromium's FrameTreeNode id: it names one
 * frame of the page for as long as the frame exists, across its navigations
 * and process swaps. 0 always means the page's main frame. */
typedef int64_t ne_frame_id;

/* The JavaScript world a script runs in. */
typedef enum ne_world {
  NE_WORLD_MAIN = 0,                  /* the page's own world */
  NE_WORLD_APP = 1,                   /* our isolated world */
} ne_world;

/* Runs `script` in one frame. `done` may be NULL (no result is collected). */
NE_EXPORT void ne_tab_eval_in_frame(ne_tab* tab, ne_frame_id frame,
                                    ne_string_view script, ne_world world,
                                    ne_eval_fn done, void* user_data);

/* Calls `fn` once for each frame of the page that has committed a document,
 * synchronously, main frame first. */
typedef void (*ne_frame_fn)(void* user_data, ne_frame_id frame,
                            ne_string_view url, bool is_main);
NE_EXPORT void ne_tab_for_each_frame(ne_tab* tab, ne_frame_fn fn,
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
  /* When the engine's compositor produced the frame: base::TimeTicks in
   * microseconds (mach_absolute_time() in microseconds on macOS). */
  int64_t capture_time_us;
} ne_frame;

typedef struct ne_presenter {
  size_t size;
  void* user_data;
  /* A new frame. The engine keeps the buffer until ne_tab_release_frame(),
   * so the app can present it without a CPU copy. On macOS the buffer is
   * the page compositor's own output (sRGB), which it draws into again
   * once the page has drawn four newer frames: hold at most the newest few,
   * and copy what must outlast that (a frozen pane's picture). */
  void (*on_frame)(void* user_data, const ne_frame* frame);
  /* First visually non-empty paint, and the page's background colour. */
  void (*on_first_paint)(void* user_data, uint32_t background_color_argb);
  void (*on_cursor)(void* user_data, int32_t cursor_kind);
  void (*on_scroll)(void* user_data, double x, double y,
                    double content_width, double content_height);
  /* Phase 1: the page's background colour changed (ARGB). */
  void (*on_background_color)(void* user_data, uint32_t background_color_argb);
  /* Phase 1: the focused element's text input (ne_text_input_type; 0 none),
   * for the app's input method handling. */
  void (*on_text_input_state)(void* user_data, int32_t type, uint32_t flags);
  /* Phase 1: the page cancelled the input method's composition. */
  void (*on_ime_cancel)(void* user_data);
  /* Phase 1: the tooltip for the element under the pointer (empty: none). */
  void (*on_tooltip)(void* user_data, ne_string_view text);
} ne_presenter;

/* Phase 1: on_cursor's kinds: Chromium's ui::mojom::CursorType values. */
typedef enum ne_cursor_kind {
  NE_CURSOR_POINTER = 0,
  NE_CURSOR_CROSS = 1,
  NE_CURSOR_HAND = 2,
  NE_CURSOR_IBEAM = 3,
  NE_CURSOR_WAIT = 4,
  NE_CURSOR_HELP = 5,
  NE_CURSOR_EAST_RESIZE = 6,
  NE_CURSOR_NORTH_RESIZE = 7,
  NE_CURSOR_NORTH_EAST_RESIZE = 8,
  NE_CURSOR_NORTH_WEST_RESIZE = 9,
  NE_CURSOR_SOUTH_RESIZE = 10,
  NE_CURSOR_SOUTH_EAST_RESIZE = 11,
  NE_CURSOR_SOUTH_WEST_RESIZE = 12,
  NE_CURSOR_WEST_RESIZE = 13,
  NE_CURSOR_NORTH_SOUTH_RESIZE = 14,
  NE_CURSOR_EAST_WEST_RESIZE = 15,
  NE_CURSOR_NORTH_EAST_SOUTH_WEST_RESIZE = 16,
  NE_CURSOR_NORTH_WEST_SOUTH_EAST_RESIZE = 17,
  NE_CURSOR_COLUMN_RESIZE = 18,
  NE_CURSOR_ROW_RESIZE = 19,
  NE_CURSOR_MOVE = 29,
  NE_CURSOR_VERTICAL_TEXT = 30,
  NE_CURSOR_CELL = 31,
  NE_CURSOR_CONTEXT_MENU = 32,
  NE_CURSOR_ALIAS = 33,
  NE_CURSOR_PROGRESS = 34,
  NE_CURSOR_NO_DROP = 35,
  NE_CURSOR_COPY = 36,
  NE_CURSOR_NONE = 37,
  NE_CURSOR_NOT_ALLOWED = 38,
  NE_CURSOR_ZOOM_IN = 39,
  NE_CURSOR_ZOOM_OUT = 40,
  NE_CURSOR_GRAB = 41,
  NE_CURSOR_GRABBING = 42,
  NE_CURSOR_CUSTOM = 45,              /* an image; shown as the pointer */
} ne_cursor_kind;

/* Phase 1: on_text_input_state's types (Chromium's ui::TextInputType). */
typedef enum ne_text_input_type {
  NE_TEXT_INPUT_NONE = 0,
  NE_TEXT_INPUT_TEXT = 1,
  NE_TEXT_INPUT_PASSWORD = 2,          /* no input method */
  /* 3 and up: search, email, number, telephone, url, dates, text areas,
   * content-editable elements: all take text. */
} ne_text_input_type;

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
  /* Phase 1: the wheel's momentum phase (blink's values: began 1,
   * changed 4, ended 8; 0 none). With `phase` and this both 0 the engine
   * adds phases itself, as for a mouse wheel. */
  int32_t momentum_phase;
  /* Phase 1: the characters ignoring modifiers, for key events. */
  ne_string_view unmodified_text;
  /* Phase 1: ne_input_flags. */
  uint32_t flags;
} ne_input_event;

/* Phase 1: ne_input_event's flags. */
typedef enum ne_input_flags {
  /* A key press whose text the app sends itself (NE_INPUT_CHAR, or an input
   * method's ne_tab_ime_commit), as AppKit views do through -insertText:. */
  NE_INPUT_FLAG_NO_CHAR = 1 << 0,
} ne_input_flags;

NE_EXPORT void ne_tab_send_input(ne_tab* tab, const ne_input_event* event);
NE_EXPORT void ne_tab_set_focus(ne_tab* tab, bool focused);

/* Phase 1: input methods (marked text) for the focused text field. Offsets
 * count UTF-16 code units. A replacement range of -1, -1 means the current
 * composition or selection. */
NE_EXPORT void ne_tab_ime_set_composition(ne_tab* tab, ne_string_view text,
                                          int32_t selection_start,
                                          int32_t selection_end,
                                          int32_t replace_start,
                                          int32_t replace_end);
NE_EXPORT void ne_tab_ime_commit(ne_tab* tab, ne_string_view text,
                                 int32_t replace_start, int32_t replace_end);
/* Keeps the composition as typed. */
NE_EXPORT void ne_tab_ime_finish(ne_tab* tab);
NE_EXPORT void ne_tab_ime_cancel(ne_tab* tab);
/* The caret's (or composition's) rectangle in the view, in DIPs, for the
 * input method's candidate window. False when there is none. */
NE_EXPORT bool ne_tab_ime_caret_rect(ne_tab* tab, ne_rect* out_rect);

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

typedef enum ne_menu_item_kind {
  NE_MENU_ITEM_COMMAND = 0,
  NE_MENU_ITEM_CHECKABLE = 1,
  NE_MENU_ITEM_GROUP = 2,             /* a <select>'s <optgroup> label */
  NE_MENU_ITEM_SEPARATOR = 3,
  NE_MENU_ITEM_SUBMENU = 4,
} ne_menu_item_kind;

typedef struct ne_menu_item {
  int32_t command_id;                 /* -1 for a separator; a <select>'s item index */
  ne_string_view label;
  bool enabled;
  bool checked;
  int32_t submenu_index;              /* index into the request's items, or -1 */
  /* Phase 1 (ne_menu_item has no size field: no release used the shorter
   * layout). */
  int32_t kind;                       /* ne_menu_item_kind */
  ne_string_view tooltip;
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
  /* Phase 1: */
  ne_string_view url;                 /* link (context menu), download, certificate or auth URL */
  ne_string_view src_url;             /* context menu: the image or media's URL */
  /* NE_UI_NEW_TAB: the new page. Accepting adopts it (the app closes it with
   * ne_tab_close later); declining closes it. Null if the engine can't
   * offer it (the app may still load `message`, the target URL). */
  ne_tab* new_tab;
  int32_t disposition;                /* NE_UI_NEW_TAB: ne_disposition */
  int32_t file_mode;                  /* NE_UI_FILE_CHOOSER: ne_file_mode */
  int32_t media_type;                 /* NE_UI_CONTEXT_MENU: ne_media_type */
  bool is_editable;                   /* NE_UI_CONTEXT_MENU: in a text field */
  bool can_proceed;                   /* NE_UI_CERTIFICATE_ERROR: false under HSTS */
  int64_t total_bytes;                /* NE_UI_DOWNLOAD: -1 if unknown */
  uint64_t download_id;               /* NE_UI_DOWNLOAD */
  ne_string_view mime_type;           /* NE_UI_DOWNLOAD */
} ne_ui_request;

/* Phase 1: NE_UI_NEW_TAB's dispositions. */
typedef enum ne_disposition {
  NE_DISPOSITION_FOREGROUND_TAB = 1,
  NE_DISPOSITION_BACKGROUND_TAB = 2,
  NE_DISPOSITION_POPUP = 3,           /* window.open with features */
  NE_DISPOSITION_WINDOW = 4,
} ne_disposition;

/* Phase 1: NE_UI_FILE_CHOOSER's modes. */
typedef enum ne_file_mode {
  NE_FILE_OPEN = 0,
  NE_FILE_OPEN_MULTIPLE = 1,
  NE_FILE_OPEN_FOLDER = 2,
  NE_FILE_SAVE = 3,
} ne_file_mode;

/* Phase 1: NE_UI_CONTEXT_MENU's media types. */
typedef enum ne_media_type {
  NE_MEDIA_NONE = 0,
  NE_MEDIA_IMAGE = 1,
  NE_MEDIA_VIDEO = 2,
  NE_MEDIA_AUDIO = 3,
  NE_MEDIA_CANVAS = 4,
  NE_MEDIA_OTHER = 5,
} ne_media_type;

/* Phase 1: the context menu's commands. The engine lists the ones that
 * apply in the request's items and carries out those it can when the app
 * answers with one; commands of the app's own (any other id, such as
 * NE_CONTEXT_OPEN_LINK_IN_NEW_TAB) are the app's to do. */
typedef enum ne_context_command {
  NE_CONTEXT_BACK = 1,
  NE_CONTEXT_FORWARD = 2,
  NE_CONTEXT_RELOAD = 3,
  NE_CONTEXT_OPEN_LINK_IN_NEW_TAB = 10,   /* the app's */
  NE_CONTEXT_COPY_LINK = 11,
  NE_CONTEXT_SAVE_LINK = 12,
  NE_CONTEXT_COPY_IMAGE = 20,
  NE_CONTEXT_COPY_IMAGE_URL = 21,
  NE_CONTEXT_SAVE_IMAGE = 22,
  NE_CONTEXT_OPEN_IMAGE_IN_NEW_TAB = 23,  /* the app's */
  NE_CONTEXT_UNDO = 30,
  NE_CONTEXT_REDO = 31,
  NE_CONTEXT_CUT = 32,
  NE_CONTEXT_COPY = 33,
  NE_CONTEXT_PASTE = 34,
  NE_CONTEXT_SELECT_ALL = 35,
  NE_CONTEXT_PASTE_AND_MATCH_STYLE = 36,
} ne_context_command;

/* Phase 1: download progress (on_download_updated). */
typedef enum ne_download_state {
  NE_DOWNLOAD_IN_PROGRESS = 0,
  NE_DOWNLOAD_COMPLETE = 1,
  NE_DOWNLOAD_CANCELLED = 2,
  NE_DOWNLOAD_INTERRUPTED = 3,
} ne_download_state;

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
  /* Phase 1: a console message from one of the tab's frames. Return true
   * when the app consumed it (it isn't logged). */
  bool (*on_console_message)(void* user_data, ne_tab* tab, ne_frame_id frame,
                             int32_t level, ne_string_view message,
                             ne_string_view source, int32_t line);
  /* Phase 1: loading progress per frame (ne_load_event_kind). */
  void (*on_load_event)(void* user_data, ne_tab* tab, ne_frame_id frame,
                        bool is_main, int32_t kind, ne_string_view url);
  /* Phase 1: a download the app accepted moved on (ne_download_state). */
  void (*on_download_updated)(void* user_data, uint64_t download_id,
                              int32_t state, int64_t received_bytes,
                              int64_t total_bytes, ne_string_view path);
  /* Phase 1: the page's icon, as PNG bytes (valid during the call). */
  void (*on_favicon)(void* user_data, ne_tab* tab, ne_string_view icon_url,
                     const uint8_t* png, size_t png_size);
} ne_ui_delegate;

/* Phase 1: on_load_event's kinds. */
typedef enum ne_load_event_kind {
  /* The main frame started a cross-document navigation (not yet committed). */
  NE_LOAD_NAVIGATION_STARTED = 1,
  /* A frame committed a new document; scripts sent now run in it. */
  NE_LOAD_COMMITTED = 2,
  /* A frame's document and its subresources finished loading. */
  NE_LOAD_FINISHED = 3,
} ne_load_event_kind;

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

/* Phase 1: stops a download the app accepted. */
NE_EXPORT void ne_download_cancel(ne_app* app, uint64_t download_id);

/* Phase 1: the zoom factor a tab shows (1.0 = 100%). */
NE_EXPORT double ne_tab_get_zoom(ne_tab* tab);

/* ------------------------------------------------------------------------ */
/* Phase 1: content blocking and document-start scripts (plan.md §4.6)       */

/* A tab's id: stable, never reused, and usable on any thread. */
NE_EXPORT uint64_t ne_tab_id(ne_tab* tab);

typedef enum ne_filter_decision {
  NE_FILTER_ALLOW = 0,
  NE_FILTER_BLOCK = 1,
  /* Answer with the filter's own response (a "redirect" stand-in). */
  NE_FILTER_RESPOND = 2,
} ne_filter_decision;

typedef struct ne_request_info {
  size_t size;
  uint64_t tab_id;                    /* ne_tab_id() of the page; 0 if none */
  ne_string_view url;
  ne_string_view initiator;           /* the requesting origin; empty if none */
  /* adblock-rust's names: "sub_frame", "stylesheet", "script", "image",
   * "font", "object", "media", "xhr", "ping", "csp_report", "other" */
  ne_string_view resource_type;
  ne_string_view method;              /* upper case */
} ne_request_info;

typedef struct ne_filter_response {
  size_t size;
  ne_string_view mime_type;
  const uint8_t* body;                /* valid until the filter's next call on this thread */
  size_t body_size;
} ne_filter_response;

/* Decides each subresource and subframe request. Top-level pages are never
 * filtered. Called on the engine's IO thread, NOT the main thread: it must
 * be fast and thread-safe. Null removes the filter. */
typedef int32_t (*ne_request_filter_fn)(void* user_data,
                                        const ne_request_info* request,
                                        ne_filter_response* out_response);
NE_EXPORT void ne_app_set_request_filter(ne_app* app, ne_request_filter_fn fn,
                                         void* user_data);

/* The script a frame runs in its main world before any of the page's own,
 * as its document's script context is created (scriptlets, Dark Reader).
 * Called on the main thread as each frame is about to commit a document.
 * `frame` is -1 for the frames a document creates without navigating
 * (about:blank children), whose script the engine asks for with the
 * parent's. Return a NUL-terminated string valid until the next call, or
 * NULL for none. */
typedef const char* (*ne_document_start_fn)(void* user_data, ne_tab* tab,
                                            ne_frame_id frame,
                                            ne_string_view frame_url,
                                            ne_string_view top_url,
                                            bool is_main);
NE_EXPORT void ne_app_set_document_start(ne_app* app, ne_document_start_fn fn,
                                         void* user_data);

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
