// Copyright 2026 Notolibra. All rights reserved.
//
// Hidden smoke check of NotolibraEngine.framework from a plain AppKit host
// (no gpui): init inside -[NSApplication run], attach mode, a tab, frames as
// IOSurfaces, navigation callbacks, script, JavaScript dialogs and a
// permission prompt as data. No windows; activation policy "prohibited".
//
//   test/engine/run_smoke [out_dir]
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.

#import <AppKit/AppKit.h>
#include <IOSurface/IOSurface.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ne_engine.h"

static ne_app* g_app;
static ne_tab* g_tab;
static int g_frames;
static uint64_t g_last_frame;
static size_t g_frame_w, g_frame_h;
static double g_first_frame_s = -1, g_start_s;
static int g_failures;
static std::vector<std::string> g_requests;
static std::string g_title;

static double Now() { return [[NSProcessInfo processInfo] systemUptime]; }

static ne_string_view View(const char* s) { return {s, strlen(s)}; }
static std::string Str(ne_string_view v) { return v.data ? std::string(v.data, v.length) : ""; }

static void Check(bool ok, const char* what, const std::string& detail = "") {
  printf("%s %s %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
  fflush(stdout);
  if (!ok) g_failures++;
}

static void OnFrame(void*, const ne_frame* frame) {
  if (g_first_frame_s < 0) g_first_frame_s = Now() - g_start_s;
  g_frames++;
  g_frame_w = IOSurfaceGetWidth((IOSurfaceRef)frame->surface);
  g_frame_h = IOSurfaceGetHeight((IOSurfaceRef)frame->surface);
  if (g_last_frame) ne_tab_release_frame(g_tab, g_last_frame);
  g_last_frame = frame->frame_id;
}

static void OnRequest(void*, const ne_ui_request* request) {
  std::string line = std::to_string(request->kind) + ":" + Str(request->message) + "|" + Str(request->detail);
  g_requests.push_back(line);
  ne_ui_response response = {sizeof(ne_ui_response)};
  // Answer from a later run-loop turn, as a gpui dialog would.
  ne_request_id id = request->id;
  int kind = request->kind;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    ne_ui_response r = {sizeof(ne_ui_response)};
    r.accept = kind != NE_UI_PERMISSION;  // dialogs accepted, permission denied
    const char* text = "typed";
    r.text = View(text);
    ne_ui_respond(g_app, id, &r);
  });
  (void)response;
}

static void OnNav(void*, ne_tab*, ne_string_view url, ne_string_view title, bool loading, bool, bool) {
  g_title = Str(title);
}

static std::string g_eval;
static void OnEval(void*, ne_status status, const char* json) { g_eval = json ? json : "(null)"; }

static void Step(int n);

static void After(double seconds, int n) {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{ Step(n); });
}

