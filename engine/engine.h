// Copyright 2026 Notolibra. All rights reserved.
//
// Engine-wide state behind api/ne_engine.h. Everything here lives on the
// browser's UI thread, which is the host app's main thread.

#ifndef NOTOLIBRA_ENGINE_ENGINE_H_
#define NOTOLIBRA_ENGINE_ENGINE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "ne_engine.h"

namespace content {
class ContentMainRunner;
}

namespace notolibra {

class BrowserContext;
class MainDelegate;
class Tab;

// Answers a UI request: `response` is null when the request was cancelled or
// the app has no delegate (treat as declined).
using UiResponseCallback = base::OnceCallback<void(const ne_ui_response*)>;

struct Engine {
  Engine();
  ~Engine();

  std::unique_ptr<MainDelegate> main_delegate;
  std::unique_ptr<content::ContentMainRunner> content_runner;
  base::FilePath user_data_dir;
  std::string keychain_service;
  // Keeps argv alive for base::CommandLine.
  std::vector<std::string> argv_storage;
  std::vector<const char*> argv;

  bool attached = false;
  bool shut_down = false;

  ne_ui_delegate ui_delegate = {};
  bool has_ui_delegate = false;

  // The app's document-start scripts (ne_app_set_document_start).
  ne_document_start_fn document_start = nullptr;
  raw_ptr<void> document_start_data = nullptr;

  ne_request_id next_request_id = 1;
  struct Pending {
    raw_ptr<Tab> tab;
    UiResponseCallback callback;
  };
  base::flat_map<ne_request_id, Pending> pending_requests;

  std::vector<Tab*> tabs;

  // Sends `request` to the app (filling in its id) and remembers `callback`.
  // Without a UI delegate the callback runs at once with a null response.
  // Returns the request's id.
  ne_request_id SendUiRequest(ne_ui_request& request, Tab* tab,
                              UiResponseCallback callback);
  // Runs a pending request's callback with `response` (null: declined).
  void Respond(ne_request_id id, const ne_ui_response* response);
  // Withdraws one pending request: tells the app and declines it.
  void CancelRequest(ne_request_id id);
  bool IsPending(ne_request_id id) const {
    return pending_requests.contains(id);
  }
  // Cancels a tab's pending requests (navigation, close): tells the app and
  // declines them.
  void CancelRequestsFor(Tab* tab);
};

// The single engine, created by ne_app_init(); null before that.
Engine* GetEngine();

// Bundle paths, bundle id and the child process path, for the browser and the
// helper processes alike. Call after base's AtExitManager exists.
void SetUpBundlePaths();

// The framework bundle, found from this binary's own path.
base::FilePath FrameworkPath();

}  // namespace notolibra

struct ne_app {
  raw_ptr<notolibra::Engine> engine;
};

struct ne_profile {
  raw_ptr<notolibra::BrowserContext> context;
};

struct ne_window {
  raw_ptr<ne_profile> profile;
  raw_ptr<void> native_window;
};

struct ne_tab {
  std::unique_ptr<notolibra::Tab> impl;
};

#endif  // NOTOLIBRA_ENGINE_ENGINE_H_
