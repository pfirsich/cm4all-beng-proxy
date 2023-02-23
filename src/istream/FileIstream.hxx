// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include <sys/types.h>

struct pool;
class UnusedIstreamPtr;
class EventLoop;
class UniqueFileDescriptor;

UnusedIstreamPtr
istream_file_fd_new(EventLoop &event_loop, struct pool &pool,
		    const char *path, UniqueFileDescriptor fd,
		    off_t start_offset, off_t end_offset) noexcept;
