/*
 * This istream filter removes HTTP chunking.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "istream_dechunk.hxx"
#include "FacadeIstream.hxx"
#include "pool.hxx"

#include <algorithm>

#include <assert.h>
#include <string.h>

enum istream_dechunk_state {
    NONE,
    CLOSED,
    SIZE,
    AFTER_SIZE,
    DATA,
    AFTER_DATA,
    TRAILER,
    TRAILER_DATA,
    EOF_DETECTED
};

class DechunkIstream final : public FacadeIstream {
    enum istream_dechunk_state state;

    size_t remaining_chunk;
    bool had_input, had_output;

    /**
     * Copy chunked data verbatim to handler?
     *
     * @see istream_dechunk_check_verbatim()
     */
    bool verbatim = false;

    /**
     * Was the end-of-file chunk seen at the end of #pending_verbatim?
     */
    bool eof_verbatim;

    /**
     * Number of bytes to be passed to handler verbatim, which have
     * already been parsed but have not yet been consumed by the
     * handler.
     */
    size_t pending_verbatim;

    void (*const eof_callback)(void *ctx);
    void *const callback_ctx;

public:
    DechunkIstream(struct pool &p, struct istream &_input,
                   void (*_eof_callback)(void *ctx), void *_callback_ctx)
        :FacadeIstream(p, _input,
                       MakeIstreamHandler<DechunkIstream>::handler, this),
         state(NONE),
         eof_callback(_eof_callback), callback_ctx(_callback_ctx)
    {
    }

    static DechunkIstream *CheckCast(Istream *i) {
        return dynamic_cast<DechunkIstream *>(i);
    }

    static DechunkIstream *CheckCast(struct istream *i) {
        if (!Istream::CheckClass(*i))
            /* not an Istream (OO) instance */
            return nullptr;

        return CheckCast(&Istream::Cast(*i));
    }

    void SetVerbatim() {
        verbatim = true;
        eof_verbatim = false;
        pending_verbatim = 0;
    }

    void Abort(GError *error);

    /**
     * @return false if the istream_dechunk has been aborted
     * indirectly (by a callback)
     */
    bool EofDetected();

    size_t Feed(const void *data, size_t length);

    /* virtual methods from class Istream */

    off_t GetAvailable(bool partial) override;
    void Read() override;
    void Close() override;

    /* handler */
    size_t OnData(const void *data, size_t length);

    ssize_t OnDirect(gcc_unused FdType type, gcc_unused int fd,
                     gcc_unused size_t max_length) {
        gcc_unreachable();
    }

    void OnEof();
    void OnError(GError *error);
};

static GQuark
dechunk_quark(void)
{
    return g_quark_from_static_string("dechunk");
}

void
DechunkIstream::Abort(GError *error)
{
    assert(state != EOF_DETECTED && state != CLOSED);
    assert(input.IsDefined());

    state = CLOSED;

    input.ClearAndClose();
    DestroyError(error);
}

bool
DechunkIstream::EofDetected()
{
    assert(input.IsDefined());
    assert(state == TRAILER);
    assert(remaining_chunk == 0);

    state = EOF_DETECTED;

    eof_callback(callback_ctx);

    assert(input.IsDefined());
    assert(state == EOF_DETECTED);

    const ScopePoolRef ref(GetPool() TRACE_ARGS);
    DestroyEof();

    if (state == CLOSED) {
        assert(!input.IsDefined());

        return false;
    } else {
        /* we must deinitialize the "input" after emitting "eof",
           because we must give the callback a chance to call
           dechunk_input_abort() on us; if we'd clear the handler too
           early, we wouldn't receive that event, and
           dechunk_input_data() couldn't change its return value to
           0 */
        assert(input.IsDefined());

        input.ClearHandler();
        return true;
    }
}

