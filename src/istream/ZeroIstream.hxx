// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

struct pool;
class UnusedIstreamPtr;

/**
 * istream implementation which reads zero bytes.
 */
UnusedIstreamPtr
istream_zero_new(struct pool &pool) noexcept;
