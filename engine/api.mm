// Copyright 2026 Notolibra. All rights reserved.
//
// The C API (api/ne_engine.h). Phase 0 implements the app, one profile,
// windows as containers, tabs with frames and input, JavaScript dialogs and
// permission prompts as UI requests. Unimplemented calls are no-ops or return
// NE_ERR_UNSUPPORTED.

#import <AppKit/AppKit.h>
#include <objc/runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "base/apple/foundation_util.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/mac/scoped_sending_event.h"
#include "base/message_loop/message_pump_apple.h"
#include "base/run_loop.h"
#include "base/strings/sys_string_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "content/public/app/content_main.h"
#include "content/public/app/content_main_runner.h"
#include "base/observer_list.h"
#include "content/public/browser/host_zoom_map.h"
#include "content/public/browser/native_event_processor_mac.h"
#include "content/public/browser/native_event_processor_observer_mac.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "notolibra/engine/browser_client.h"
#include "notolibra/engine/browser_context.h"
#include "notolibra/engine/engine.h"
#include "notolibra/engine/main_delegate.h"
#include "notolibra/engine/profile_import.h"
#include "notolibra/engine/request_filter.h"
#include "notolibra/engine/tab.h"
#include "third_party/blink/public/common/page/page_zoom.h"

namespace notolibra {

namespace {

Engine* g_engine = nullptr;
ne_profile* g_profile = nullptr;

// Copies an array of the caller's structs, each starting with its size (the
// array's stride), into ours: fields the caller's version lacks stay zero.
template <typename T>
std::vector<T> CopySizedArray(const T* items, size_t count) {
  std::vector<T> out(count);
  if (!items || count == 0 || items->size < sizeof(size_t)) {
    out.clear();
    return out;
  }
  const size_t stride = items->size;
  const auto* bytes = reinterpret_cast<const uint8_t*>(items);
  for (size_t i = 0; i < count; ++i) {
    // SAFETY: the caller passes `count` structs of `stride` bytes each.
    UNSAFE_BUFFERS(std::memcpy(&out[i], bytes + i * stride,
                               std::min(stride, sizeof(T))));
    out[i].size = sizeof(T);
  }
  return out;
}

std::string ToString(ne_string_view view) {
  return view.data ? std::string(view.data, view.length) : std::string();
}

// plan.md §2.4 step 2: Chromium's macOS pump wants NSApp to implement
// CrAppProtocol (-isHandlingSendEvent), and content's responsiveness watcher
// wants NativeEventProcessor (observers around each -sendEvent:). The host's
// application class (gpui's GPUIApplication) gets the methods at run time,
// and its -sendEvent: sets the flag and notifies the observers. A host that
// already added CrAppControlProtocol (today's CEF prototype) keeps its own
// -isHandlingSendEvent; NativeEventProcessor is added either way.
BOOL g_handling_send_event = NO;
base::ObserverList<content::NativeEventProcessorObserver>::Unchecked*
    g_event_observers = nullptr;

void InstallCrAppProtocol() {
  CHECK(NSApp) << "create the NSApplication before ne_app_init()";
  Class cls = [NSApp class];
  const bool has_cr_app =
      [NSApp conformsToProtocol:@protocol(CrAppControlProtocol)];
  g_event_observers =
      new base::ObserverList<content::NativeEventProcessorObserver>::Unchecked;
  class_addMethod(
      cls, @selector(addNativeEventProcessorObserver:),
      imp_implementationWithBlock(
          ^(id, content::NativeEventProcessorObserver* observer) {
            g_event_observers->AddObserver(observer);
          }),
      "v@:^v");
  class_addMethod(
      cls, @selector(removeNativeEventProcessorObserver:),
      imp_implementationWithBlock(
          ^(id, content::NativeEventProcessorObserver* observer) {
            g_event_observers->RemoveObserver(observer);
          }),
      "v@:^v");
  class_addProtocol(cls, @protocol(NativeEventProcessor));
  if (!has_cr_app) {
  class_addMethod(cls, @selector(isHandlingSendEvent),
                  imp_implementationWithBlock(^BOOL(id) {
                    return g_handling_send_event;
                  }),
                  "c@:");
  class_addMethod(cls, @selector(setHandlingSendEvent:),
                  imp_implementationWithBlock(^(id, BOOL value) {
                    g_handling_send_event = value;
                  }),
                  "v@:c");
  }
  Method send = class_getInstanceMethod(cls, @selector(sendEvent:));
  IMP original = method_getImplementation(send);
  IMP wrapped = imp_implementationWithBlock(^(id self, NSEvent* event) {
    BOOL was = g_handling_send_event;
    if (!has_cr_app) {
      g_handling_send_event = YES;
    }
    content::ScopedNotifyNativeEventProcessorObserver notifier(
        g_event_observers, event);
    reinterpret_cast<void (*)(id, SEL, NSEvent*)>(original)(
        self, @selector(sendEvent:), event);
    if (!has_cr_app) {
      g_handling_send_event = was;
    }
  });
  if (!class_addMethod(cls, @selector(sendEvent:), wrapped, "v@:@")) {
    method_setImplementation(send, wrapped);
  }
  if (!has_cr_app) {
    class_addProtocol(cls, @protocol(CrAppProtocol));
    class_addProtocol(cls, @protocol(CrAppControlProtocol));
  }
}

bool OnMainThread() {
  return [NSThread isMainThread];
}

}  // namespace

Engine::Engine() = default;
Engine::~Engine() = default;

Engine* GetEngine() {
  return g_engine;
}

ne_request_id Engine::SendUiRequest(ne_ui_request& request,
                                    Tab* tab,
                                    UiResponseCallback callback) {
  const ne_request_id id = next_request_id++;
  request.id = id;
  pending_requests.emplace(id, Pending{tab, std::move(callback)});
  if (has_ui_delegate && ui_delegate.on_request) {
    ui_delegate.on_request(ui_delegate.user_data, &request);
  } else {
    Respond(id, nullptr);
  }
  return id;
}

void Engine::CancelRequest(ne_request_id id) {
  if (!pending_requests.contains(id)) {
    return;
  }
  if (has_ui_delegate && ui_delegate.on_cancel) {
    ui_delegate.on_cancel(ui_delegate.user_data, id);
  }
  Respond(id, nullptr);
}

void Engine::Respond(ne_request_id id, const ne_ui_response* response) {
  auto it = pending_requests.find(id);
  if (it == pending_requests.end()) {
    return;
  }
  UiResponseCallback callback = std::move(it->second.callback);
  pending_requests.erase(it);
  std::move(callback).Run(response);
}

void Engine::CancelRequestsFor(Tab* tab) {
  std::vector<ne_request_id> ids;
  for (const auto& [id, pending] : pending_requests) {
    if (pending.tab == tab) {
      ids.push_back(id);
    }
  }
  for (ne_request_id id : ids) {
    if (has_ui_delegate && ui_delegate.on_cancel) {
      ui_delegate.on_cancel(ui_delegate.user_data, id);
    }
    Respond(id, nullptr);
  }
}

}  // namespace notolibra

