// Copyright 2026 Notolibra. All rights reserved.

#ifndef NOTOLIBRA_ENGINE_BROWSER_CLIENT_H_
#define NOTOLIBRA_ENGINE_BROWSER_CLIENT_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "content/public/browser/browser_main_parts.h"
#include "content/public/browser/content_browser_client.h"

namespace display {
class ScopedNativeScreen;
}

namespace notolibra {

class BrowserContext;

class BrowserMainParts : public content::BrowserMainParts {
 public:
  BrowserMainParts();
  ~BrowserMainParts() override;

  // content::BrowserMainParts:
  void ToolkitInitialized() override;
  int PreMainMessageLoopRun() override;
  void PostMainMessageLoopRun() override;

  BrowserContext* browser_context() { return browser_context_.get(); }

 private:
  std::unique_ptr<display::ScopedNativeScreen> screen_;
  std::unique_ptr<BrowserContext> browser_context_;
};

class BrowserClient : public content::ContentBrowserClient {
 public:
  BrowserClient();
  ~BrowserClient() override;

  // content::ContentBrowserClient:
  std::unique_ptr<content::BrowserMainParts> CreateBrowserMainParts(
      bool is_integration_test) override;
  std::string GetUserAgent() override;
  blink::UserAgentMetadata GetUserAgentMetadata() override;
  std::string GetAcceptLangs(content::BrowserContext* context) override;
  void ConfigureNetworkContextParams(
      content::BrowserContext* context,
      bool in_memory,
      const base::FilePath& relative_partition_path,
      network::mojom::NetworkContextParams* network_context_params,
      cert_verifier::mojom::CertVerifierCreationParams*
          cert_verifier_creation_params) override;
  std::vector<base::FilePath> GetNetworkContextsParentDirectory() override;

  // The one profile of Phase 0, created in PreMainMessageLoopRun().
  BrowserContext* browser_context();

 private:
  raw_ptr<BrowserMainParts> main_parts_ = nullptr;
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_BROWSER_CLIENT_H_
