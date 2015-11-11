/*
 * An istream handler which sends data to a socket.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "sink_fd.hxx"
#include "pool.hxx"
#include "pevent.hxx"
#include "direct.hxx"
#include "system/fd-util.h"
#include "istream_pointer.hxx"

#include <event.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

struct SinkFd {
    struct pool *pool;

    IstreamPointer input;

    int fd;
    FdType fd_type;
    const SinkFdHandler *handler;
    void *handler_ctx;

    struct event event;

    /**
     * Set to true each time data was received from the istream.
     */
    bool got_data;

    /**
     * This flag is used to determine if the EV_WRITE event shall be
     * scheduled after a splice().  We need to add the event only if
     * the splice() was triggered by EV_WRITE, because then we're
     * responsible for querying more data.
     */
    bool got_event;

#ifndef NDEBUG
    bool valid;
#endif

    void ScheduleWrite() {
        assert(fd >= 0);
        assert(input.IsDefined());

        got_event = false;

        p_event_add(&event, nullptr, pool, "sink_fd");
    }

    /* request istream handler */
    size_t OnData(const void *data, size_t length);
    ssize_t OnDirect(FdType type, int fd, size_t max_length);
    void OnEof();
    void OnError(GError *error);
};

/*
 * istream handler
 *
 */

inline size_t
SinkFd::OnData(const void *data, size_t length)
{
    got_data = true;

    ssize_t nbytes = IsAnySocket(fd_type)
        ? send(fd, data, length, MSG_DONTWAIT|MSG_NOSIGNAL)
        : write(fd, data, length);
    if (nbytes >= 0) {
        ScheduleWrite();
        return nbytes;
    } else if (errno == EAGAIN) {
        ScheduleWrite();
        return 0;
    } else {
        p_event_del(&event, pool);
        if (handler->send_error(errno, handler_ctx))
            input.Close();
        return 0;
    }
}

inline ssize_t
SinkFd::OnDirect(FdType type, gcc_unused int _fd, size_t max_length)
{
    got_data = true;

    ssize_t nbytes = istream_direct_to(fd, type, fd, fd_type,
                                       max_length);
    if (unlikely(nbytes < 0 && errno == EAGAIN)) {
        if (!fd_ready_for_writing(fd)) {
            ScheduleWrite();
            return ISTREAM_RESULT_BLOCKING;
        }

        /* try again, just in case connection->fd has become ready
           between the first istream_direct_to_socket() call and
           fd_ready_for_writing() */
        nbytes = istream_direct_to(fd, type, fd, fd_type, max_length);
    }

    if (likely(nbytes > 0) && (got_event || type == FdType::FD_FILE))
        /* regular files don't have support for EV_READ, and thus the
           sink is responsible for triggering the next splice */
        ScheduleWrite();

    return nbytes;
}

inline void
SinkFd::OnEof()
{
    got_data = true;

#ifndef NDEBUG
    valid = false;
#endif

    p_event_del(&event, pool);

    handler->input_eof(handler_ctx);
}

inline void
SinkFd::OnError(GError *error)
{
    got_data = true;

#ifndef NDEBUG
    valid = false;
#endif

    p_event_del(&event, pool);

    handler->input_error(error, handler_ctx);
}

/*
 * libevent callback
 *
 */

static void
socket_event_callback(gcc_unused int fd, gcc_unused short event,
                      void *ctx)
{
    SinkFd *ss = (SinkFd *)ctx;

    assert(fd == ss->fd);

    pool_ref(ss->pool);

    ss->got_event = true;
    ss->got_data = false;
    ss->input.Read();

    if (!ss->got_data)
        /* the fd is ready for writing, but the istream is blocking -
           don't try again for now */
        p_event_del(&ss->event, ss->pool);

    pool_unref(ss->pool);
    pool_commit();
}

/*
 * constructor
 *
 */

SinkFd *
sink_fd_new(struct pool &pool, Istream &istream,
            int fd, FdType fd_type,
            const SinkFdHandler &handler, void *ctx)
{
    assert(fd >= 0);
    assert(handler.input_eof != nullptr);
    assert(handler.input_error != nullptr);
    assert(handler.send_error != nullptr);

    auto ss = NewFromPool<SinkFd>(pool);
    ss->pool = &pool;

    ss->input.Set(istream,
                  MakeIstreamHandler<SinkFd>::handler, ss,
                  istream_direct_mask_to(fd_type));

    ss->fd = fd;
    ss->fd_type = fd_type;
    ss->handler = &handler;
    ss->handler_ctx = ctx;

    event_set(&ss->event, fd, EV_WRITE|EV_PERSIST, socket_event_callback, ss);
    ss->ScheduleWrite();

    ss->got_event = false;

#ifndef NDEBUG
    ss->valid = true;
#endif

    return ss;
}

void
sink_fd_read(SinkFd *ss)
{
    assert(ss != nullptr);
    assert(ss->valid);
    assert(ss->input.IsDefined());

    ss->input.Read();
}

void
sink_fd_close(SinkFd *ss)
{
    assert(ss != nullptr);
    assert(ss->valid);
    assert(ss->input.IsDefined());

#ifndef NDEBUG
    ss->valid = false;
#endif

    p_event_del(&ss->event, ss->pool);
    ss->input.Close();
}