using notolibra::Engine;
using notolibra::g_engine;
using notolibra::g_profile;
using notolibra::ToString;

extern "C" {

NE_EXPORT int32_t ne_api_version(void) {
  return NE_API_VERSION;
}

NE_EXPORT const char* ne_chromium_version(void) {
  return NE_CHROMIUM_VERSION;
}

NE_EXPORT ne_status ne_app_init(const ne_app_config* config,
                                ne_app** out_app) {
  if (!notolibra::OnMainThread()) {
    return NE_ERR_WRONG_THREAD;
  }
  // Configs from before pdf_directory was appended are accepted.
  if (!config || !out_app ||
      config->size < offsetof(ne_app_config, pdf_directory)) {
    return NE_ERR_INVALID_ARGUMENT;
  }
  if (config->api_version != NE_API_VERSION) {
    return NE_ERR_API_VERSION;
  }
  if (g_engine) {
    return NE_ERR_FAILED;
  }
  notolibra::InstallCrAppProtocol();

  g_engine = new Engine;
  Engine& engine = *g_engine;
  std::string user_data_dir = ToString(config->user_data_dir);
  if (user_data_dir.empty()) {
    return NE_ERR_INVALID_ARGUMENT;
  }
  engine.user_data_dir = base::FilePath(user_data_dir);
  if (!base::CreateDirectory(engine.user_data_dir.Append("Default"))) {
    return NE_ERR_FAILED;
  }
  engine.keychain_service = ToString(config->keychain_service);
  if (config->size >= offsetof(ne_app_config, pdf_directory) +
                          sizeof(config->pdf_directory)) {
    const std::string pdf_directory = ToString(config->pdf_directory);
    if (!pdf_directory.empty()) {
      engine.pdf_directory = base::FilePath(pdf_directory);
    }
  }

  // argv[0] is the host's executable. Child processes run in Chromium's
  // macOS sandbox (helper_main.cc); a caller may still pass --no-sandbox.
  // Cookies are encrypted with the key in the app's own keychain item
  // (keychain_service); without one (verification runs) the keychain is a
  // mock and nothing reaches the user's keychain.
  NSString* executable = NSProcessInfo.processInfo.arguments.firstObject;
  engine.argv_storage.push_back(base::SysNSStringToUTF8(executable));
  if (engine.keychain_service.empty()) {
    engine.argv_storage.push_back("--use-mock-keychain");
  }
  // SAFETY: the caller passes argc entries in argv.
  auto extra_args = UNSAFE_BUFFERS(
      base::span(config->argv, static_cast<size_t>(std::max(config->argc, 0))));
  for (const char* arg : extra_args) {
    engine.argv_storage.push_back(arg);
  }
  for (const std::string& arg : engine.argv_storage) {
    engine.argv.push_back(arg.c_str());
  }
  engine.argv.push_back(nullptr);

  engine.main_delegate = std::make_unique<notolibra::MainDelegate>();
  content::ContentMainParams params(engine.main_delegate.get());
  params.argc = static_cast<int>(engine.argv.size() - 1);
  params.argv = engine.argv.data();
  params.embedder_owns_run_loop = true;
  engine.content_runner = content::ContentMainRunner::Create();
  int exit_code =
      content::RunContentProcess(std::move(params), engine.content_runner.get());
  if (exit_code != 0) {
    LOG(ERROR) << "content startup failed: " << exit_code;
    return NE_ERR_FAILED;
  }
  // The app's own features run scripts in pages' main worlds (the gpui
  // scrollbar's reports, Dark Reader's live updates), as CEF's
  // ExecuteJavaScript allows.
  content::RenderFrameHost::AllowInjectingJavaScript();
  *out_app = new ne_app{g_engine};
  return NE_OK;
}

NE_EXPORT ne_status ne_app_attach_run_loop(ne_app* app) {
  if (!notolibra::OnMainThread()) {
    return NE_ERR_WRONG_THREAD;
  }
  if (!app || !app->engine) {
    return NE_ERR_NOT_INITIALIZED;
  }
  if (!app->engine->attached) {
    base::AttachCurrentUIThreadToNSApplicationLoop();
    app->engine->attached = true;
  }
  return NE_OK;
}

NE_EXPORT int64_t ne_app_pump(ne_app* app) {
  // The fallback pump (plan.md §2.4 step 4). Re-entrant: it spins AppKit's
  // event loop until Chromium is idle.
  if (!app || !app->engine || app->engine->attached) {
    return -1;
  }
  base::RunLoop().RunUntilIdle();
  return -1;
}

NE_EXPORT void ne_app_set_pump_wakeup(ne_app* app,
                                      ne_pump_wakeup_fn fn,
                                      void* user_data) {
  // Not implemented: attach mode needs no wakeups.
}

NE_EXPORT ne_terminate_reply ne_app_try_terminate(ne_app* app,
                                                  ne_terminate_fn done,
                                                  void* user_data) {
  if (!app || !app->engine || app->engine->shut_down) {
    return NE_TERMINATE_NOW;
  }
  Engine& engine = *app->engine;
  // Phase 0 closes pages without running beforeunload.
  while (!engine.tabs.empty()) {
    ne_tab* handle = engine.tabs.back()->handle();
    delete handle;
  }
  engine.main_delegate->ShutdownBrowser();
  engine.content_runner->Shutdown();
  engine.shut_down = true;
  return NE_TERMINATE_NOW;
}

NE_EXPORT void ne_profile_open(ne_app* app,
                               ne_string_view relative_path,
                               ne_profile_open_fn done,
                               void* user_data) {
  // Phase 0 has one profile, created at startup under user_data_dir/Default.
  ne_status status = NE_OK;
  if (!g_profile) {
    notolibra::BrowserContext* context =
        app && app->engine
            ? app->engine->main_delegate->browser_client()->browser_context()
            : nullptr;
    if (context) {
      g_profile = new ne_profile{context};
    } else {
      status = NE_ERR_NOT_INITIALIZED;
    }
  }
  ne_profile* profile = g_profile;
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(
                     [](ne_profile_open_fn done, void* user_data,
                        ne_status status, ne_profile* profile) {
                       if (done) {
                         done(user_data, status, profile);
                       }
                     },
                     done, user_data, status, profile));
}

