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

#include "file_request.hxx"
#include "static_headers.hxx"
#include "HttpResponseHandler.hxx"
#include "strmap.hxx"
#include "istream/UnusedPtr.hxx"
#include "istream/FileIstream.hxx"
#include "pool/pool.hxx"
#include "io/Open.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "system/Error.hxx"
#include "http/Status.h"

#include <assert.h>
#include <sys/stat.h>

void
static_file_get(EventLoop &event_loop, struct pool &pool,
		const char *path, const char *content_type,
		HttpResponseHandler &handler)
{
	assert(path != nullptr);

	struct stat st;
	if (lstat(path, &st) != 0) {
		handler.InvokeError(std::make_exception_ptr(FormatErrno("Failed to open %s", path)));
		return;
	}

	if (!S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode)) {
		handler.InvokeResponse(pool, HTTP_STATUS_NOT_FOUND,
				       "Not a regular file");
		return;
	}

	UniqueFileDescriptor fd;

	try {
		fd = OpenReadOnly(path);
	} catch (...) {
		handler.InvokeError(std::current_exception());
		return;
	}

	FdType fd_type = FdType::FD_FILE;
	off_t size = st.st_size;

	if (S_ISCHR(st.st_mode)) {
		fd_type = FdType::FD_CHARDEV;
		size = -1;
	}

	auto headers = static_response_headers(pool, fd, st, content_type);

	auto *body = istream_file_fd_new(event_loop, pool, path,
					 std::move(fd),
					 fd_type, size);

	handler.InvokeResponse(HTTP_STATUS_OK,
			       std::move(headers),
			       UnusedIstreamPtr(body));
}
