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

#include "istream/DelayedIstream.hxx"
#include "istream/istream_string.hxx"
#include "istream/UnusedPtr.hxx"
#include "pool/pool.hxx"
#include "util/Cancellable.hxx"

#include <stdio.h>

#define EXPECTED_RESULT "foo"

class EventLoop;

struct DelayedTest final : Cancellable {
    /* virtual methods from class Cancellable */
    void Cancel() noexcept override {
        printf("delayed_abort\n");
    }
};

static UnusedIstreamPtr
create_input(struct pool &pool)
{
    return istream_string_new(pool, "foo");
}

static UnusedIstreamPtr
create_test(EventLoop &event_loop, struct pool &pool,
            UnusedIstreamPtr input) noexcept
{
    auto *test = NewFromPool<DelayedTest>(pool);

    auto delayed = istream_delayed_new(pool, event_loop);
    delayed.second.cancel_ptr = *test;
    delayed.second.Set(std::move(input));
    return std::move(delayed.first);
}

#include "t_istream_filter.hxx"
