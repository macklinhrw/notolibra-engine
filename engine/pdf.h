// Copyright 2026 Notolibra. All rights reserved.
//
// PDFs without a viewer of the engine's own (ne_engine.h, ne_pdf_event).
// A navigation whose response is a PDF commits a document the engine makes,
// at the PDF's address, instead of downloading; in the main frame the PDF's
// bytes are saved into the app's PDF folder meanwhile, for the app to show
// it. The seam is the one Chrome's PDF viewer uses
// (PluginResponseInterceptorURLLoaderThrottle): a navigation throttle that
// replaces the response's body and keeps the original stream.

#ifndef NOTOLIBRA_ENGINE_PDF_H_
#define NOTOLIBRA_ENGINE_PDF_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "base/time/time.h"
#include "content/public/browser/frame_tree_node_id.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_drainer.h"
#include "ne_engine.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "third_party/blink/public/common/loader/url_loader_throttle.h"
#include "url/gurl.h"

namespace network {
struct ResourceRequest;
}

namespace notolibra {

// A throttle for a frame's navigation request when the app handles PDFs
// (Engine::pdf_directory), else null.
std::unique_ptr<blink::URLLoaderThrottle> MaybeCreatePdfThrottle(
    const network::ResourceRequest& request,
    content::FrameTreeNodeId frame_tree_node_id,
    std::optional<int64_t> navigation_id);

// Saves one main-frame PDF response's bytes into a file, reporting progress
// through `notify` (owned by its Tab, which outlives it).
class PdfSaver : public network::mojom::URLLoaderClient,
                 public mojo::DataPipeDrainer::Client {
 public:
  using Notify = base::RepeatingCallback<void(const PdfSaver&,
                                              ne_pdf_event_kind)>;

  PdfSaver(uint64_t id,
           const GURL& url,
           const base::FilePath& path,
           int64_t total_bytes,
           mojo::PendingRemote<network::mojom::URLLoader> loader,
           mojo::PendingReceiver<network::mojom::URLLoaderClient> client,
           mojo::ScopedDataPipeConsumerHandle body,
           Notify notify);
  PdfSaver(const PdfSaver&) = delete;
  PdfSaver& operator=(const PdfSaver&) = delete;
  ~PdfSaver() override;

  // Stops saving (the tab left the PDF's document); FINISHED, cancelled.
  void Cancel();

  uint64_t id() const { return id_; }
  const GURL& url() const { return url_; }
  const base::FilePath& path() const { return path_; }
  int32_t state() const { return state_; }
  int64_t received_bytes() const { return received_; }
  int64_t total_bytes() const { return total_; }
  bool finished() const { return finished_; }

  // network::mojom::URLLoaderClient:
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr hints) override {}
  void OnReceiveResponse(
      network::mojom::URLResponseHeadPtr head,
      mojo::ScopedDataPipeConsumerHandle body,
      std::optional<mojo_base::BigBuffer> cached_metadata) override {}
  void OnReceiveRedirect(const net::RedirectInfo& redirect_info,
                         network::mojom::URLResponseHeadPtr head) override {}
  void OnUploadProgress(int64_t current_position,
                        int64_t total_size,
                        OnUploadProgressCallback callback) override {}
  void OnTransferSizeUpdated(int32_t transfer_size_diff) override {}
  void OnComplete(const network::URLLoaderCompletionStatus& status) override;

  // mojo::DataPipeDrainer::Client:
  void OnDataAvailable(base::span<const uint8_t> data) override;
  void OnDataComplete() override;

 private:
  class File;

  void OnClientGone();
  // Finishes once the body is drained and the loader reported completion.
  void MaybeFinish();
  void Finish(int32_t state);
  void Closed(int32_t state, bool written);

  const uint64_t id_;
  const GURL url_;
  const base::FilePath path_;
  int64_t received_ = 0;
  int64_t total_;
  int32_t state_ = NE_DOWNLOAD_IN_PROGRESS;
  bool body_done_ = false;
  std::optional<int> net_error_;
  bool finishing_ = false;
  bool finished_ = false;
  base::TimeTicks last_progress_;
  Notify notify_;
  base::SequenceBound<File> file_;
  mojo::Remote<network::mojom::URLLoader> loader_;
  mojo::Receiver<network::mojom::URLLoaderClient> client_{this};
  std::unique_ptr<mojo::DataPipeDrainer> drainer_;
  base::WeakPtrFactory<PdfSaver> weak_factory_{this};
};

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_PDF_H_