NE_EXPORT void ne_profile_close(ne_profile* profile) {}

NE_EXPORT ne_status ne_window_create(ne_profile* profile,
                                     void* native_window,
                                     ne_window** out_window) {
  if (!profile || !out_window) {
    return NE_ERR_INVALID_ARGUMENT;
  }
  *out_window = new ne_window{profile, native_window};
  return NE_OK;
}

NE_EXPORT void ne_window_destroy(ne_window* window) {
  delete window;
}

NE_EXPORT ne_status ne_tab_create(ne_window* window,
                                  const ne_tab_config* config,
                                  ne_tab** out_tab) {
  if (!notolibra::OnMainThread()) {
    return NE_ERR_WRONG_THREAD;
  }
  // Apps built before surface_color_argb pass the shorter struct.
  if (!window || !config || !out_tab ||
      config->size < offsetof(ne_tab_config, surface_color_argb)) {
    return NE_ERR_INVALID_ARGUMENT;
  }
  ne_tab_config full = {};
  // SAFETY: the caller's struct starts with its own size; at most that
  // much (and no more than ours) is copied.
  UNSAFE_BUFFERS(std::memcpy(&full, config,
                             std::min(config->size, sizeof(full))));
  auto* tab = new ne_tab;
  tab->impl = std::make_unique<notolibra::Tab>(tab, window->profile->context,
                                               full);
  *out_tab = tab;
  return NE_OK;
}

