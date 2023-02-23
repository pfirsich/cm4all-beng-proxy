// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "Listener.hxx"
#include "Connection.hxx"
#include "Instance.hxx"
#include "PrometheusExporter.hxx"
#include "pool/UniquePtr.hxx"
#include "ssl/Factory.hxx"
#include "ssl/Filter.hxx"
#include "ssl/CertCallback.hxx"
#include "ssl/AlpnProtos.hxx"
#include "fs/FilteredSocket.hxx"
#include "net/SocketAddress.hxx"
#include "io/Logger.hxx"

static std::unique_ptr<SslFactory>
MakeSslFactory(const SslConfig *ssl_config)
{
	if (ssl_config == nullptr)
		return nullptr;

	auto ssl_factory = std::make_unique<SslFactory>(*ssl_config, nullptr);
	// TODO: call SetSessionIdContext()

#ifdef HAVE_NGHTTP2
	ssl_factory->AddAlpn(alpn_http_any);
#endif

	return ssl_factory;
}

BPListener::BPListener(BpInstance &_instance,
		       TaggedHttpStats &_http_stats,
		       std::shared_ptr<TranslationService> _translation_service,
		       const char *_tag,
		       bool _prometheus_exporter,
		       bool _auth_alt_host,
		       const SslConfig *ssl_config)
	:instance(_instance),
	 http_stats(_http_stats),
	 translation_service(_translation_service),
	 prometheus_exporter(_prometheus_exporter
			     ? new BpPrometheusExporter(instance)
			     : nullptr),
	 tag(_tag),
	 auth_alt_host(_auth_alt_host),
	 listener(instance.root_pool, instance.event_loop,
		  MakeSslFactory(ssl_config),
		  *this)
{
}

BPListener::~BPListener() noexcept = default;

void
BPListener::OnFilteredSocketConnect(PoolPtr pool,
				    UniquePoolPtr<FilteredSocket> socket,
				    SocketAddress address,
				    const SslFilter *ssl_filter) noexcept
{
	new_connection(std::move(pool), instance, *this,
		       prometheus_exporter.get(),
		       std::move(socket), ssl_filter,
		       address);
}

void
BPListener::OnFilteredSocketError(std::exception_ptr ep) noexcept
{
	LogConcat(2, "listener", ep);
}
