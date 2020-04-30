/*
 * Copyright 2007-2020 CM4all GmbH
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

#include "FileIstream.hxx"
#include "FdIstream.hxx"
#include "istream.hxx"
#include "New.hxx"
#include "Result.hxx"
#include "io/Buffered.hxx"
#include "io/Open.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "pool/pool.hxx"
#include "fb_pool.hxx"
#include "SliceFifoBuffer.hxx"
#include "system/Error.hxx"
#include "event/TimerEvent.hxx"
#include "util/RuntimeError.hxx"

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

/**
 * If EAGAIN occurs (on NFS), we try again after 100ms.  We can't
 * check SocketEvent::READ, because the kernel always indicates VFS files as
 * "readable without blocking".
 */
static constexpr Event::Duration file_retry_timeout = std::chrono::milliseconds(100);

class FileIstream final : public Istream {
	UniqueFileDescriptor fd;

	/**
	 * A timer to retry reading after EAGAIN.
	 */
	TimerEvent retry_event;

	off_t rest;
	SliceFifoBuffer buffer;
	const char *path;

public:
	FileIstream(struct pool &p, EventLoop &event_loop,
		    UniqueFileDescriptor &&_fd, off_t _length,
		    const char *_path) noexcept
		:Istream(p),
		 fd(std::move(_fd)),
		 retry_event(event_loop, BIND_THIS_METHOD(EventCallback)),
		 rest(_length),
		 path(_path) {}

	~FileIstream() noexcept {
		buffer.FreeIfDefined();
	}

private:
	void EofDetected() noexcept {
		assert(fd.IsDefined());

		DestroyEof();
	}

	gcc_pure
	size_t GetMaxRead() const noexcept {
		if (rest < (off_t)INT_MAX)
			return (size_t)rest;
		else
			return INT_MAX;
	}

	void TryData();
	void TryDirect();

	void TryRead() noexcept {
		try {
			if (CheckDirect(FdType::FD_FILE))
				TryDirect();
			else
				TryData();
		} catch (...) {
			DestroyError(std::current_exception());
		}
	}

	void EventCallback() noexcept {
		TryRead();
	}

	/* virtual methods from class Istream */

	off_t _GetAvailable(bool partial) noexcept override;
	off_t _Skip(gcc_unused off_t length) noexcept override;

	void _Read() noexcept override {
		retry_event.Cancel();
		TryRead();
	}

	int _AsFd() noexcept override;
	void _Close() noexcept override {
		Destroy();
	}
};

inline void
FileIstream::TryData()
{
	if (buffer.IsNull()) {
		if (rest != 0)
			buffer.Allocate(fb_pool_get());
	} else if (!buffer.empty()) {
		if (SendFromBuffer(buffer) == 0)
			/* not a single byte was consumed: we may have
			   been closed, and we must bail out now */
			return;
	}

	if (rest == 0) {
		if (buffer.empty())
			EofDetected();
		return;
	}

	ssize_t nbytes = read_to_buffer(fd.Get(), buffer, GetMaxRead());
	if (nbytes == 0) {
		throw FormatRuntimeError("premature end of file in '%s'",
					 path);
	} else if (nbytes == -1) {
		throw FormatErrno("Failed to read from '%s'", path);
	} else if (nbytes > 0) {
		rest -= (off_t)nbytes;
		assert(rest >= 0);
	}

	assert(!buffer.empty());

	if (ConsumeFromBuffer(buffer) == 0 && rest == 0)
		EofDetected();
}

inline void
FileIstream::TryDirect()
{
	/* first consume the rest of the buffer */
	if (ConsumeFromBuffer(buffer) > 0)
		return;

	if (rest == 0) {
		EofDetected();
		return;
	}

	ssize_t nbytes = InvokeDirect(FdType::FD_FILE, fd.Get(), GetMaxRead());
	if (nbytes == ISTREAM_RESULT_CLOSED)
		/* this stream was closed during the direct() callback */
		return;

	if (nbytes > 0 || nbytes == ISTREAM_RESULT_BLOCKING) {
		/* -2 means the callback wasn't able to consume any data right
		   now */
		if (nbytes > 0) {
			rest -= (off_t)nbytes;
			assert(rest >= 0);
			if (rest == 0)
				EofDetected();
		}
	} else if (nbytes == ISTREAM_RESULT_EOF) {
		throw FormatRuntimeError("premature end of file in '%s'", path);
	} else if (errno == EAGAIN) {
		/* this should only happen for splice(SPLICE_F_NONBLOCK) from
		   NFS files - unfortunately we cannot use SocketEvent::READ
		   here, so we just install a timer which retries after
		   100ms */

		retry_event.Schedule(file_retry_timeout);
	} else {
		/* XXX */
		throw FormatErrno("Failed to read from '%s'", path);
	}
}

/*
 * istream implementation
 *
 */

off_t
FileIstream::_GetAvailable(bool) noexcept
{
	return rest + buffer.GetAvailable();
}

off_t
FileIstream::_Skip(off_t length) noexcept
{
	retry_event.Cancel();

	if (length == 0)
		return 0;

	const size_t buffer_available = buffer.GetAvailable();
	if (length < off_t(buffer_available)) {
		buffer.Consume(length);
		Consumed(length);
		return length;
	}

	length -= buffer_available;
	buffer.Clear();

	if (length >= rest) {
		/* skip beyond EOF */

		length = rest;
		rest = 0;
	} else {
		/* seek the file descriptor */

		if (fd.Skip(length) < 0)
			return -1;
		rest -= length;
	}

	off_t result = buffer_available + length;
	Consumed(result);
	return result;
}

int
FileIstream::_AsFd() noexcept
{
	int result_fd = fd.Steal();

	Destroy();

	return result_fd;
}

/*
 * constructor and public methods
 *
 */

UnusedIstreamPtr
istream_file_fd_new(EventLoop &event_loop, struct pool &pool,
		    const char *path, UniqueFileDescriptor fd,
		    off_t length) noexcept
{
	assert(fd.IsDefined());
	assert(length >= -1);

	return NewIstreamPtr<FileIstream>(pool, event_loop,
					  std::move(fd),
					  length, path);
}

UnusedIstreamPtr
istream_file_new(EventLoop &event_loop, struct pool &pool,
		 const char *path)
{
	auto fd = OpenReadOnly(path);

	struct stat st;
	if (fstat(fd.Get(), &st) < 0)
		throw FormatErrno("Failed to stat %s", path);

	if (!S_ISREG(st.st_mode)) {
		FdType fd_type = FdType::FD_NONE;
		if (S_ISCHR(st.st_mode))
			fd_type = FdType::FD_CHARDEV;
		else if (S_ISFIFO(st.st_mode))
			fd_type = FdType::FD_PIPE;
		else if (S_ISSOCK(st.st_mode))
			fd_type = FdType::FD_SOCKET;
		return NewFdIstream(event_loop, pool, path,
				    std::move(fd), fd_type);
	}

	return istream_file_fd_new(event_loop, pool, path,
				   std::move(fd), st.st_size);
}
