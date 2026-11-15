// Copyright 2026 The Notolibra Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notolibra/engine/download_delegate.h"

#include <string>
#include <utility>

#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "components/download/public/common/download_target_info.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/download_item_utils.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/web_contents.h"
#include "net/base/filename_util.h"
#include "notolibra/engine/engine.h"
#include "notolibra/engine/tab.h"

namespace notolibra {

namespace {

std::string ToString(ne_string_view view) {
  return view.data ? std::string(view.data, view.length) : std::string();
}

int32_t StateOf(const download::DownloadItem* item) {
  switch (item->GetState()) {
    case download::DownloadItem::COMPLETE:
      return NE_DOWNLOAD_COMPLETE;
    case download::DownloadItem::CANCELLED:
      return NE_DOWNLOAD_CANCELLED;
    case download::DownloadItem::INTERRUPTED:
      return NE_DOWNLOAD_INTERRUPTED;
    default:
      return NE_DOWNLOAD_IN_PROGRESS;
  }
}

}  // namespace

DownloadDelegate::DownloadDelegate(content::BrowserContext* context)
    : context_(context) {}

DownloadDelegate::~DownloadDelegate() {
  Shutdown();
}

void DownloadDelegate::Shutdown() {
  for (download::DownloadItem* item : observed_) {
    item->RemoveObserver(this);
  }
  observed_.clear();
  weak_factory_.InvalidateWeakPtrs();
}

void DownloadDelegate::GetNextId(content::DownloadIdCallback callback) {
  std::move(callback).Run(next_id_++);
}

bool DownloadDelegate::DetermineDownloadTarget(
    download::DownloadItem* item,
    download::DownloadTargetCallback* callback) {
  if (!item->GetForcedFilePath().empty()) {
    download::DownloadTargetInfo info;
    info.target_path = item->GetForcedFilePath();
    info.intermediate_path = item->GetForcedFilePath();
    std::move(*callback).Run(std::move(info));
    return true;
  }
  // The name the page or the server suggests (no file access).
  const base::FilePath name = net::GenerateFileName(
      item->GetURL(), item->GetContentDisposition(), std::string(),
      item->GetSuggestedFilename(), item->GetMimeType(), "download");
  content::WebContents* contents =
      content::DownloadItemUtils::GetWebContents(item);
  Tab* tab = contents ? Tab::FromWebContents(contents) : nullptr;
  std::string file_name = name.value();
  std::string url = item->GetURL().spec();
  std::string mime = item->GetMimeType();
  std::string origin = item->GetReferrerUrl().is_valid()
                           ? url::Origin::Create(item->GetReferrerUrl()).Serialize()
                           : std::string();
  ne_ui_request request = {sizeof(ne_ui_request)};
  request.kind = NE_UI_DOWNLOAD;
  request.tab = tab ? tab->handle() : nullptr;
  request.origin = {origin.data(), origin.size()};
  request.message = {file_name.data(), file_name.size()};
  request.url = {url.data(), url.size()};
  request.mime_type = {mime.data(), mime.size()};
  request.total_bytes = item->GetTotalBytes() > 0 ? item->GetTotalBytes() : -1;
  request.download_id = item->GetId();
  const uint32_t id = item->GetId();
  GetEngine()->SendUiRequest(
      request, tab,
      base::BindOnce(
          [](base::WeakPtr<DownloadDelegate> self, uint32_t id,
             download::DownloadTargetCallback callback,
             const ne_ui_response* response) {
            base::FilePath target;
            if (response && response->accept && response->path_count > 0) {
              target = base::FilePath(ToString(response->paths[0]));
            }
            if (self) {
              self->Answered(id, std::move(callback), target);
            } else {
              std::move(callback).Run(download::DownloadTargetInfo());
            }
          },
          weak_factory_.GetWeakPtr(), id, std::move(*callback)));
  return true;
}

void DownloadDelegate::Answered(uint32_t id,
                                download::DownloadTargetCallback callback,
                                const base::FilePath& target) {
  download::DownloadTargetInfo info;
  // An empty target cancels the download (declined).
  if (!target.empty()) {
    info.target_path = target;
    info.intermediate_path = target.AddExtensionASCII("crdownload");
    info.display_name = target.BaseName();
    if (download::DownloadItem* item =
            context_->GetDownloadManager()->GetDownload(id)) {
      item->AddObserver(this);
      observed_.insert(item);
    }
  }
  std::move(callback).Run(std::move(info));
}

bool DownloadDelegate::ShouldOpenDownload(
    download::DownloadItem* item,
    content::DownloadOpenDelayedCallback callback) {
  // Nothing opens downloads by itself; the app decides.
  return true;
}

void DownloadDelegate::Cancel(uint32_t id) {
  if (download::DownloadItem* item =
          context_->GetDownloadManager()->GetDownload(id)) {
    item->Cancel(/*user_cancel=*/true);
  }
}

void DownloadDelegate::OnDownloadUpdated(download::DownloadItem* item) {
  Engine* engine = GetEngine();
  if (engine && engine->has_ui_delegate &&
      engine->ui_delegate.on_download_updated) {
    std::string path = item->GetTargetFilePath().value();
    engine->ui_delegate.on_download_updated(
        engine->ui_delegate.user_data, item->GetId(), StateOf(item),
        item->GetReceivedBytes(), item->GetTotalBytes(),
        {path.data(), path.size()});
  }
  if (item->IsDone()) {
    item->RemoveObserver(this);
    observed_.erase(item);
  }
}

void DownloadDelegate::OnDownloadDestroyed(download::DownloadItem* item) {
  item->RemoveObserver(this);
  observed_.erase(item);
}

}  // namespace notolibra
