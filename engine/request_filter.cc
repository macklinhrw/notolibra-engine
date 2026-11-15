// Copyright 2026 The Notolibra Authors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "notolibra/engine/request_filter.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace notolibra {

namespace {

std::atomic<ne_request_filter_fn> g_filter{nullptr};
std::atomic<void*> g_filter_data{nullptr};

// adblock-rust's name for a request's kind (cef_host.rs's request_type).
const char* ResourceType(const network::ResourceRequest& request) {
  using network::mojom::RequestDestination;
  switch (request.destination) {
    case RequestDestination::kDocument:
      return "document";
    case RequestDestination::kIframe:
    case RequestDestination::kFrame:
    case RequestDestination::kFencedframe:
      return "sub_frame";
    case RequestDestination::kStyle:
    case RequestDestination::kXslt:
      return "stylesheet";
    case RequestDestination::kScript:
    case RequestDestination::kWorker:
    case RequestDestination::kSharedWorker:
    case RequestDestination::kServiceWorker:
    case RequestDestination::kAudioWorklet:
    case RequestDestination::kPaintWorklet:
      return "script";
    case RequestDestination::kImage:
      return "image";
    case RequestDestination::kFont:
      return "font";
    case RequestDestination::kObject:
    case RequestDestination::kEmbed:
      return "object";
    case RequestDestination::kAudio:
    case RequestDestination::kVideo:
    case RequestDestination::kTrack:
      return "media";
    case RequestDestination::kReport:
      return "csp_report";
    case RequestDestination::kEmpty:
      // fetch() and XMLHttpRequest; pings are keepalive requests.
      return request.keepalive ? "ping" : "xhr";
    default:
      return "other";
  }
}

// A loader that does nothing, for requests answered without the network
// (its client has its answer already).
class NoopLoader : public network::mojom::URLLoader {
 public:
  void FollowRedirect(network::HttpRequestHeadersUpdateParams headers_update_params,
                      const std::optional<GURL>& new_url) override {}
  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {}
};

// Answers a request with a stand-in body (uBlock Origin's redirect files).
void Respond(mojo::PendingReceiver<network::mojom::URLLoader> loader,
             mojo::PendingRemote<network::mojom::URLLoaderClient> client_remote,
             const std::string& mime_type,
             const std::string& body) {
  mojo::Remote<network::mojom::URLLoaderClient> client(std::move(client_remote));
  auto head = network::mojom::URLResponseHead::New();
  std::string headers = "HTTP/1.1 200 OK\nContent-Type: " + mime_type +
                        "\nAccess-Control-Allow-Origin: *\n\n";
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      net::HttpUtil::AssembleRawHeaders(headers));
  head->mime_type = mime_type;
  head->content_length = body.size();
  mojo::ScopedDataPipeProducerHandle producer;
  mojo::ScopedDataPipeConsumerHandle consumer;
  MojoCreateDataPipeOptions options = {sizeof(options),
                                       MOJO_CREATE_DATA_PIPE_FLAG_NONE, 1,
                                       static_cast<uint32_t>(std::max<size_t>(body.size(), 1))};
  if (mojo::CreateDataPipe(&options, producer, consumer) != MOJO_RESULT_OK) {
    client->OnComplete(network::URLLoaderCompletionStatus(net::ERR_FAILED));
    return;
  }
  size_t written = 0;
  if (!body.empty()) {
    producer->WriteData(base::as_byte_span(body), MOJO_WRITE_DATA_FLAG_ALL_OR_NONE,
                        written);
  }
  producer.reset();
  client->OnReceiveResponse(std::move(head), std::move(consumer), std::nullopt);
  network::URLLoaderCompletionStatus status(net::OK);
  status.decoded_body_length = base::ByteSize(body.size());
  status.encoded_data_length = base::ByteSize(body.size());
  client->OnComplete(status);
  mojo::MakeSelfOwnedReceiver(std::make_unique<NoopLoader>(), std::move(loader));
}

// The proxy, on the IO thread. It lives while any of its receivers does.
class FilterFactory : public network::mojom::URLLoaderFactory {
 public:
  static void Create(
      uint64_t tab_id,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> target) {
    new FilterFactory(tab_id, std::move(receiver), std::move(target));
  }

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    ne_request_filter_fn filter = g_filter.load();
    const char* type = ResourceType(request);
    // Top-level pages are never filtered.
    if (filter && std::string_view(type) != "document") {
      std::string url = request.url.spec();
      std::string initiator =
          request.request_initiator ? request.request_initiator->Serialize()
                                    : std::string();
      if (initiator == "null") {
        initiator.clear();
      }
      std::string method = base::ToUpperASCII(request.method);
      ne_request_info info = {sizeof(ne_request_info)};
      info.tab_id = tab_id_;
      info.url = {url.data(), url.size()};
      info.initiator = {initiator.data(), initiator.size()};
      info.resource_type = {type, strlen(type)};
      info.method = {method.data(), method.size()};
      ne_filter_response response = {sizeof(ne_filter_response)};
      const int32_t decision = filter(g_filter_data.load(), &info, &response);
      if (decision == NE_FILTER_BLOCK) {
        mojo::Remote<network::mojom::URLLoaderClient>(std::move(client))
            ->OnComplete(network::URLLoaderCompletionStatus(
                net::ERR_BLOCKED_BY_CLIENT));
        return;
      }
      if (decision == NE_FILTER_RESPOND) {
        std::string mime = response.mime_type.data
                               ? std::string(response.mime_type.data,
                                             response.mime_type.length)
                               : "application/octet-stream";
        // SAFETY: the filter's body is body_size bytes, valid until its next
        // call on this thread.
        std::string body =
            response.body
                ? std::string(reinterpret_cast<const char*>(response.body),
                              response.body_size)
                : std::string();
        Respond(std::move(loader), std::move(client), mime, body);
        return;
      }
    }
    target_->CreateLoaderAndStart(std::move(loader), request_id, options,
                                  request, std::move(client),
                                  traffic_annotation);
  }

  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver)
      override {
    receivers_.Add(this, std::move(receiver));
  }

 private:
  FilterFactory(uint64_t tab_id,
                mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
                mojo::PendingRemote<network::mojom::URLLoaderFactory> target)
      : tab_id_(tab_id), target_(std::move(target)) {
    receivers_.Add(this, std::move(receiver));
    receivers_.set_disconnect_handler(base::BindRepeating(
        &FilterFactory::OnDisconnect, base::Unretained(this)));
    target_.set_disconnect_handler(base::BindOnce(
        &FilterFactory::OnTargetDisconnect, base::Unretained(this)));
  }

  void OnDisconnect() {
    if (receivers_.empty()) {
      delete this;
    }
  }

  void OnTargetDisconnect() { delete this; }

  const uint64_t tab_id_;
  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
};

}  // namespace

void SetRequestFilter(ne_request_filter_fn fn, void* user_data) {
  g_filter_data.store(user_data);
  g_filter.store(fn);
}

bool HasRequestFilter() {
  return g_filter.load() != nullptr;
}

void InstallRequestFilter(network::URLLoaderFactoryBuilder& factory_builder,
                          uint64_t tab_id) {
  auto [receiver, target] = factory_builder.Append();
  content::GetIOThreadTaskRunner({})->PostTask(
      FROM_HERE, base::BindOnce(&FilterFactory::Create, tab_id,
                                std::move(receiver), std::move(target)));
}

}  // namespace notolibra
