// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "RLogger.hxx"
#include "Instance.hxx"
#include "access_log/Glue.hxx"
#include "http/IncomingRequest.hxx"

BpRequestLogger::BpRequestLogger(BpInstance &_instance,
				 TaggedHttpStats &_http_stats,
				 bool _access_logger) noexcept
	:instance(_instance), http_stats(_http_stats),
	 start_time(instance.event_loop.SteadyNow()),
	 access_logger(_access_logger)
{
}

void
BpRequestLogger::LogHttpRequest(IncomingHttpRequest &request,
				HttpStatus status, int64_t length,
				uint64_t bytes_received, uint64_t bytes_sent) noexcept
{
	const auto duration = GetDuration(instance.event_loop.SteadyNow());

	instance.http_stats.AddRequest(status,
				       bytes_received, bytes_sent,
				       duration);

	http_stats.AddRequest(stats_tag, status,
			      bytes_received, bytes_sent,
			      duration);

	if (access_logger && instance.access_log != nullptr)
		instance.access_log->Log(instance.event_loop.SystemNow(),
					 request, site_name,
					 analytics_id,
					 nullptr,
					 request.headers.Get("referer"),
					 request.headers.Get("user-agent"),
					 status, length,
					 bytes_received, bytes_sent,
					 duration);
}
