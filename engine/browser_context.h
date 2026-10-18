// Copyright 2026 Notolibra. All rights reserved.

#ifndef NOTOLIBRA_ENGINE_BROWSER_CONTEXT_H_
#define NOTOLIBRA_ENGINE_BROWSER_CONTEXT_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include "base/files/file_path.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/permission_controller_delegate.h"
#include "content/public/browser/ssl_host_state_delegate.h"
#include "net/base/net_errors.h"

namespace notolibra {

// Permissions are the app's to decide: each request goes to it as a UI
// request (NE_UI_PERMISSION). Answers the app asks to remember are kept per
// (origin, permission) for this session.
class PermissionManager : public content::PermissionControllerDelegate {
 public:
  PermissionManager();
  ~PermissionManager() override;

  // content::PermissionControllerDelegate:
  void RequestPermissionsFromCurrentDocument(
      content::RenderFrameHost* render_frame_host,
      const content::PermissionRequestDescription& request_description,
      base::OnceCallback<void(const std::vector<content::PermissionResult>&)>
          callback) override;
  content::PermissionStatus GetPermissionStatus(
      const blink::mojom::PermissionDescriptorPtr& permission,
      const GURL& requesting_origin,
      const GURL& embedding_origin) override;
  content::PermissionResult GetPermissionResultForOriginWithoutContext(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      const url::Origin& requesting_origin,
      const url::Origin& embedding_origin) override;
  content::PermissionResult GetPermissionResultForCurrentDocument(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      content::RenderFrameHost* render_frame_host,
      bool should_include_device_status) override;
  content::PermissionResult GetPermissionResultForWorker(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      content::RenderProcessHost* render_process_host,
      const GURL& worker_origin) override;
  content::PermissionResult GetPermissionResultForEmbeddedRequester(
      const blink::mojom::PermissionDescriptorPtr& permission_descriptor,
      content::RenderFrameHost* render_frame_host,
      const url::Origin& requesting_origin) override;
  void ResetPermission(blink::PermissionType permission,
                       const GURL& requesting_origin,
                       const GURL& embedding_origin) override;

 private:
  content::PermissionStatus Lookup(blink::PermissionType type,
                                   const url::Origin& origin) const;

  std::map<std::pair<std::string, int>, content::PermissionStatus> decided_;
  base::WeakPtrFactory<PermissionManager> weak_factory_{this};
};

// Certificate errors the user chose to proceed past, and hosts allowed over
// HTTP, for this session (as Chrome remembers them until the browser quits).
class SslHostState : public content::SSLHostStateDelegate {
 public:
  SslHostState();
  ~SslHostState() override;

  // content::SSLHostStateDelegate:
  void AllowCert(const std::string& host,
                 const net::X509Certificate& cert,
                 net::Error error,
                 content::StoragePartition* storage_partition) override;
  void Clear(
      base::RepeatingCallback<bool(const std::string&)> host_filter) override;
  CertJudgment QueryPolicy(const std::string& host,
                           const net::X509Certificate& cert,
                           net::Error error,
                           content::StoragePartition* storage_partition) override;
  void HostRanInsecureContent(const std::string& host,
                              InsecureContentType content_type) override;
  bool DidHostRunInsecureContent(const std::string& host,
                                 InsecureContentType content_type) override;
  void AllowHttpForHost(const std::string& host,
                        content::StoragePartition* storage_partition) override;
  bool IsHttpAllowedForHost(const std::string& host,
                            content::StoragePartition* storage_partition) override;
  void RevokeUserAllowExceptions(const std::string& host) override;
  void SetHttpsEnforcementForHost(
      const std::string& host,
      bool enforce,
      content::StoragePartition* storage_partition) override;
  bool IsHttpsEnforcedForUrl(
      const GURL& url,
      content::StoragePartition* storage_partition) override;
  bool HasAllowException(const std::string& host,
                         content::StoragePartition* storage_partition) override;
  bool HasAllowExceptionForAnyHost(
      content::StoragePartition* storage_partition) override;

 private:
  // (host, certificate fingerprint, error) the user allowed.
  std::set<std::tuple<std::string, std::string, int>> allowed_;
  std::set<std::string> http_allowed_;
  std::set<std::string> https_enforced_;
  std::set<std::pair<std::string, int>> insecure_content_;
};

class DownloadDelegate;

class BrowserContext : public content::BrowserContext {
 public:
  explicit BrowserContext(const base::FilePath& path);
  ~BrowserContext() override;

  // content::BrowserContext:
  std::unique_ptr<content::ZoomLevelDelegate> CreateZoomLevelDelegate(
      const base::FilePath& partition_path) override;
  base::FilePath GetPath() const override;
  bool IsOffTheRecord() override;
  content::DownloadManagerDelegate* GetDownloadManagerDelegate() override;
  content::BrowserPluginGuestManager* GetGuestManager() override;
  storage::SpecialStoragePolicy* GetSpecialStoragePolicy() override;
  content::PlatformNotificationService* GetPlatformNotificationService()
      override;
  content::PushMessagingService* GetPushMessagingService() override;
  content::StorageNotificationService* GetStorageNotificationService() override;
  content::SSLHostStateDelegate* GetSSLHostStateDelegate() override;
  content::PermissionControllerDelegate* GetPermissionControllerDelegate()
      override;
  content::ReduceAcceptLanguageControllerDelegate*
  GetReduceAcceptLanguageControllerDelegate() override;
  content::ClientHintsControllerDelegate* GetClientHintsControllerDelegate()
      override;
  content::BackgroundFetchDelegate* GetBackgroundFetchDelegate() override;
  content::BackgroundSyncController* GetBackgroundSyncController() override;
  content::BrowsingDataRemoverDelegate* GetBrowsingDataRemoverDelegate()
      override;

  // Stops the profile's downloads (ne_download_cancel).
  void CancelDownload(uint32_t id);

 private:
  base::FilePath path_;
  std::unique_ptr<PermissionManager> permission_manager_;
  std::unique_ptr<SslHostState> ssl_host_state_;
  std::unique_ptr<DownloadDelegate> download_delegate_;
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_BROWSER_CONTEXT_H_
