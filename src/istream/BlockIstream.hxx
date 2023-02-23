// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

struct pool;
class UnusedIstreamPtr;

/**
 * #Istream implementation which blocks indefinitely until closed.
 */
UnusedIstreamPtr
istream_block_new(struct pool &pool) noexcept;
