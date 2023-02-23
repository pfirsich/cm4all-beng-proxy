// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "PrometheusExporter.hxx"
#include "Instance.hxx"
#include "prometheus/Stats.hxx"
#include "prometheus/HttpStats.hxx"
#include "http/Headers.hxx"
#include "http/IncomingRequest.hxx"
#include "http/ResponseHandler.hxx"
#include "memory/istream_gb.hxx"
#include "memory/GrowingBuffer.hxx"

void
BpPrometheusExporter::HandleHttpRequest(IncomingHttpRequest &request,
					const StopwatchPtr &,
					CancellablePointer &) noexcept
{
	GrowingBuffer buffer;

	const char *process = "bp";
	Prometheus::Write(buffer, process, instance.GetStats());

	for (const auto &[name, stats] : instance.listener_stats)
		Prometheus::Write(buffer, process, name.c_str(), stats);

	HttpHeaders headers;
	headers.Write("content-type", "text/plain;version=0.0.4");

	request.SendResponse(HttpStatus::OK, std::move(headers),
			     istream_gb_new(request.pool, std::move(buffer)));
}
