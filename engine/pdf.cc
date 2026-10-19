// Copyright 2026 Notolibra. All rights reserved.

#include "notolibra/engine/pdf.h"

#include <tuple>
#include <utility>
#include <vector>

#include "base/byte_size.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "content/public/browser/download_utils.h"
#include "content/public/browser/web_contents.h"
#include "net/base/filename_util.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "notolibra/engine/engine.h"
#include "notolibra/engine/tab.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/mojom/parsed_headers.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace notolibra {

namespace {

constexpr char kPdfMimeType[] = "application/pdf";
// Progress reaches the app at most this often.
constexpr base::TimeDelta kProgressInterval = base::Milliseconds(100);

bool IsPdfResponse(const GURL& url, network::mojom::URLResponseHead* head) {
  if (head->mime_type == "text/pdf") {
    // An alias of application/pdf (Chrome's interceptor does the same).
    head->mime_type = kPdfMimeType;
  }
  if (head->mime_type != kPdfMimeType) {
    return false;
  }
  // A text/plain response sniffed as a PDF stays text, as in Chrome (its
  // viewer refuses HTTP(S) responses served as text/plain).
  std::string header_mime_type;
  if (url.SchemeIsHTTPOrHTTPS() && head->headers &&
      head->headers->GetMimeType(&header_mime_type) &&
      header_mime_type == "text/plain") {
    return false;
  }
  return true;
}

// The response's CSP would apply to the engine's document; only its
// frame-ancestors directive still matters (who may frame the PDF). From
// Chrome's PluginResponseInterceptorURLLoaderThrottle
// (chrome/browser/plugins/, BSD-3-Clause).
void ClearAllButFrameAncestors(network::mojom::URLResponseHead* head) {
  if (head->headers) {
    head->headers->RemoveHeader("Content-Security-Policy");
    head->headers->RemoveHeader("Content-Security-Policy-Report-Only");
  }
  if (!head->parsed_headers) {
    return;
  }
  std::vector<network::mojom::ContentSecurityPolicyPtr>& csp =
      head->parsed_headers->content_security_policy;
  std::vector<network::mojom::ContentSecurityPolicyPtr> cleared;
  for (auto& policy : csp) {
    auto frame_ancestors = policy->directives.find(
        network::mojom::CSPDirectiveName::FrameAncestors);
    if (frame_ancestors == policy->directives.end()) {
      continue;
    }
    auto cleared_policy = network::mojom::ContentSecurityPolicy::New();
    cleared_policy->self_origin = std::move(policy->self_origin);
    cleared_policy->header = std::move(policy->header);
    cleared_policy->header->header_value = "";
    cleared_policy
        ->directives[network::mojom::CSPDirectiveName::FrameAncestors] =
        std::move(frame_ancestors->second);
    auto raw_frame_ancestors = policy->raw_directives.find(
        network::mojom::CSPDirectiveName::FrameAncestors);
    if (raw_frame_ancestors != policy->raw_directives.end()) {
      cleared_policy->header->header_value =
          "frame-ancestors " + raw_frame_ancestors->second;
      if (head->headers) {
        head->headers->AddHeader(
            cleared_policy->header->type ==
                    network::mojom::ContentSecurityPolicyType::kEnforce
                ? "Content-Security-Policy"
                : "Content-Security-Policy-Report-Only",
            cleared_policy->header->header_value);
      }
      cleared_policy
          ->raw_directives[network::mojom::CSPDirectiveName::FrameAncestors] =
          std::move(raw_frame_ancestors->second);
    }
    cleared.push_back(std::move(cleared_policy));
  }
  csp.swap(cleared);
}

class PdfThrottle : public blink::URLLoaderThrottle {
 public:
  PdfThrottle(bool main_frame,
              content::FrameTreeNodeId frame_tree_node_id,
              int64_t navigation_id)
      : main_frame_(main_frame),
        frame_tree_node_id_(frame_tree_node_id),
        navigation_id_(navigation_id) {}

  const char* NameForLoggingWillProcessResponse() override {
    return "NotolibraPdfThrottle";
  }