NE_EXPORT void ne_tab_close(ne_tab* tab) {
  delete tab;
}

NE_EXPORT void ne_tab_load(ne_tab* tab, ne_string_view url) {
  if (tab) {
    tab->impl->Load(ToString(url));
  }
}

NE_EXPORT void ne_tab_go_back(ne_tab* tab) {
  if (tab && tab->impl->web_contents()->GetController().CanGoBack()) {
    tab->impl->web_contents()->GetController().GoBack();
  }
}

NE_EXPORT void ne_tab_go_forward(ne_tab* tab) {
  if (tab && tab->impl->web_contents()->GetController().CanGoForward()) {
    tab->impl->web_contents()->GetController().GoForward();
  }
}

NE_EXPORT void ne_tab_reload(ne_tab* tab, bool bypass_cache) {
  if (tab) {
    tab->impl->web_contents()->GetController().Reload(
        bypass_cache ? content::ReloadType::BYPASSING_CACHE
                     : content::ReloadType::NORMAL,
        false);
  }
}

NE_EXPORT void ne_tab_stop(ne_tab* tab) {
  if (tab) {
    tab->impl->web_contents()->Stop();
  }
}

NE_EXPORT void ne_tab_set_zoom(ne_tab* tab, double factor) {
  if (tab && factor > 0) {
    content::HostZoomMap::SetZoomLevel(tab->impl->web_contents(),
                                       blink::ZoomFactorToZoomLevel(factor));
  }
}

NE_EXPORT void ne_tab_set_visible(ne_tab* tab, bool visible) {
  if (tab) {
    tab->impl->SetVisible(visible);
  }
}

NE_EXPORT void ne_tab_set_surface_color(ne_tab* tab, uint32_t argb) {
  if (tab) {
    tab->impl->SetSurfaceColor(argb);
  }
}