static void Step(int n) {
  switch (n) {
    case 0: {  // Wait for frames.
      if (g_frames < 10 && Now() - g_start_s < 20) { After(0.1, 0); return; }
      Check(g_frames >= 10, "frames-arrive", "frames=" + std::to_string(g_frames) + " first_after_s=" + std::to_string(g_first_frame_s) +
            " size=" + std::to_string(g_frame_w) + "x" + std::to_string(g_frame_h));
      Check(g_title == "smoke", "title-callback", "title=" + g_title);
      // Frame rate with a CSS animation: count frames over 2 s.
      g_frames = 0;
      After(2.0, 1);
      return;
    }
    case 1:
      Check(g_frames >= 60, "frame-rate", "frames_in_2s=" + std::to_string(g_frames));
      ne_tab_eval(g_tab, View("document.title + ':' + (1+2)"), NE_EVAL_TEST, OnEval, nullptr);
      After(0.5, 2);
      return;
    case 2:
      Check(g_eval == "\"smoke:3\"", "eval", g_eval);
      ne_tab_eval(g_tab, View("String([alert('hello'), confirm('sure?'), prompt('name?', 'x')])"), NE_EVAL_TEST, OnEval, nullptr);
      After(1.5, 3);
      return;
    case 3:
      Check(g_requests.size() >= 3 && g_requests[0] == "1:hello|" && g_requests[1] == "2:sure?|" && g_requests[2] == "3:name?|x",
            "js-dialogs-as-data", g_requests.empty() ? "" : g_requests[0] + " " + (g_requests.size() > 2 ? g_requests[2] : ""));
      Check(g_eval == "\",true,typed\"", "js-dialog-answers", g_eval);
      g_requests.clear();
      ne_tab_eval(g_tab, View("Notification.requestPermission().then(r => window.perm = r); 1"), NE_EVAL_TEST, OnEval, nullptr);
      After(1.5, 4);
      return;
    case 4:
      Check(!g_requests.empty() && g_requests[0].rfind("5:", 0) == 0, "permission-as-data", g_requests.empty() ? "none" : g_requests[0]);
      // Typing into a field through ne_tab_send_input.
      ne_tab_set_focus(g_tab, true);
      ne_tab_eval(g_tab, View("document.querySelector('input').focus(); 1"), NE_EVAL_TEST, OnEval, nullptr);
      After(0.3, 5);
      return;
    case 5: {
      const char* keys[] = {"h", "i"};
      unsigned short codes[] = {4, 34};  // kVK_ANSI_H, kVK_ANSI_I
      for (int i = 0; i < 2; ++i) {
        ne_input_event e = {sizeof(ne_input_event)};
        e.kind = NE_INPUT_KEY_DOWN;
        e.native_key_code = codes[i];
        e.text = View(keys[i]);
        ne_tab_send_input(g_tab, &e);
        e.kind = NE_INPUT_KEY_UP;
        ne_tab_send_input(g_tab, &e);
      }
      // A click at the button, which counts clicks.
      ne_input_event m = {sizeof(ne_input_event)};
      m.x = 30; m.y = 80; m.click_count = 1; m.kind = NE_INPUT_MOUSE_MOVE; ne_tab_send_input(g_tab, &m);
      m.kind = NE_INPUT_MOUSE_DOWN; ne_tab_send_input(g_tab, &m);
      m.kind = NE_INPUT_MOUSE_UP; ne_tab_send_input(g_tab, &m);
      ne_input_event w = {sizeof(ne_input_event)};
      w.kind = NE_INPUT_WHEEL; w.x = 100; w.y = 300; w.delta_y = -400; w.precise = true;
      ne_tab_send_input(g_tab, &w);
      After(0.8, 6);
      return;
    }
    case 6:
      ne_tab_eval(g_tab, View("document.querySelector('input').value + '|' + window.clicks + '|' + Math.round(scrollY) + '|' + window.perm"), NE_EVAL_TEST, OnEval, nullptr);
      After(0.4, 7);
      return;
    case 7: {
      Check(g_eval.rfind("\"hi|1|", 0) == 0, "input-keys-click", g_eval);
      Check(g_eval.find("|0|") == std::string::npos && g_eval.find('|') != std::string::npos, "input-wheel", g_eval);
      Check(g_eval.find("|denied\"") != std::string::npos, "permission-answer", g_eval);
      double t = Now();
      ne_app_try_terminate(g_app, nullptr, nullptr);
      printf("shutdown_s=%.2f\n", Now() - t);
      printf("%s: %d failures\n", g_failures ? "FAIL" : "PASS", g_failures);
      fflush(stdout);
      exit(g_failures ? 1 : 0);
    }
  }
}

@interface Delegate : NSObject <NSApplicationDelegate>
@end

@implementation Delegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
  g_start_s = Now();
  printf("engine %s, api %d\n", ne_chromium_version(), ne_api_version());
  std::string profile = std::string(getenv("TMPDIR") ?: "/tmp/") + "nl-engine-smoke-profile";
  const char* argv[] = {"--enable-logging=stderr"};
  ne_app_config config = {sizeof(ne_app_config)};
  config.api_version = NE_API_VERSION;
  config.user_data_dir = {profile.data(), profile.size()};
  config.argc = 1;
  config.argv = argv;
  double t = Now();
  ne_status status = ne_app_init(&config, &g_app);
  Check(status == NE_OK, "init", "status=" + std::to_string(status) + " init_s=" + std::to_string(Now() - t));
  if (status != NE_OK) exit(1);
  Check(ne_app_attach_run_loop(g_app) == NE_OK, "attach-run-loop");
  ne_ui_delegate delegate = {sizeof(ne_ui_delegate)};
  delegate.on_request = OnRequest;
  delegate.on_navigation_state = OnNav;
  ne_app_set_ui_delegate(g_app, &delegate);
  ne_profile_open(g_app, View("Default"), [](void*, ne_status status, ne_profile* profile) {
    Check(status == NE_OK && profile, "profile-open");
    ne_window* window = nullptr;
    ne_window_create(profile, nullptr, &window);
    const char* html =
        "data:text/html,<title>smoke</title><style>@keyframes s{to{transform:rotate(360deg)}}"
        "div{width:80px;height:80px;background:teal;animation:s 1s linear infinite}</style>"
        "<input><button onclick='window.clicks=(window.clicks||0)+1' style='position:absolute;left:10px;top:70px;width:80px;height:30px'>b</button>"
        "<div style='margin-top:60px'></div><div style='height:3000px'></div><script>window.clicks=0</script>";
    ne_tab_config tab_config = {sizeof(ne_tab_config)};
    tab_config.url = View(html);
    ne_tab_create(window, &tab_config, &g_tab);
    ne_tab_set_view_size(g_tab, 800, 600, 2);
    ne_presenter presenter = {sizeof(ne_presenter)};
    presenter.on_frame = OnFrame;
    ne_tab_set_presenter(g_tab, &presenter);
    After(0.1, 0);
  }, nullptr);
}
@end

int main(int argc, const char** argv) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    Delegate* delegate = [[Delegate alloc] init];
    NSApp.delegate = delegate;
    // A hard stop, so a hang can't keep the check alive.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 90 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
      printf("FAIL timeout\n");
      exit(2);
    });
    [NSApp run];
  }
  return 0;
}
