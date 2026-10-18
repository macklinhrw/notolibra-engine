// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/main_delegate.h"

#include <dlfcn.h>

#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/path_service.h"
#include "content/public/browser/browser_main_runner.h"
#include "content/public/common/content_paths.h"
#include "content/public/common/content_switches.h"
#include "content/public/renderer/content_renderer_client.h"
#include "notolibra/engine/browser_client.h"
#include "notolibra/engine/engine.h"
#include "notolibra/engine/renderer/renderer_client.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"

namespace notolibra {

namespace {
constexpr char kBundleId[] = "com.notolibra.engine";
constexpr char kHelperName[] = "Notolibra Engine Helper";
}  // namespace

base::FilePath FrameworkPath() {
  Dl_info info = {};
  CHECK(dladdr(reinterpret_cast<const void*>(&ne_api_version), &info));
  base::FilePath path(info.dli_fname);
  while (!path.empty() && path.Extension() != ".framework" &&
         path != path.DirName()) {
    path = path.DirName();
  }
  CHECK_EQ(path.Extension(), ".framework") << info.dli_fname;
  return path;
}

void SetUpBundlePaths() {
  base::FilePath framework = FrameworkPath();
  base::apple::SetOverrideFrameworkBundlePath(framework);
  // Children must share the browser's bundle id: the Mach port rendezvous
  // server is named after it.
  base::apple::SetBaseBundleIDOverride(kBundleId);
  base::FilePath helper = framework.Append("Helpers")
                              .Append(std::string(kHelperName) + ".app")
                              .Append("Contents")
                              .Append("MacOS")
                              .Append(kHelperName);
  base::PathService::OverrideAndCreateIfNeeded(
      content::CHILD_PROCESS_EXE, helper, /*is_absolute=*/true,
      /*create=*/false);
}

std::u16string ContentClient::GetLocalizedString(int message_id) {
  return l10n_util::GetStringUTF16(message_id);
}

std::string_view ContentClient::GetDataResource(
    int resource_id,
    ui::ResourceScaleFactor scale_factor) {
  return ui::ResourceBundle::GetSharedInstance().GetRawDataResourceForScale(
      resource_id, scale_factor);
}

scoped_refptr<base::RefCountedMemory> ContentClient::GetDataResourceBytes(
    int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceBytes(
      resource_id);
}

std::string ContentClient::GetDataResourceString(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
      resource_id);
}

gfx::Image& ContentClient::GetNativeImageNamed(int resource_id) {
  return ui::ResourceBundle::GetSharedInstance().GetNativeImageNamed(
      resource_id);
}

MainDelegate::MainDelegate() = default;
MainDelegate::~MainDelegate() = default;

std::optional<int> MainDelegate::BasicStartupComplete() {
  SetUpBundlePaths();
  return std::nullopt;
}

void MainDelegate::PreSandboxStartup() {
  NSString* pak = [base::apple::FrameworkBundle() pathForResource:@"notolibra_engine"
                                                           ofType:@"pak"];
  CHECK(pak) << "notolibra_engine.pak missing from "
             << base::apple::FrameworkBundlePath();
  ui::ResourceBundle::InitSharedInstanceWithPakPath(
      base::apple::NSStringToFilePath(pak));
}

std::variant<int, content::MainFunctionParams> MainDelegate::RunProcess(
    const std::string& process_type,
    content::MainFunctionParams main_function_params) {
  if (!process_type.empty()) {
    return std::move(main_function_params);
  }
  // The browser process: initialise, but don't run a loop. The UI thread's
  // task runner is attached to the host's run loop afterwards
  // (ne_app_attach_run_loop()).
  browser_runner_ = content::BrowserMainRunner::Create();
  int exit_code = browser_runner_->Initialize(std::move(main_function_params));
  if (exit_code >= 0) {
    LOG(ERROR) << "BrowserMainRunner::Initialize failed: " << exit_code;
    return exit_code;
  }
  return 0;
}

void MainDelegate::ShutdownBrowser() {
  if (browser_runner_) {
    browser_runner_->Shutdown();
    browser_runner_.reset();
  }
}

content::ContentClient* MainDelegate::CreateContentClient() {
  return &content_client_;
}

content::ContentBrowserClient* MainDelegate::CreateContentBrowserClient() {
  browser_client_ = std::make_unique<BrowserClient>();
  return browser_client_.get();
}

content::ContentRendererClient* MainDelegate::CreateContentRendererClient() {
  renderer_client_ = std::make_unique<RendererClient>();
  return renderer_client_.get();
}

}  // namespace notolibra
