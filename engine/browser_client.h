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
#include "content/public/browser/login_delegate.h"
#include "services/network/public/cpp/network_quality_tracker.h"

namespace display {
class ScopedNativeScreen;
}

namespace os_crypt_async {
class OSCryptAsync;
}

class CookieEncryptionProviderImpl;

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
  CookieEncryptionProviderImpl* cookie_encryption() {
    return cookie_encryption_.get();
  }

 private:
  std::unique_ptr<display::ScopedNativeScreen> screen_;
  // Cookies are encrypted with a key in our own keychain item (plan.md §4.6).
  std::unique_ptr<os_crypt_async::OSCryptAsync> os_crypt_;
  std::unique_ptr<CookieEncryptionProviderImpl> cookie_encryption_;
  std::unique_ptr<network::NetworkQualityTracker> network_quality_tracker_;
  std::unique_ptr<
      network::NetworkQualityTracker::RTTAndThroughputEstimatesObserver>
      network_quality_observer_;
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
  void UpdateRendererPreferencesForWorker(
      content::BrowserContext* browser_context,
      blink::RendererPreferences* out_prefs) override;
  void ConfigureNetworkContextParams(
      content::BrowserContext* context,
      bool in_memory,
      const base::FilePath& relative_partition_path,
      network::mojom::NetworkContextParams* network_context_params,
      cert_verifier::mojom::CertVerifierCreationParams*
          cert_verifier_creation_params) override;
  std::vector<base::FilePath> GetNetworkContextsParentDirectory() override;
  base::FilePath GetChildProcessPath(int child_flags) override;
  void WillCreateURLLoaderFactory(
      content::BrowserContext* browser_context,
      content::RenderFrameHost* frame,
      int render_process_id,
      URLLoaderFactoryType type,
      const url::Origin& request_initiator,
      const net::IsolationInfo& isolation_info,
      std::optional<int64_t> navigation_id,
      ukm::SourceIdObj ukm_source_id,
      network::URLLoaderFactoryBuilder& factory_builder,
      mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>*
          header_client,
      bool* bypass_redirect_checks,
      bool* disable_secure_dns,
      network::mojom::URLLoaderFactoryOverridePtr* factory_override,
      scoped_refptr<base::SequencedTaskRunner> navigation_response_task_runner,
      bool is_for_network_service) override;
  std::unique_ptr<content::LoginDelegate> CreateLoginDelegate(
      const net::AuthChallengeInfo& auth_info,
      content::WebContents* web_contents,
      content::BrowserContext* browser_context,
      const content::GlobalRequestID& request_id,
      bool is_request_for_primary_main_frame_navigation,
      bool is_request_for_navigation,
      const GURL& url,
      scoped_refptr<net::HttpResponseHeaders> response_headers,
      bool first_auth_attempt,
      content::GuestPageHolder* guest_page_holder,
      content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback)
      override;
  void AllowCertificateError(
      content::WebContents* web_contents,
      int cert_error,
      const net::SSLInfo& ssl_info,
      const GURL& request_url,
      bool is_primary_main_frame_request,
      bool strict_enforcement,
      base::OnceCallback<void(content::CertificateRequestResultType)> callback)
      override;
  // PDFs a frame navigates to open in the app (pdf.h).
  std::vector<std::unique_ptr<blink::URLLoaderThrottle>>
  CreateURLLoaderThrottles(
      const network::ResourceRequest& request,
      content::BrowserContext* browser_context,
      const base::RepeatingCallback<content::WebContents*()>& wc_getter,
      content::NavigationUIData* navigation_ui_data,
      content::FrameTreeNodeId frame_tree_node_id,
      std::optional<int64_t> navigation_id) override;
  // Refuses drags that would start a system drag nobody is making (a hidden
  // run's synthetic one), ending the page's drag (system_drag.h).
  bool IsDragAllowedByPolicy(const content::ClipboardEndpoint& source,
                             const content::DropData& drop_data) override;

  // The one profile of Phase 0, created in PreMainMessageLoopRun().
  BrowserContext* browser_context();

 private:
  raw_ptr<BrowserMainParts> main_parts_ = nullptr;
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_BROWSER_CLIENT_H_
