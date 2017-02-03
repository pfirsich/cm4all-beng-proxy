/*
 * Wrapper for a socket file descriptor.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "socket_wrapper.hxx"
#include "direct.hxx"
#include "system/fd-util.h"
#include "system/fd_util.h"
#include "io/Buffered.hxx"
#include "pool.hxx"

#include <socket/util.h>

#include <unistd.h>
#include <sys/socket.h>

void
SocketWrapper::ReadEventCallback(unsigned events)
{
    assert(IsValid());

    if (events & EV_TIMEOUT)
        handler->timeout(handler_ctx);
    else
        handler->read(handler_ctx);

    pool_commit();
}

void
SocketWrapper::WriteEventCallback(unsigned events)
{
    assert(IsValid());

    if (events & EV_TIMEOUT)
        handler->timeout(handler_ctx);
    else
        handler->write(handler_ctx);

    pool_commit();
}

void
SocketWrapper::Init(int _fd, FdType _fd_type,
                    const struct socket_handler &_handler, void *_ctx)
{
    assert(_fd >= 0);
    assert(_handler.read != nullptr);
    assert(_handler.write != nullptr);

    fd = _fd;
    fd_type = _fd_type;
    direct_mask = istream_direct_mask_to(fd_type);

    read_event.Set(fd, EV_READ|EV_PERSIST|EV_TIMEOUT);
    write_event.Set(fd, EV_WRITE|EV_PERSIST|EV_TIMEOUT);

    handler = &_handler;
    handler_ctx = _ctx;
}

void
SocketWrapper::Init(SocketWrapper &&src,
                    const struct socket_handler &_handler, void *_ctx)
{
    Init(src.fd, src.fd_type, _handler, _ctx);
    src.Abandon();
}

void
SocketWrapper::Shutdown()
{
    if (fd < 0)
        return;

    shutdown(fd, SHUT_RDWR);
}

void
SocketWrapper::Close()
{
    if (fd < 0)
        return;

    read_event.Delete();
    write_event.Delete();

    close(fd);
    fd = -1;
}

void
SocketWrapper::Abandon()
{
    assert(fd >= 0);

    read_event.Delete();
    write_event.Delete();

    fd = -1;
}

int
SocketWrapper::AsFD()
{
    assert(IsValid());

    const int result = dup_cloexec(fd);
    Abandon();
    return result;
}

ssize_t
SocketWrapper::ReadToBuffer(ForeignFifoBuffer<uint8_t> &buffer, size_t length)
{
    assert(IsValid());

    return recv_to_buffer(fd, buffer, length);
}

void
SocketWrapper::SetCork(bool cork)
{
    assert(IsValid());

    socket_set_cork(fd, cork);
}

bool
SocketWrapper::IsReadyForWriting() const
{
    assert(IsValid());

    return fd_ready_for_writing(fd);
}

ssize_t
SocketWrapper::Write(const void *data, size_t length)
{
    assert(IsValid());

    return send(fd, data, length, MSG_DONTWAIT|MSG_NOSIGNAL);
}

ssize_t
SocketWrapper::WriteV(const struct iovec *v, size_t n)
{
    assert(IsValid());

    struct msghdr m = {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = const_cast<struct iovec *>(v),
        .msg_iovlen = n,
        .msg_control = nullptr,
        .msg_controllen = 0,
        .msg_flags = 0,
    };

    return sendmsg(fd, &m, MSG_DONTWAIT|MSG_NOSIGNAL);
}

ssize_t
SocketWrapper::WriteFrom(int other_fd, FdType other_fd_type,
                         size_t length)
{
    return istream_direct_to_socket(other_fd_type, other_fd, fd, length);
}

