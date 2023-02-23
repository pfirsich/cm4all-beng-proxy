// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "ResourceLoader.hxx"

class HttpCache;

/**
 * A #ResourceLoader implementation which sends HTTP requests through
 * the HTTP cache.
 */
class CachedResourceLoader final : public ResourceLoader {
	HttpCache &cache;

public:
	explicit CachedResourceLoader(HttpCache &_cache) noexcept
		:cache(_cache) {}

	/* virtual methods from class ResourceLoader */
	void SendRequest(struct pool &pool,
			 const StopwatchPtr &parent_stopwatch,
			 const ResourceRequestParams &params,
			 HttpMethod method,
			 const ResourceAddress &address,
			 HttpStatus status, StringMap &&headers,
			 UnusedIstreamPtr body, const char *body_etag,
			 HttpResponseHandler &handler,
			 CancellablePointer &cancel_ptr) noexcept override;
};
