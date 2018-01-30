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

#include "istream_memory.hxx"
#include "istream.hxx"
#include "Bucket.hxx"
#include "UnusedPtr.hxx"
#include "util/ConstBuffer.hxx"
#include "util/Cast.hxx"

#include <algorithm>

#include <stdint.h>

class MemoryIstream final : public Istream {
    ConstBuffer<uint8_t> data;

public:
    MemoryIstream(struct pool &p, const void *_data, size_t length)
        :Istream(p),
         data((const uint8_t *)_data, length) {}

    /* virtual methods from class Istream */

    off_t _GetAvailable(gcc_unused bool partial) noexcept override {
        return data.size;
    }

    off_t _Skip(off_t length) noexcept override {
        size_t nbytes = std::min(off_t(data.size), length);
        data.skip_front(nbytes);
        Consumed(nbytes);
        return nbytes;
    }

    void _Read() noexcept override {
        if (!data.empty()) {
            auto nbytes = InvokeData(data.data, data.size);
            if (nbytes == 0)
                return;

            data.skip_front(nbytes);
        }

        if (data.empty())
            DestroyEof();
    }

    void _FillBucketList(IstreamBucketList &list) noexcept override {
        if (!data.empty())
            list.Push(data.ToVoid());
    }

    size_t _ConsumeBucketList(size_t nbytes) noexcept override {
        if (nbytes > data.size)
            nbytes = data.size;
        data.skip_front(nbytes);
        Consumed(nbytes);
        return nbytes;
    }
};

UnusedIstreamPtr
istream_memory_new(struct pool &pool, const void *data, size_t length) noexcept
{
    return UnusedIstreamPtr(NewIstream<MemoryIstream>(pool, data, length));
}
