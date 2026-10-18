// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/browser_client.h"

#include "components/embedder_support/user_agent_utils.h"
#include "notolibra/engine/browser_context.h"
#include "notolibra/engine/engine.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "ui/display/screen.h"

namespace notolibra {

BrowserMainParts::BrowserMainParts() = default;
BrowserMainParts::~BrowserMainParts() = default;

void BrowserMainParts::ToolkitInitialized() {
  // Views and RenderWidgetHostViewMac need a display::Screen, which the
  // embedder provides (content_shell's ShellPlatformDelegate does the same).
  screen_ = std::make_unique<display::ScopedNativeScreen>();
}

int BrowserMainParts::PreMainMessageLoopRun() {
  base::FilePath dir = GetEngine()->user_data_dir.Append("Default");
  browser_context_ = std::make_unique<BrowserContext>(dir);
  return 0;
}

void BrowserMainParts::PostMainMessageLoopRun() {
  browser_context_.reset();
}

BrowserClient::BrowserClient() = default;
BrowserClient::~BrowserClient() = default;

std::unique_ptr<content::BrowserMainParts> BrowserClient::CreateBrowserMainParts(
    bool is_integration_test) {
  auto parts = std::make_unique<BrowserMainParts>();
  main_parts_ = parts.get();
  return parts;
}

std::string BrowserClient::GetUserAgent() {
  // Chrome's reduced user agent: sites (Twitch among them) sniff for it.
  return embedder_support::BuildUserAgentFromProduct(
      "Chrome/" NE_CHROMIUM_MAJOR ".0.0.0");
}

blink::UserAgentMetadata BrowserClient::GetUserAgentMetadata() {
  blink::UserAgentMetadata metadata;
  metadata.brand_version_list.emplace_back("Chromium", NE_CHROMIUM_MAJOR);
  metadata.brand_version_list.emplace_back("Notolibra", "0");
  metadata.brand_full_version_list.emplace_back("Chromium", NE_CHROMIUM_VERSION);
  metadata.full_version = NE_CHROMIUM_VERSION;
  metadata.platform = "macOS";
  metadata.architecture = "arm";
  metadata.bitness = "64";
  metadata.mobile = false;
  return metadata;
}

std::string BrowserClient::GetAcceptLangs(content::BrowserContext* context) {
  return "en-US,en";
}

void BrowserClient::ConfigureNetworkContextParams(
    content::BrowserContext* context,
    bool in_memory,
    const base::FilePath& relative_partition_path,
    network::mojom::NetworkContextParams* network_context_params,
    cert_verifier::mojom::CertVerifierCreationParams*
        cert_verifier_creation_params) {
  network_context_params->user_agent = GetUserAgent();
  network_context_params->accept_language = GetAcceptLangs(context);
  network_context_params->enable_zstd = true;
}

std::vector<base::FilePath> BrowserClient::GetNetworkContextsParentDirectory() {
  return {GetEngine()->user_data_dir};
}

BrowserContext* BrowserClient::browser_context() {
  return main_parts_ ? main_parts_->browser_context() : nullptr;
}

}  // namespace notolibra
