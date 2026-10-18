// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/browser_context.h"

#include <vector>

#include "base/files/file_util.h"
#include "base/strings/string_util.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/permission_request_description.h"
#include "content/public/browser/permission_result.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/zoom_level_delegate.h"
#include "net/cert/x509_certificate.h"
#include "notolibra/engine/download_delegate.h"
#include "notolibra/engine/engine.h"
#include "notolibra/engine/tab.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "url/origin.h"

namespace notolibra {

PermissionManager::PermissionManager() = default;
PermissionManager::~PermissionManager() = default;

content::PermissionStatus PermissionManager::Lookup(
    blink::PermissionType type,
    const url::Origin& origin) const {
  auto it = decided_.find({origin.Serialize(), static_cast<int>(type)});
  return it == decided_.end() ? content::PermissionStatus::ASK : it->second;
}

void PermissionManager::RequestPermissionsFromCurrentDocument(
    content::RenderFrameHost* render_frame_host,
    const content::PermissionRequestDescription& request_description,
    base::OnceCallback<void(const std::vector<content::PermissionResult>&)>
        callback) {
  const url::Origin origin = render_frame_host->GetLastCommittedOrigin();
  std::vector<blink::PermissionType> types;
  std::vector<std::string> names;
  bool all_decided = true;
  for (const auto& descriptor : request_description.permissions) {
    blink::PermissionType type =
        blink::PermissionDescriptorToPermissionType(descriptor);
    types.push_back(type);
    names.push_back(blink::GetPermissionString(type));
    if (Lookup(type, origin) == content::PermissionStatus::ASK) {
      all_decided = false;
    }
  }
  auto results_for = [this, types, origin](bool accept, bool remember) {
    std::vector<content::PermissionResult> results;
    for (blink::PermissionType type : types) {
      content::PermissionStatus status = Lookup(type, origin);
      if (status == content::PermissionStatus::ASK) {
        status = accept ? content::PermissionStatus::GRANTED
                        : content::PermissionStatus::DENIED;
        if (remember) {
          decided_[{origin.Serialize(), static_cast<int>(type)}] = status;
        }
      }
      results.emplace_back(status);
    }
    return results;
  };
  if (all_decided) {
    std::move(callback).Run(results_for(false, false));
    return;
  }

  content::WebContents* contents =
      content::WebContents::FromRenderFrameHost(render_frame_host);
  Tab* tab = Tab::FromWebContents(contents);
  std::string origin_text = origin.Serialize();
  std::string detail = base::JoinString(names, ",");
  ne_ui_request request = {sizeof(ne_ui_request)};
  request.kind = NE_UI_PERMISSION;
  request.tab = tab ? tab->handle() : nullptr;
  request.origin = {origin_text.data(), origin_text.size()};
  request.detail = {detail.data(), detail.size()};
  GetEngine()->SendUiRequest(
      request, tab,
      base::BindOnce(
          [](base::WeakPtr<PermissionManager> self,
             decltype(results_for) results_for,
             base::OnceCallback<void(
                 const std::vector<content::PermissionResult>&)> callback,
             const ne_ui_response* response) {
            if (!self) {
              return;
            }
            bool accept = response && response->accept;
            bool remember = response && response->remember;
            std::move(callback).Run(results_for(accept, remember));
          },
          weak_factory_.GetWeakPtr(), std::move(results_for),
          std::move(callback)));
}

content::PermissionStatus PermissionManager::GetPermissionStatus(
    const blink::mojom::PermissionDescriptorPtr& permission,
    const GURL& requesting_origin,
    const GURL& embedding_origin) {
  return Lookup(blink::PermissionDescriptorToPermissionType(permission),
                url::Origin::Create(requesting_origin));
}

content::PermissionResult
PermissionManager::GetPermissionResultForOriginWithoutContext(
    const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
    const url::Origin& requesting_origin,
    const url::Origin& embedding_origin) {
  return content::PermissionResult(Lookup(
      blink::PermissionDescriptorToPermissionType(permission_descriptor),
      requesting_origin));
}

content::PermissionResult
PermissionManager::GetPermissionResultForCurrentDocument(
    const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
    content::RenderFrameHost* render_frame_host,
    bool should_include_device_status) {
  return content::PermissionResult(Lookup(
      blink::PermissionDescriptorToPermissionType(permission_descriptor),
      render_frame_host->GetLastCommittedOrigin()));
}

content::PermissionResult PermissionManager::GetPermissionResultForWorker(
    const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
    content::RenderProcessHost* render_process_host,
    const GURL& worker_origin) {
  return content::PermissionResult(Lookup(
      blink::PermissionDescriptorToPermissionType(permission_descriptor),
      url::Origin::Create(worker_origin)));
}

content::PermissionResult
PermissionManager::GetPermissionResultForEmbeddedRequester(
    const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
    content::RenderFrameHost* render_frame_host,
    const url::Origin& requesting_origin) {
  return content::PermissionResult(Lookup(
      blink::PermissionDescriptorToPermissionType(permission_descriptor),
      requesting_origin));
}

void PermissionManager::ResetPermission(blink::PermissionType permission,
                                        const GURL& requesting_origin,
                                        const GURL& embedding_origin) {
  decided_.erase({url::Origin::Create(requesting_origin).Serialize(),
                  static_cast<int>(permission)});
}

SslHostState::SslHostState() = default;
SslHostState::~SslHostState() = default;

namespace {
std::string Fingerprint(const net::X509Certificate& cert) {
  net::SHA256HashValue hash = cert.CalculateChainFingerprint256();
  return std::string(reinterpret_cast<const char*>(hash.data()), hash.size());
}
}  // namespace

void SslHostState::AllowCert(const std::string& host,
                             const net::X509Certificate& cert,
                             net::Error error,
                             content::StoragePartition* storage_partition) {
  allowed_.insert({host, Fingerprint(cert), error});
}

void SslHostState::Clear(
    base::RepeatingCallback<bool(const std::string&)> host_filter) {
  auto matches = [&](const std::string& host) {
    return host_filter.is_null() || host_filter.Run(host);
  };
  std::erase_if(allowed_, [&](const auto& entry) {
    return matches(std::get<0>(entry));
  });
  std::erase_if(http_allowed_, matches);
}

content::SSLHostStateDelegate::CertJudgment SslHostState::QueryPolicy(
    const std::string& host,
    const net::X509Certificate& cert,
    net::Error error,
    content::StoragePartition* storage_partition) {
  return allowed_.contains({host, Fingerprint(cert), error}) ? ALLOWED
                                                             : DENIED;
}

void SslHostState::HostRanInsecureContent(const std::string& host,
                                          InsecureContentType content_type) {
  insecure_content_.insert({host, static_cast<int>(content_type)});
}

bool SslHostState::DidHostRunInsecureContent(const std::string& host,
                                             InsecureContentType content_type) {
  return insecure_content_.contains({host, static_cast<int>(content_type)});
}

void SslHostState::AllowHttpForHost(
    const std::string& host,
    content::StoragePartition* storage_partition) {
  http_allowed_.insert(host);
}

bool SslHostState::IsHttpAllowedForHost(
    const std::string& host,
    content::StoragePartition* storage_partition) {
  return http_allowed_.contains(host);
}

void SslHostState::RevokeUserAllowExceptions(const std::string& host) {
  std::erase_if(allowed_,
                [&](const auto& entry) { return std::get<0>(entry) == host; });
  http_allowed_.erase(host);
}

void SslHostState::SetHttpsEnforcementForHost(
    const std::string& host,
    bool enforce,
    content::StoragePartition* storage_partition) {
  if (enforce) {
    https_enforced_.insert(host);
  } else {
    https_enforced_.erase(host);
  }
}

bool SslHostState::IsHttpsEnforcedForUrl(
    const GURL& url,
    content::StoragePartition* storage_partition) {
  return https_enforced_.contains(url.GetHost());
}

bool SslHostState::HasAllowException(
    const std::string& host,
    content::StoragePartition* storage_partition) {
  return http_allowed_.contains(host) ||
         std::any_of(allowed_.begin(), allowed_.end(), [&](const auto& entry) {
           return std::get<0>(entry) == host;
         });
}

bool SslHostState::HasAllowExceptionForAnyHost(
    content::StoragePartition* storage_partition) {
  return !allowed_.empty() || !http_allowed_.empty();
}

// ne_app_init() creates the directory, before IO is disallowed here.
BrowserContext::BrowserContext(const base::FilePath& path) : path_(path) {}

BrowserContext::~BrowserContext() {
  // The download manager calls its delegate as it shuts down, which the
  // base class's destructor would do after download_delegate_ is gone.
  if (download_delegate_) {
    GetDownloadManager()->Shutdown();
    download_delegate_->Shutdown();
  }
  NotifyWillBeDestroyed();
  ShutdownStoragePartitions();
}

std::unique_ptr<content::ZoomLevelDelegate>
BrowserContext::CreateZoomLevelDelegate(const base::FilePath& partition_path) {
  return nullptr;
}

base::FilePath BrowserContext::GetPath() const {
  return path_;
}

bool BrowserContext::IsOffTheRecord() {
  return false;
}

content::DownloadManagerDelegate* BrowserContext::GetDownloadManagerDelegate() {
  if (!download_delegate_) {
    download_delegate_ = std::make_unique<DownloadDelegate>(this);
  }
  return download_delegate_.get();
}

void BrowserContext::CancelDownload(uint32_t id) {
  if (download_delegate_) {
    download_delegate_->Cancel(id);
  }
}

content::BrowserPluginGuestManager* BrowserContext::GetGuestManager() {
  return nullptr;
}

storage::SpecialStoragePolicy* BrowserContext::GetSpecialStoragePolicy() {
  return nullptr;
}

content::PlatformNotificationService*
BrowserContext::GetPlatformNotificationService() {
  return nullptr;
}

content::PushMessagingService* BrowserContext::GetPushMessagingService() {
  return nullptr;
}

content::StorageNotificationService*
BrowserContext::GetStorageNotificationService() {
  return nullptr;
}

content::SSLHostStateDelegate* BrowserContext::GetSSLHostStateDelegate() {
  if (!ssl_host_state_) {
    ssl_host_state_ = std::make_unique<SslHostState>();
  }
  return ssl_host_state_.get();
}

content::PermissionControllerDelegate*
BrowserContext::GetPermissionControllerDelegate() {
  if (!permission_manager_) {
    permission_manager_ = std::make_unique<PermissionManager>();
  }
  return permission_manager_.get();
}

content::ReduceAcceptLanguageControllerDelegate*
BrowserContext::GetReduceAcceptLanguageControllerDelegate() {
  return nullptr;
}

content::ClientHintsControllerDelegate*
BrowserContext::GetClientHintsControllerDelegate() {
  return nullptr;
}

content::BackgroundFetchDelegate* BrowserContext::GetBackgroundFetchDelegate() {
  return nullptr;
}

content::BackgroundSyncController*
BrowserContext::GetBackgroundSyncController() {
  return nullptr;
}

content::BrowsingDataRemoverDelegate*
BrowserContext::GetBrowsingDataRemoverDelegate() {
  return nullptr;
}

}  // namespace notolibra
