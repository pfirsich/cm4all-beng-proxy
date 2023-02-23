// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "MirrorResourceLoader.hxx"
#include "istream/UnusedPtr.hxx"
#include "http/ResponseHandler.hxx"

void
MirrorResourceLoader::SendRequest(struct pool &,
				  const StopwatchPtr &,
				  const ResourceRequestParams &,
				  HttpMethod,
				  const ResourceAddress &,
				  HttpStatus,
				  StringMap &&headers,
				  UnusedIstreamPtr body, const char *,
				  HttpResponseHandler &handler,
				  CancellablePointer &) noexcept
{
	handler.InvokeResponse(body ? HttpStatus::OK : HttpStatus::NO_CONTENT,
			       std::move(headers), std::move(body));
}
