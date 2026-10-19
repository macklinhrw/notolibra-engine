// Copyright 2026 Notolibra. All rights reserved.

#ifndef NOTOLIBRA_ENGINE_MAIN_DELEGATE_H_
#define NOTOLIBRA_ENGINE_MAIN_DELEGATE_H_

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "base/memory/scoped_refptr.h"
#include "content/public/app/content_main_delegate.h"
#include "content/public/common/content_client.h"

namespace content {
class BrowserMainRunner;
class ContentRendererClient;
}  // namespace content

namespace notolibra {

class BrowserClient;

// Resources come from the framework's notolibra_engine.pak (content's,
// blink's, net's and ui's resources).
class ContentClient : public content::ContentClient {
 public:
  std::u16string GetLocalizedString(int message_id) override;
  std::string_view GetDataResource(
      int resource_id,
      ui::ResourceScaleFactor scale_factor) override;
  scoped_refptr<base::RefCountedMemory> GetDataResourceBytes(
      int resource_id) override;
  std::string GetDataResourceString(int resource_id) override;
  gfx::Image& GetNativeImageNamed(int resource_id) override;
  void AddPlugins(std::vector<content::WebPluginInfo>* plugins) override;
};

// One delegate for every process type. In the browser process, RunProcess()
// starts BrowserMainRunner without running a loop: the host app's
// -[NSApplication run] is already spinning the main thread
// (ContentMainParams::embedder_owns_run_loop).
class MainDelegate : public content::ContentMainDelegate {
 public:
  MainDelegate();
  ~MainDelegate() override;

  // content::ContentMainDelegate:
  std::optional<int> BasicStartupComplete() override;
  void PreSandboxStartup() override;
  std::variant<int, content::MainFunctionParams> RunProcess(
      const std::string& process_type,
      content::MainFunctionParams main_function_params) override;
  content::ContentClient* CreateContentClient() override;
  content::ContentBrowserClient* CreateContentBrowserClient() override;
  content::ContentRendererClient* CreateContentRendererClient() override;

  BrowserClient* browser_client() { return browser_client_.get(); }
  content::BrowserMainRunner* browser_runner() { return browser_runner_.get(); }
  void ShutdownBrowser();

 private:
  ContentClient content_client_;
  std::unique_ptr<BrowserClient> browser_client_;
  std::unique_ptr<content::ContentRendererClient> renderer_client_;
  std::unique_ptr<content::BrowserMainRunner> browser_runner_;
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_MAIN_DELEGATE_H_
