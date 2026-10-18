// Copyright 2026 Notolibra. All rights reserved.
//
// Downloads as data (plan.md §2.6): each new download is offered to the app
// (NE_UI_DOWNLOAD) with its suggested file name, and saved where the app
// answers; progress goes to the UI delegate's on_download_updated.

#ifndef NOTOLIBRA_ENGINE_DOWNLOAD_DELEGATE_H_
#define NOTOLIBRA_ENGINE_DOWNLOAD_DELEGATE_H_

#include <cstdint>
#include <set>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/download/public/common/download_item.h"
#include "content/public/browser/download_manager_delegate.h"

namespace content {
class BrowserContext;
}

namespace notolibra {

class DownloadDelegate : public content::DownloadManagerDelegate,
                         public download::DownloadItem::Observer {
 public:
  explicit DownloadDelegate(content::BrowserContext* context);
  ~DownloadDelegate() override;

  // Stops a download the app accepted.
  void Cancel(uint32_t id);

  // content::DownloadManagerDelegate:
  void Shutdown() override;
  void GetNextId(content::DownloadIdCallback callback) override;
  bool DetermineDownloadTarget(
      download::DownloadItem* item,
      download::DownloadTargetCallback* callback) override;
  bool ShouldOpenDownload(
      download::DownloadItem* item,
      content::DownloadOpenDelayedCallback callback) override;

  // download::DownloadItem::Observer:
  void OnDownloadUpdated(download::DownloadItem* item) override;
  void OnDownloadDestroyed(download::DownloadItem* item) override;

 private:
  void Answered(uint32_t id,
                download::DownloadTargetCallback callback,
                const base::FilePath& target);

  raw_ptr<content::BrowserContext> context_;
  uint32_t next_id_ = download::DownloadItem::kInvalidId + 1;
  std::set<raw_ptr<download::DownloadItem>> observed_;
  base::WeakPtrFactory<DownloadDelegate> weak_factory_{this};
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_DOWNLOAD_DELEGATE_H_
