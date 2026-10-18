// Copyright 2026 Notolibra. All rights reserved.
//
// Content blocking in the browser process (plan.md §4.6): every subresource
// and subframe request of a page goes through a proxy in front of its
// URLLoaderFactory, on the IO thread, which asks the app's filter
// (ne_app_set_request_filter) and then passes the request on unchanged,
// fails it with ERR_BLOCKED_BY_CLIENT, or answers it with the filter's
// stand-in. The proxy never changes a request it passes on.
//
// (The plan named a URLLoaderThrottle; browser-side throttles only see
// navigations, not a renderer's subresource requests, so the filter sits in
// a factory proxy as Chrome's own interceptors do (URLLoaderFactoryBuilder).)

#ifndef NOTOLIBRA_ENGINE_REQUEST_FILTER_H_
#define NOTOLIBRA_ENGINE_REQUEST_FILTER_H_

#include <cstdint>

#include "ne_engine.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"

namespace notolibra {

// The app's filter, read on the IO thread.
void SetRequestFilter(ne_request_filter_fn fn, void* user_data);
bool HasRequestFilter();

// Puts the proxy in front of a factory for the page `tab_id` (0: none).
void InstallRequestFilter(network::URLLoaderFactoryBuilder& factory_builder,
                          uint64_t tab_id);

}  // namespace notolibra

#endif  // NOTOLIBRA_ENGINE_REQUEST_FILTER_H_
