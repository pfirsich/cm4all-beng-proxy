// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

struct pool;
class UnusedIstreamPtr;

UnusedIstreamPtr
istream_iconv_new(struct pool &pool, UnusedIstreamPtr input,
		  const char *tocode, const char *fromcode) noexcept;