  void WillProcessResponse(const GURL& response_url,
                           network::mojom::URLResponseHead* head,
                           bool* defer) override {
    if (!IsPdfResponse(response_url, head)) {
      return;
    }
    Engine* engine = GetEngine();
    content::WebContents* contents =
        content::WebContents::FromFrameTreeNodeId(frame_tree_node_id_);
    Tab* tab = contents ? Tab::FromWebContents(contents) : nullptr;
    if (!engine || engine->pdf_directory.empty() || !tab) {
      return;
    }
    // Content-Disposition: attachment still downloads, as in Chrome.
    if (content::download_utils::MustDownload(contents->GetBrowserContext(),
                                              response_url, head->headers.get(),
                                              head->mime_type)) {
      return;
    }
    if (contents->IsPrerenderedFrame(frame_tree_node_id_)) {
      delegate_->CancelWithError(net::ERR_BLOCKED_BY_CLIENT,
                                 "PDFs aren't prerendered");
      return;
    }
    std::string disposition;
    if (head->headers) {
      disposition =
          head->headers->GetNormalizedHeader("Content-Disposition")
              .value_or(std::string());
    }
    base::FilePath name = net::GenerateFileName(
        response_url, disposition, std::string(), std::string(), kPdfMimeType,
        "document");
    if (!base::EqualsCaseInsensitiveASCII(name.Extension(), ".pdf")) {
      name = name.AddExtensionASCII("pdf");
    }
    const std::string file_name = name.BaseName().value();

    const std::string payload =
        tab->PdfDocumentHtml(file_name, response_url, main_frame_);
    ClearAllButFrameAncestors(head);
    const int64_t total = head->content_length;

    // The engine's document replaces the body; the original stream is kept
    // (main frame) or dropped (a subframe's card).
    mojo::PendingRemote<network::mojom::URLLoader> new_loader;
    std::ignore = new_loader.InitWithNewPipeAndPassReceiver();
    mojo::Remote<network::mojom::URLLoaderClient> new_client;
    mojo::PendingReceiver<network::mojom::URLLoaderClient> new_client_receiver =
        new_client.BindNewPipeAndPassReceiver();
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle body;
    CHECK_EQ(mojo::CreateDataPipe(payload.size(), producer, body),
             MOJO_RESULT_OK);
    CHECK_EQ(producer->WriteAllData(base::as_byte_span(payload)),
             MOJO_RESULT_OK);
    network::URLLoaderCompletionStatus status(net::OK);
    status.decoded_body_length = base::ByteSize(payload.size());
    new_client->OnComplete(status);

    mojo::PendingRemote<network::mojom::URLLoader> original_loader;
    mojo::PendingReceiver<network::mojom::URLLoaderClient> original_client;
    delegate_->InterceptResponse(std::move(new_loader),
                                 std::move(new_client_receiver),
                                 &original_loader, &original_client, &body);

    head->mime_type = "text/html";
    head->charset = "utf-8";
    // Not a download (its MIME type is no longer the PDF's anyway), and
    // the engine's markup may use inline style.
    head->intercepted_by_plugin = true;
    head->client_side_content_decoding_types.clear();

    if (main_frame_) {
      tab->StartPdf(navigation_id_, response_url, file_name, total,
                    std::move(original_loader), std::move(original_client),
                    std::move(body));
    }
    // A subframe's card needs nothing of the PDF: dropping the loader
    // cancels its request.
  }

 private:
  const bool main_frame_;
  const content::FrameTreeNodeId frame_tree_node_id_;
  const int64_t navigation_id_;
};

}  // namespace

std::unique_ptr<blink::URLLoaderThrottle> MaybeCreatePdfThrottle(
    const network::ResourceRequest& request,
    content::FrameTreeNodeId frame_tree_node_id,
    std::optional<int64_t> navigation_id) {
  Engine* engine = GetEngine();
  if (!engine || engine->pdf_directory.empty() || !navigation_id ||
      !frame_tree_node_id) {
    return nullptr;
  }
  using network::mojom::RequestDestination;
  const RequestDestination destination = request.destination;
  const bool main_frame = destination == RequestDestination::kDocument;
  if (!main_frame && destination != RequestDestination::kIframe &&
      destination != RequestDestination::kFrame &&
      destination != RequestDestination::kEmbed &&
      destination != RequestDestination::kObject) {
    return nullptr;
  }
  return std::make_unique<PdfThrottle>(main_frame, frame_tree_node_id,
                                       *navigation_id);
}