size_t
DechunkIstream::Feed(const void *data0, size_t length)
{
    const char *data = (const char *)data0;
    size_t position = 0, digit, size, nbytes;

    assert(input.IsDefined());
    assert(!verbatim || !eof_verbatim);

    if (verbatim)
        /* skip the part that has already been parsed in the last
           invocation, but could not be consumed by the handler */
        position = pending_verbatim;

    had_input = true;

    while (position < length) {
        switch (state) {
        case NONE:
        case SIZE:
            if (data[position] >= '0' && data[position] <= '9') {
                digit = data[position] - '0';
            } else if (data[position] >= 'a' && data[position] <= 'f') {
                digit = data[position] - 'a' + 0xa;
            } else if (data[position] >= 'A' && data[position] <= 'F') {
                digit = data[position] - 'A' + 0xa;
            } else if (state == SIZE) {
                state = AFTER_SIZE;
                ++position;
                continue;
            } else {
                GError *error =
                    g_error_new_literal(dechunk_quark(), 0,
                                        "chunk length expected");
                Abort(error);
                return 0;
            }

            if (state == NONE) {
                state = SIZE;
                remaining_chunk = 0;
            }

            ++position;
            remaining_chunk = remaining_chunk * 0x10 + digit;
            break;

        case CLOSED:
            assert(0);
            break;

        case AFTER_SIZE:
            if (data[position++] == '\n') {
                if (remaining_chunk == 0)
                    state = TRAILER;
                else
                    state = DATA;
            }
            break;

        case DATA:
            assert(remaining_chunk > 0);

            size = length - position;
            if (size > remaining_chunk)
                size = remaining_chunk;

            if (verbatim) {
                /* postpone this data chunk; try to send it all later
                   in one big block */
                nbytes = size;
            } else {
                had_output = true;
                nbytes = InvokeData(data + position, size);
                assert(nbytes <= size);

                if (nbytes == 0)
                    return state == CLOSED ? 0 : position;
            }

            remaining_chunk -= nbytes;
            if (remaining_chunk == 0)
                state = AFTER_DATA;

            position += nbytes;
            break;

        case AFTER_DATA:
            if (data[position] == '\n') {
                state = NONE;
            } else if (data[position] != '\r') {
                GError *error =
                    g_error_new_literal(dechunk_quark(), 0,
                                        "newline expected");
                Abort(error);
                return 0;
            }
            ++position;
            break;

        case TRAILER:
            if (data[position] == '\n') {
                ++position;

                if (verbatim) {
                    /* in "verbatim" mode, we need to send all data
                       before handling the EOF chunk */
                    had_output = true;
                    nbytes = InvokeData(data, position);
                    if (state == CLOSED)
                        return 0;

                    pending_verbatim = position - nbytes;
                    if (pending_verbatim > 0) {
                        /* not everything could be sent; postpone to
                           next call */
                        eof_verbatim = true;
                        return nbytes;
                    }
                }

                return EofDetected() ? position : 0;
            } else if (data[position] == '\r') {
                ++position;
            } else {
                ++position;
                state = TRAILER_DATA;
            }
            break;

        case TRAILER_DATA:
            if (data[position++] == '\n')
                state = TRAILER;
            break;

        case EOF_DETECTED:
            assert(0);
            return 0;
        }
    }

    if (verbatim && position > 0) {
        /* send all chunks in one big block */
        had_output = true;
        nbytes = InvokeData(data, position);
        if (state == CLOSED)
            return 0;

        /* postpone the rest that was not handled; it will not be
           parsed again */
        pending_verbatim = position - nbytes;
        return nbytes;
    }

    return position;
}


/*
 * istream handler
 *
 */

size_t
DechunkIstream::OnData(const void *data, size_t length)
{
    assert(!verbatim || length >= pending_verbatim);

    if (verbatim && eof_verbatim) {
        /* during the last call, the EOF chunk was parsed, but we
           could not handle it yet, because the handler did not
           consume all data yet; try to send the remaining pre-EOF
           data again and then handle the EOF chunk */

        assert(pending_verbatim > 0);

        assert(length >= pending_verbatim);

        had_output = true;
        size_t nbytes = InvokeData(data, pending_verbatim);
        if (nbytes == 0)
            return 0;

        pending_verbatim -= nbytes;
        if (pending_verbatim > 0)
            /* more data needed */
            return nbytes;

        return EofDetected() ? nbytes : 0;
    }

    const ScopePoolRef ref(GetPool() TRACE_ARGS);
    return Feed(data, length);
}

void
DechunkIstream::OnEof()
{
    assert(state != EOF_DETECTED && state != CLOSED);

    state = CLOSED;

    input.Clear();

    GError *error =
        g_error_new_literal(dechunk_quark(), 0,
                            "premature EOF in dechunker");
    DestroyError(error);
}

void
DechunkIstream::OnError(GError *error)
{
    input.Clear();

    if (state != EOF_DETECTED)
        DestroyError(error);
    else
        g_error_free(error);

    state = CLOSED;
}

/*
 * istream implementation
 *
 */

off_t
DechunkIstream::GetAvailable(bool partial)
{
    if (partial && state == DATA)
        return (off_t)remaining_chunk;

    return (off_t)-1;
}

void
DechunkIstream::Read()
{
    const ScopePoolRef ref(GetPool() TRACE_ARGS);

    had_output = false;

    do {
        had_input = false;
        input.Read();
    } while (input.IsDefined() && had_input && !had_output);
}

void
DechunkIstream::Close()
{
    assert(state != EOF_DETECTED);

    state = CLOSED;

    input.ClearHandlerAndClose();
    Destroy();
}

/*
 * constructor
 *
 */

struct istream *
istream_dechunk_new(struct pool *pool, struct istream *input,
                    void (*eof_callback)(void *ctx), void *callback_ctx)
{
    assert(input != nullptr);
    assert(!istream_has_handler(input));
    assert(eof_callback != nullptr);

    return NewIstream<DechunkIstream>(*pool, *input,
                                      eof_callback, callback_ctx);
}

bool
istream_dechunk_check_verbatim(struct istream *i)
{
    auto *dechunk = DechunkIstream::CheckCast(i);
    if (dechunk == nullptr)
        /* not a DechunkIstream instance */
        return false;

    dechunk->SetVerbatim();
    return true;
}