NE_EXPORT void ne_tab_discard(ne_tab* tab) {}

NE_EXPORT void ne_tab_find(ne_tab* tab, ne_string_view text, uint32_t flags) {
  if (tab) {
    tab->impl->Find(ToString(text), flags);
  }
}

NE_EXPORT void ne_tab_stop_finding(ne_tab* tab, bool keep_selection) {
  if (tab) {
    tab->impl->StopFinding(keep_selection);
  }
}

NE_EXPORT void ne_tab_eval(ne_tab* tab,
                           ne_string_view script,
                           ne_eval_reason reason,
                           ne_eval_fn done,
                           void* user_data) {
  if (!tab) {
    if (done) {
      done(user_data, NE_ERR_INVALID_ARGUMENT, nullptr);
    }
    return;
  }
  tab->impl->Eval(ToString(script), reason, done, user_data);
}

NE_EXPORT void ne_tab_eval_in_frame(ne_tab* tab,
                                    ne_frame_id frame,
                                    ne_string_view script,
                                    ne_world world,
                                    ne_eval_fn done,
                                    void* user_data) {
  if (!tab) {
    if (done) {
      done(user_data, NE_ERR_INVALID_ARGUMENT, nullptr);
    }
    return;
  }
  tab->impl->EvalInFrame(frame, ToString(script), world, done, user_data);
}

NE_EXPORT void ne_tab_for_each_frame(ne_tab* tab,
                                     ne_frame_fn fn,
                                     void* user_data) {
  if (tab) {
    tab->impl->ForEachFrame(fn, user_data);
  }
}

NE_EXPORT void ne_tab_set_presenter(ne_tab* tab,
                                    const ne_presenter* presenter) {
  if (tab) {
    tab->impl->SetPresenter(presenter);
  }
}

NE_EXPORT void ne_tab_set_view_size(ne_tab* tab,
                                    double width,
                                    double height,
                                    double scale_factor) {
  if (tab) {
    tab->impl->SetViewSize(width, height, scale_factor);
  }
}

NE_EXPORT void ne_tab_release_frame(ne_tab* tab, uint64_t frame_id) {
  if (tab) {
    tab->impl->ReleaseFrame(frame_id);
  }
}

NE_EXPORT void ne_tab_send_input(ne_tab* tab, const ne_input_event* event) {
  // An older caller's shorter struct: the fields it lacks stay zero.
  constexpr size_t kPhase0Size = offsetof(ne_input_event, momentum_phase);
  if (!tab || !event || event->size < kPhase0Size) {
    return;
  }
  ne_input_event copy = {};
  // SAFETY: the caller's struct starts with its own size; at most that much
  // (and no more than ours) is copied.
  UNSAFE_BUFFERS(std::memcpy(&copy, event,
                             std::min(event->size, sizeof(ne_input_event))));
  copy.size = sizeof(ne_input_event);
  tab->impl->SendInput(copy);
}

NE_EXPORT void ne_tab_set_focus(ne_tab* tab, bool focused) {
  if (tab) {
    tab->impl->SetFocus(focused);
  }
}

NE_EXPORT void ne_tab_ime_set_composition(ne_tab* tab,
                                          ne_string_view text,
                                          int32_t selection_start,
                                          int32_t selection_end,
                                          int32_t replace_start,
                                          int32_t replace_end) {
  if (tab) {
    tab->impl->ImeSetComposition(base::UTF8ToUTF16(ToString(text)),
                                 selection_start, selection_end,
                                 notolibra::ImeRange(replace_start, replace_end));
  }
}

NE_EXPORT void ne_tab_ime_commit(ne_tab* tab,
                                 ne_string_view text,
                                 int32_t replace_start,
                                 int32_t replace_end) {
  if (tab) {
    tab->impl->ImeCommit(base::UTF8ToUTF16(ToString(text)),
                         notolibra::ImeRange(replace_start, replace_end));
  }
}

NE_EXPORT void ne_tab_ime_finish(ne_tab* tab) {
  if (tab) {
    tab->impl->ImeFinish();
  }
}

NE_EXPORT void ne_tab_ime_cancel(ne_tab* tab) {
  if (tab) {
    tab->impl->ImeCancel();
  }
}