// The file, on a sequence that may block.
class PdfSaver::File {
 public:
  explicit File(const base::FilePath& path) {
    if (base::CreateDirectory(path.DirName())) {
      file_.Initialize(path,
                       base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_WRITE);
    }
    ok_ = file_.IsValid();
    if (!ok_) {
      LOG(ERROR) << "NotolibraEngine: cannot write " << path;
    }
  }

  void Write(std::vector<uint8_t> data) {
    if (ok_ && !file_.WriteAtCurrentPosAndCheck(data)) {
      ok_ = false;
    }
  }

  // Whether everything was written.
  bool Close() {
    file_.Close();
    return ok_;
  }

 private:
  base::File file_;
  bool ok_ = false;
};

PdfSaver::PdfSaver(uint64_t id,
                   const GURL& url,
                   const base::FilePath& path,
                   int64_t total_bytes,
                   mojo::PendingRemote<network::mojom::URLLoader> loader,
                   mojo::PendingReceiver<network::mojom::URLLoaderClient> client,
                   mojo::ScopedDataPipeConsumerHandle body,
                   Notify notify)
    : id_(id),
      url_(url),
      path_(path),
      total_(total_bytes > 0 ? total_bytes : -1),
      notify_(std::move(notify)),
      file_(base::ThreadPool::CreateSequencedTaskRunner(
                {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
                 base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN}),
            path) {
  if (loader) {
    loader_.Bind(std::move(loader));
  }
  if (client) {
    client_.Bind(std::move(client));
    client_.set_disconnect_handler(
        base::BindOnce(&PdfSaver::OnClientGone, base::Unretained(this)));
  } else {
    net_error_ = net::OK;
  }
  if (body) {
    drainer_ = std::make_unique<mojo::DataPipeDrainer>(this, std::move(body));
  } else {
    body_done_ = true;
  }
}

PdfSaver::~PdfSaver() = default;

void PdfSaver::Cancel() {
  if (!finishing_) {
    Finish(NE_DOWNLOAD_CANCELLED);
  }
}

void PdfSaver::OnComplete(const network::URLLoaderCompletionStatus& status) {
  net_error_ = status.error_code;
  MaybeFinish();
}

void PdfSaver::OnClientGone() {
  if (!net_error_) {
    // No completion: the request was dropped.
    net_error_ = net::ERR_ABORTED;
  }
  MaybeFinish();
}

void PdfSaver::OnDataAvailable(base::span<const uint8_t> data) {
  if (finishing_) {
    return;
  }
  received_ += static_cast<int64_t>(data.size());
  file_.AsyncCall(&File::Write)
      .WithArgs(std::vector<uint8_t>(data.begin(), data.end()));
  const base::TimeTicks now = base::TimeTicks::Now();
  if (now - last_progress_ >= kProgressInterval) {
    last_progress_ = now;
    notify_.Run(*this, NE_PDF_PROGRESS);
  }
}

void PdfSaver::OnDataComplete() {
  body_done_ = true;
  MaybeFinish();
}

void PdfSaver::MaybeFinish() {
  if (finishing_ || !net_error_ || (*net_error_ == net::OK && !body_done_)) {
    return;
  }
  Finish(*net_error_ == net::OK ? NE_DOWNLOAD_COMPLETE
                                : NE_DOWNLOAD_INTERRUPTED);
}

void PdfSaver::Finish(int32_t state) {
  finishing_ = true;
  drainer_.reset();
  client_.reset();
  loader_.reset();
  // Reported once everything queued is on disk.
  file_.AsyncCall(&File::Close).Then(
      base::BindOnce(&PdfSaver::Closed, weak_factory_.GetWeakPtr(), state));
}

void PdfSaver::Closed(int32_t state, bool written) {
  if (state == NE_DOWNLOAD_COMPLETE && !written) {
    state = NE_DOWNLOAD_INTERRUPTED;
  }
  if (state == NE_DOWNLOAD_COMPLETE && total_ < 0) {
    total_ = received_;
  }
  state_ = state;
  finished_ = true;
  notify_.Run(*this, NE_PDF_FINISHED);
}

}  // namespace notolibra
