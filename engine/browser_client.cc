// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/browser_client.h"

#include "base/path_service.h"
#include "components/embedder_support/user_agent_utils.h"
#include "content/public/common/content_paths.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "components/os_crypt/async/browser/keychain_key_provider.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/common/keychain_password_mac.h"
#include "content/public/browser/certificate_request_result_type.h"
#include "content/public/browser/web_contents.h"
#include "net/base/auth.h"
#include "net/base/net_errors.h"
#include "net/ssl/ssl_info.h"
#include "notolibra/engine/browser_context.h"
#include "notolibra/engine/request_filter.h"
#include "notolibra/engine/tab.h"
#include "services/network/public/cpp/cookie_encryption_provider_impl.h"
#include "url/origin.h"
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
  // Our own Safe Storage keychain item, never Chromium's; under
  // --use-mock-keychain (verification runs) nothing reaches the keychain.
  Engine* engine = GetEngine();
  if (!engine->keychain_service.empty()) {
    KeychainPassword::GetServiceName() = engine->keychain_service;
    KeychainPassword::GetAccountName() = "Notolibra Engine";
  }
  std::vector<std::pair<os_crypt_async::OSCryptAsync::Precedence,
                        std::unique_ptr<os_crypt_async::KeyProvider>>>
      providers;
  providers.emplace_back(10u,
                         std::make_unique<os_crypt_async::KeychainKeyProvider>());
  os_crypt_ = std::make_unique<os_crypt_async::OSCryptAsync>(std::move(providers));
  cookie_encryption_ =
      std::make_unique<CookieEncryptionProviderImpl>(os_crypt_.get());
  base::FilePath dir = GetEngine()->user_data_dir.Append("Default");
  browser_context_ = std::make_unique<BrowserContext>(dir);
  return 0;
}

void BrowserMainParts::PostMainMessageLoopRun() {
  browser_context_.reset();
  cookie_encryption_.reset();
  os_crypt_.reset();
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
  if (in_memory) {
    return;
  }
  // The profile keeps its cookies, cache and network state on disk, as
  // Chrome lays them out (ProfileNetworkContextService).
  const base::FilePath base = context->GetPath().Append(relative_partition_path);
  auto paths = network::mojom::NetworkContextFilePaths::New();
  paths->data_directory = base.Append(FILE_PATH_LITERAL("Network"));
  paths->unsandboxed_data_path = base;
  paths->cookie_database_name = base::FilePath(FILE_PATH_LITERAL("Cookies"));
  paths->trust_token_database_name =
      base::FilePath(FILE_PATH_LITERAL("Trust Tokens"));
  paths->http_server_properties_file_name =
      base::FilePath(FILE_PATH_LITERAL("Network Persistent State"));
  paths->transport_security_persister_file_name =
      base::FilePath(FILE_PATH_LITERAL("TransportSecurity"));
  paths->reporting_and_nel_store_database_name =
      base::FilePath(FILE_PATH_LITERAL("Reporting and NEL"));
  paths->sct_auditing_pending_reports_file_name =
      base::FilePath(FILE_PATH_LITERAL("SCT Auditing Pending Reports"));
  paths->http_cache_directory = base.Append(FILE_PATH_LITERAL("Cache"));
  network_context_params->file_paths = std::move(paths);
  // Session cookies survive a restart, as the CEF pane's did.
  network_context_params->persist_session_cookies = true;
  network_context_params->enable_encrypted_cookies = true;
  if (main_parts_ && main_parts_->cookie_encryption()) {
    network_context_params->cookie_encryption_provider =
        main_parts_->cookie_encryption()->BindNewRemote();
  }
}

std::vector<base::FilePath> BrowserClient::GetNetworkContextsParentDirectory() {
  return {GetEngine()->user_data_dir};
}

base::FilePath BrowserClient::GetChildProcessPath(int child_flags) {
  // One helper app for every child process type. (Chromium otherwise picks
  // "<helper> (Renderer).app" and the like when the host app is a bundle.)
  base::FilePath path;
  base::PathService::Get(content::CHILD_PROCESS_EXE, &path);
  return path;
}

