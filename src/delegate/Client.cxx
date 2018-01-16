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

#include "Client.hxx"
#include "Handler.hxx"
#include "Protocol.hxx"
#include "please.hxx"
#include "system/fd_util.h"
#include "pool/pool.hxx"
#include "event/SocketEvent.hxx"
#include "system/Error.hxx"
#include "util/Cancellable.hxx"
#include "util/Macros.hxx"

#include <stdexcept>

#include <assert.h>
#include <string.h>
#include <sys/socket.h>

struct DelegateClient final : Cancellable {
    struct lease_ref lease_ref;
    const int fd;
    SocketEvent event;

    struct pool &pool;

    DelegateHandler &handler;

    DelegateClient(EventLoop &event_loop, int _fd, Lease &lease,
                   struct pool &_pool,
                   DelegateHandler &_handler)
        :fd(_fd), event(event_loop, fd, SocketEvent::READ,
                        BIND_THIS_METHOD(SocketEventCallback)),
         pool(_pool),
         handler(_handler) {
        p_lease_ref_set(lease_ref, lease,
                        _pool, "delegate_client_lease");

        event.Add();
    }

    ~DelegateClient() {
        pool_unref(&pool);
    }

    void Destroy() {
        this->~DelegateClient();
    }

    void ReleaseSocket(bool reuse) {
        assert(fd >= 0);

        p_lease_release(lease_ref, reuse, pool);
    }

    void DestroyError(std::exception_ptr ep) {
        ReleaseSocket(false);
        handler.OnDelegateError(ep);
        Destroy();
    }

    void DestroyError(const char *msg) {
        DestroyError(std::make_exception_ptr(std::runtime_error(msg)));
    }

    void HandleFd(const struct msghdr &msg, size_t length);
    void HandleErrno(size_t length);
    void HandleMsg(const struct msghdr &msg,
                   DelegateResponseCommand command, size_t length);
    void TryRead();

private:
    void SocketEventCallback(unsigned) {
        TryRead();
    }

    /* virtual methods from class Cancellable */
    void Cancel() noexcept override {
        event.Delete();
        ReleaseSocket(false);
        Destroy();
    }
};

inline void
DelegateClient::HandleFd(const struct msghdr &msg, size_t length)
{
    if (length != 0) {
        DestroyError("Invalid message length");
        return;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg == nullptr) {
        DestroyError("No fd passed");
        return;
    }

    if (cmsg->cmsg_type != SCM_RIGHTS) {
        DestroyError("got control message of unknown type");
        return;
    }

    ReleaseSocket(true);

    const void *data = CMSG_DATA(cmsg);
    const int *fd_p = (const int *)data;

    int new_fd = *fd_p;
    handler.OnDelegateSuccess(new_fd);
    Destroy();
}

inline void
DelegateClient::HandleErrno(size_t length)
{
    int e;

    if (length != sizeof(e)) {
        DestroyError("Invalid message length");
        return;
    }

    ssize_t nbytes = recv(fd, &e, sizeof(e), 0);
    std::exception_ptr ep;

    if (nbytes == sizeof(e)) {
        ReleaseSocket(true);

        ep = std::make_exception_ptr(MakeErrno(e));
    } else {
        ReleaseSocket(false);

        ep = std::make_exception_ptr(std::runtime_error("Failed to receive errno"));
    }

    handler.OnDelegateError(ep);
    Destroy();
}

inline void
DelegateClient::HandleMsg(const struct msghdr &msg,
                          DelegateResponseCommand command, size_t length)
{
    switch (command) {
    case DelegateResponseCommand::FD:
        HandleFd(msg, length);
        return;

    case DelegateResponseCommand::ERRNO:
        /* i/o error */
        HandleErrno(length);
        return;
    }

    DestroyError("Invalid delegate response");
}

inline void
DelegateClient::TryRead()
{
    struct iovec iov;
    int new_fd;
    char ccmsg[CMSG_SPACE(sizeof(new_fd))];
    struct msghdr msg = {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ccmsg,
        .msg_controllen = sizeof(ccmsg),
    };
    DelegateResponseHeader header;
    ssize_t nbytes;

    iov.iov_base = &header;
    iov.iov_len = sizeof(header);

    nbytes = recvmsg_cloexec(fd, &msg, 0);
    if (nbytes < 0) {
        DestroyError(std::make_exception_ptr(MakeErrno("recvmsg() failed")));
        return;
    }

    if ((size_t)nbytes != sizeof(header)) {
        DestroyError("short recvmsg()");
        return;
    }

    HandleMsg(msg, header.command, header.length);
}

/*
 * constructor
 *
 */

static void
SendDelegatePacket(int fd, DelegateRequestCommand cmd,
                   const void *payload, size_t length)
{
    const DelegateRequestHeader header = {
        .length = (uint16_t)length,
        .command = cmd,
    };

    struct iovec v[] = {
        { const_cast<void *>((const void *)&header), sizeof(header) },
        { const_cast<void *>(payload), length },
    };

    struct msghdr msg = {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = v,
        .msg_iovlen = ARRAY_SIZE(v),
        .msg_control = nullptr,
        .msg_controllen = 0,
        .msg_flags = 0,
    };

    auto nbytes = sendmsg(fd, &msg, MSG_DONTWAIT);
    if (nbytes < 0)
        throw MakeErrno("Failed to send to delegate");

    if (size_t(nbytes) != sizeof(header) + length)
        throw std::runtime_error("Short send to delegate");
}

void
delegate_open(EventLoop &event_loop, int fd, Lease &lease,
              struct pool *pool, const char *path,
              DelegateHandler &handler,
              CancellablePointer &cancel_ptr)
{
    try {
        SendDelegatePacket(fd, DelegateRequestCommand::OPEN,
                           path, strlen(path));
    } catch (...) {
        lease.ReleaseLease(false);
        handler.OnDelegateError(std::current_exception());
        return;
    }

    auto d = NewFromPool<DelegateClient>(*pool, event_loop, fd, lease,
                                         *pool,
                                         handler);

    pool_ref(pool);

    cancel_ptr = *d;
}
