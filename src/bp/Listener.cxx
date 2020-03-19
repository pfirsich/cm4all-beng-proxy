/*
 * Copyright 2007-2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "Listener.hxx"
#include "Connection.hxx"
#include "Instance.hxx"
#include "pool/UniquePtr.hxx"
#include "ssl/Factory.hxx"
#include "ssl/Filter.hxx"
#include "ssl/SniCallback.hxx"
#include "fs/FilteredSocket.hxx"
#include "fs/Ptr.hxx"
#include "fs/ThreadSocketFilter.hxx"
#include "net/SocketAddress.hxx"
#include "io/FdType.hxx"
#include "io/Logger.hxx"
#include "thread/Pool.hxx"
#include "util/Exception.hxx"

BPListener::BPListener(BpInstance &_instance, const char *_tag,
		       bool _auth_alt_host,
		       const SslConfig *ssl_config)
	:instance(_instance),
	 tag(_tag),
	 auth_alt_host(_auth_alt_host),
	 listener(instance.root_pool, instance.event_loop,
		  ssl_config ? ssl_factory_new_server(*ssl_config, nullptr) : nullptr,
		  *this)
{
}

BPListener::~BPListener() noexcept = default;

void
BPListener::OnFilteredSocketConnect(PoolPtr pool,
				    UniquePoolPtr<FilteredSocket> socket,
				    SocketAddress address,
				    const SslFilter *) noexcept
{
	new_connection(std::move(pool), instance,
		       std::move(socket),
		       address,
		       tag, auth_alt_host);
}

void
BPListener::OnFilteredSocketError(std::exception_ptr ep) noexcept
{
	LogConcat(2, "listener", ep);
}