namespace {

// HTTP sign-in as data: the app asks for a user name and password
// (NE_UI_HTTP_AUTH). Destroying the delegate cancels the request.
class LoginDelegate : public content::LoginDelegate {
 public:
  LoginDelegate(const net::AuthChallengeInfo& auth_info,
                content::WebContents* web_contents,
                const GURL& url,
                LoginAuthRequiredCallback callback)
      : callback_(std::move(callback)) {
    // Not from inside CreateLoginDelegate: the app may answer at once.
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&LoginDelegate::Ask, weak_factory_.GetWeakPtr(),
                       auth_info, web_contents ? web_contents->GetWeakPtr()
                                               : nullptr,
                       url));
  }

  ~LoginDelegate() override {
    if (request_id_) {
      GetEngine()->CancelRequest(request_id_);
    }
  }

 private:
  void Ask(net::AuthChallengeInfo auth_info,
           base::WeakPtr<content::WebContents> web_contents,
           GURL url) {
    Tab* tab = web_contents ? Tab::FromWebContents(web_contents.get()) : nullptr;
    std::string origin = auth_info.challenger.Serialize();
    std::string realm = auth_info.realm;
    std::string spec = url.spec();
    std::string scheme = auth_info.scheme;
    ne_ui_request request = {sizeof(ne_ui_request)};
    request.kind = NE_UI_HTTP_AUTH;
    request.tab = tab ? tab->handle() : nullptr;
    request.origin = {origin.data(), origin.size()};
    request.message = {realm.data(), realm.size()};
    request.detail = {scheme.data(), scheme.size()};
    request.url = {spec.data(), spec.size()};
    request.can_proceed = !auth_info.is_proxy;
    request_id_ = GetEngine()->SendUiRequest(
        request, tab,
        base::BindOnce(&LoginDelegate::Answered, weak_factory_.GetWeakPtr()));
  }

  void Answered(const ne_ui_response* response) {
    request_id_ = 0;
    if (!callback_) {
      return;
    }
    if (response && response->accept) {
      auto text = [](ne_string_view view) {
        return view.data ? std::string(view.data, view.length) : std::string();
      };
      std::move(callback_).Run(net::AuthCredentials(
          base::UTF8ToUTF16(text(response->text)),
          base::UTF8ToUTF16(text(response->secret))));
    } else {
      std::move(callback_).Run(std::nullopt);
    }
  }

  LoginAuthRequiredCallback callback_;
  ne_request_id request_id_ = 0;
  base::WeakPtrFactory<LoginDelegate> weak_factory_{this};
};

}  // namespace

std::unique_ptr<content::LoginDelegate> BrowserClient::CreateLoginDelegate(
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
    content::LoginDelegate::LoginAuthRequiredCallback auth_required_callback) {
  return std::make_unique<LoginDelegate>(auth_info, web_contents, url,
                                         std::move(auth_required_callback));
}

void BrowserClient::AllowCertificateError(
    content::WebContents* web_contents,
    int cert_error,
    const net::SSLInfo& ssl_info,
    const GURL& request_url,
    bool is_primary_main_frame_request,
    bool strict_enforcement,
    base::OnceCallback<void(content::CertificateRequestResultType)> callback) {
  // Subresources with bad certificates are refused without asking, as Chrome
  // does; the page itself asks the app (NE_UI_CERTIFICATE_ERROR).
  Tab* tab = web_contents ? Tab::FromWebContents(web_contents) : nullptr;
  if (!is_primary_main_frame_request || !tab) {
    std::move(callback).Run(content::CERTIFICATE_REQUEST_RESULT_TYPE_DENY);
    return;
  }
  std::string error = net::ErrorToString(cert_error);
  std::string host = request_url.GetHost();
  std::string spec = request_url.spec();
  ne_ui_request request = {sizeof(ne_ui_request)};
  request.kind = NE_UI_CERTIFICATE_ERROR;
  request.tab = tab->handle();
  request.origin = {host.data(), host.size()};
  request.message = {host.data(), host.size()};
  request.detail = {error.data(), error.size()};
  request.url = {spec.data(), spec.size()};
  // HSTS sites can't be proceeded past (strict enforcement).
  request.can_proceed = !strict_enforcement;
  GetEngine()->SendUiRequest(
      request, tab,
      base::BindOnce(
          [](bool strict,
             base::OnceCallback<void(content::CertificateRequestResultType)>
                 callback,
             const ne_ui_response* response) {
            std::move(callback).Run(
                response && response->accept && !strict
                    ? content::CERTIFICATE_REQUEST_RESULT_TYPE_CONTINUE
                    : content::CERTIFICATE_REQUEST_RESULT_TYPE_DENY);
          },
          strict_enforcement, std::move(callback)));
}

void BrowserClient::WillCreateURLLoaderFactory(
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
    bool is_for_network_service) {
  // Every page's requests pass the app's content filter (request_filter.h).
  content::WebContents* contents =
      frame ? content::WebContents::FromRenderFrameHost(frame) : nullptr;
  Tab* tab = contents ? Tab::FromWebContents(contents) : nullptr;
  InstallRequestFilter(factory_builder, tab ? tab->id() : 0);
}

BrowserContext* BrowserClient::browser_context() {
  return main_parts_ ? main_parts_->browser_context() : nullptr;
}

}  // namespace notolibra