NE_EXPORT bool ne_tab_ime_caret_rect(ne_tab* tab, ne_rect* out_rect) {
  return tab && out_rect && tab->impl->ImeCaretRect(out_rect);
}

NE_EXPORT void ne_tab_edit(ne_tab* tab, ne_edit_command command) {
  if (tab) {
    tab->impl->Edit(command);
  }
}

NE_EXPORT void ne_app_set_ui_delegate(ne_app* app,
                                      const ne_ui_delegate* delegate) {
  if (!app || !app->engine) {
    return;
  }
  Engine& engine = *app->engine;
  engine.ui_delegate = {};
  engine.has_ui_delegate = delegate != nullptr;
  if (delegate) {
    // SAFETY: the caller's struct starts with its own size; at most
    // that much (and no more than ours) is copied.
    UNSAFE_BUFFERS(std::memcpy(&engine.ui_delegate, delegate,
                std::min(delegate->size, sizeof(engine.ui_delegate))));
  }
}

NE_EXPORT void ne_ui_respond(ne_app* app,
                             ne_request_id id,
                             const ne_ui_response* response) {
  if (app && app->engine) {
    app->engine->Respond(id, response);
  }
}

NE_EXPORT uint64_t ne_tab_id(ne_tab* tab) {
  return tab ? tab->impl->id() : 0;
}

NE_EXPORT void ne_app_set_request_filter(ne_app* app,
                                         ne_request_filter_fn fn,
                                         void* user_data) {
  notolibra::SetRequestFilter(fn, user_data);
}

NE_EXPORT void ne_app_set_document_start(ne_app* app,
                                         ne_document_start_fn fn,
                                         void* user_data) {
  if (app && app->engine) {
    app->engine->document_start = fn;
    app->engine->document_start_data = user_data;
  }
}

NE_EXPORT void ne_download_cancel(ne_app* app, uint64_t download_id) {
  if (g_profile && g_profile->context) {
    g_profile->context->CancelDownload(static_cast<uint32_t>(download_id));
  }
}

NE_EXPORT double ne_tab_get_zoom(ne_tab* tab) {
  if (!tab) {
    return 1.0;
  }
  return blink::ZoomLevelToZoomFactor(
      content::HostZoomMap::GetZoomLevel(tab->impl->web_contents()));
}

NE_EXPORT void ne_profile_set_feature(ne_profile* profile,
                                      ne_feature feature,
                                      bool enabled) {}

NE_EXPORT void ne_profile_set_site_feature(ne_profile* profile,
                                           ne_string_view host,
                                           ne_feature feature,
                                           bool enabled) {}

NE_EXPORT void ne_profile_import_cookies(ne_profile* profile,
                                         const ne_cookie* cookies,
                                         size_t count,
                                         ne_cookie_import_fn done,
                                         void* user_data) {
  if (!profile || !profile->context || (count && !cookies)) {
    if (done) {
      done(user_data, NE_ERR_INVALID_ARGUMENT, nullptr);
    }
    return;
  }
  std::vector<ne_cookie> copy = notolibra::CopySizedArray(cookies, count);
  notolibra::ImportCookies(profile->context, copy.data(), copy.size(), done,
                           user_data);
}

NE_EXPORT void ne_profile_import_local_storage(ne_profile* profile,
                                               const ne_storage_origin* origins,
                                               size_t count,
                                               bool overwrite,
                                               ne_storage_import_fn done,
                                               void* user_data) {
  if (!profile || !profile->context || (count && !origins)) {
    if (done) {
      done(user_data, NE_ERR_INVALID_ARGUMENT, nullptr);
    }
    return;
  }
  std::vector<ne_storage_origin> copy = notolibra::CopySizedArray(origins, count);
  notolibra::ImportLocalStorage(profile->context, copy.data(), copy.size(),
                                overwrite, done, user_data);
}

// The helper app's entry point (helper_main.cc): every child process.
NE_EXPORT int ne_helper_main(int argc, const char** argv) {
  notolibra::MainDelegate delegate;
  content::ContentMainParams params(&delegate);
  params.argc = argc;
  params.argv = argv;
  return content::ContentMain(std::move(params));
}

}  // extern "C"
