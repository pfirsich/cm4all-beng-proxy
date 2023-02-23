// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "istream.hxx"
#include "Handler.hxx"
#include "io/FileDescriptor.hxx"

#include <cassert>

bool
Istream::InvokeReady() noexcept
{
	assert(!destroyed);
	assert(handler != nullptr);
	assert(!in_data);
	assert(!eof);
	assert(!closing);

#ifndef NDEBUG
	const DestructObserver destructed(*this);
#endif

	bool result = handler->OnIstreamReady();

#ifndef NDEBUG
	if (destructed || destroyed) {
		assert(!result);
	}
#endif

	return result;
}

std::size_t
Istream::InvokeData(std::span<const std::byte> src) noexcept
{
	assert(!destroyed);
	assert(handler != nullptr);
	assert(src.data() != nullptr);
	assert(!src.empty());
	assert(!in_data);
	assert(!eof);
	assert(!closing);
	assert(src.size() >= data_available);
	assert(!available_full_set ||
	       (off_t)src.size() <= available_full);

#ifndef NDEBUG
	const DestructObserver destructed(*this);
	in_data = true;
#endif

	std::size_t nbytes = handler->OnData(src);
	assert(nbytes <= src.size());
	assert(nbytes == 0 || !eof);

#ifndef NDEBUG
	if (destructed || destroyed) {
		assert(nbytes == 0);
		return nbytes;
	}

	in_data = false;

	if (nbytes > 0)
		Consumed(nbytes);

	data_available = src.size() - nbytes;
#endif

	return nbytes;
}

IstreamDirectResult
Istream::InvokeDirect(FdType type, FileDescriptor fd, off_t offset,
		      std::size_t max_length) noexcept
{
	assert(!destroyed);
	assert(handler != nullptr);
	assert(fd.IsDefined());
	assert(max_length > 0);
	assert(!in_data);
	assert(!eof);
	assert(!closing);

#ifndef NDEBUG
	const DestructObserver destructed(*this);
	in_data = true;
#endif

	const auto result = handler->OnDirect(type, fd, offset, max_length);
	assert(result == IstreamDirectResult::CLOSED || !eof);

#ifndef NDEBUG
	if (destructed || destroyed) {
		assert(result == IstreamDirectResult::CLOSED);
		return result;
	}

	assert(result != IstreamDirectResult::CLOSED);

	in_data = false;
#endif

	return result;
}

IstreamHandler &
Istream::PrepareEof() noexcept
{
	assert(!destroyed);
	assert(!eof);
	assert(!closing);
	assert(data_available == 0);
	assert(available_partial == 0);
	assert(!available_full_set || available_full == 0);
	assert(handler != nullptr);

#ifndef NDEBUG
	eof = true;
#endif

	return *handler;
}

void
Istream::InvokeEof() noexcept
{
	PrepareEof().OnEof();
}

void
Istream::DestroyEof() noexcept
{
	auto &_handler = PrepareEof();
	Destroy();
	_handler.OnEof();
}

IstreamHandler &
Istream::PrepareError() noexcept
{
	assert(!destroyed);
	assert(!eof);
	assert(!closing);
	assert(handler != nullptr);

#ifndef NDEBUG
	eof = true;
#endif

	return *handler;
}

void
Istream::InvokeError(std::exception_ptr ep) noexcept
{
	assert(ep);

	PrepareError().OnError(ep);
}

void
Istream::DestroyError(std::exception_ptr ep) noexcept
{
	auto &_handler = PrepareError();
	Destroy();
	_handler.OnError(ep);
}
