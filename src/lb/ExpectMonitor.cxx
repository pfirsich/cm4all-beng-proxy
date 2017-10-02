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

#include "ExpectMonitor.hxx"
#include "Monitor.hxx"
#include "MonitorConfig.hxx"
#include "pool.hxx"
#include "system/Error.hxx"
#include "net/PConnectSocket.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "net/SocketAddress.hxx"
#include "event/SocketEvent.hxx"
#include "event/TimerEvent.hxx"
#include "event/Duration.hxx"
#include "util/Cancellable.hxx"

#include <unistd.h>
#include <sys/socket.h>
#include <string.h>
#include <errno.h>

struct ExpectMonitor final : ConnectSocketHandler, Cancellable {
    struct pool &pool;
    const LbMonitorConfig &config;

    int fd = -1;

    SocketEvent event;

    /**
     * A timer which is used to delay the recv() call, just in case
     * the server sends the response in more than one packet.
     */
    TimerEvent delay_event;

    LbMonitorHandler &handler;

    CancellablePointer &cancel_ptr;

    ExpectMonitor(EventLoop &event_loop,
                  struct pool &_pool, const LbMonitorConfig &_config,
                  LbMonitorHandler &_handler,
                  CancellablePointer &_cancel_ptr)
        :pool(_pool), config(_config),
         event(event_loop, BIND_THIS_METHOD(EventCallback)),
         delay_event(event_loop, BIND_THIS_METHOD(DelayCallback)),
         handler(_handler),
         cancel_ptr(_cancel_ptr) {}

    ExpectMonitor(const ExpectMonitor &other) = delete;

    /* virtual methods from class Cancellable */
    void Cancel() override;

    /* virtual methods from class ConnectSocketHandler */
    void OnSocketConnectSuccess(UniqueSocketDescriptor &&fd) override;

    void OnSocketConnectTimeout() override {
        handler.Timeout();
        delete this;
    }

    void OnSocketConnectError(std::exception_ptr ep) override {
        handler.Error(ep);
        delete this;
    }

private:
    void EventCallback(unsigned events);
    void DelayCallback();
};

static bool
check_expectation(char *received, size_t received_length,
                  const char *expect)
{
    return memmem(received, received_length, expect, strlen(expect)) != nullptr;
}

/*
 * async operation
 *
 */

void
ExpectMonitor::Cancel()
{
    event.Delete();
    delay_event.Cancel();
    close(fd);
    pool_unref(&pool);
    delete this;
}

/*
 * libevent callback
 *
 */

inline void
ExpectMonitor::EventCallback(unsigned events)
{
    if (events & SocketEvent::TIMEOUT) {
        close(fd);
        handler.Timeout();
    } else {
        /* wait 10ms before we start reading */
        delay_event.Add(EventDuration<0, 10000>::value);
        return;
    }

    pool_unref(&pool);
    delete this;
}

void
ExpectMonitor::DelayCallback()
{
    char buffer[1024];

    ssize_t nbytes = recv(fd, buffer, sizeof(buffer),
                          MSG_DONTWAIT);
    if (nbytes < 0) {
        auto e = MakeErrno("Failed to receive");
        close(fd);
        handler.Error(std::make_exception_ptr(e));
    } else if (!config.fade_expect.empty() &&
               check_expectation(buffer, nbytes,
                                 config.fade_expect.c_str())) {
        close(fd);
        handler.Fade();
    } else if (config.expect.empty() ||
               check_expectation(buffer, nbytes,
                                 config.expect.c_str())) {
        close(fd);
        handler.Success();
    } else {
        close(fd);
        handler.Error(std::make_exception_ptr(std::runtime_error("Expectation failed")));
    }

    pool_unref(&pool);
    delete this;
}

/*
 * client_socket handler
 *
 */

void
ExpectMonitor::OnSocketConnectSuccess(UniqueSocketDescriptor &&new_fd)
{
    if (!config.send.empty()) {
        ssize_t nbytes = send(new_fd.Get(), config.send.data(),
                              config.send.length(),
                              MSG_DONTWAIT);
        if (nbytes < 0) {
            handler.Error(std::make_exception_ptr(MakeErrno("Failed to send")));
            delete this;
            return;
        }
    }

    struct timeval expect_timeout = {
        time_t(config.timeout > 0 ? config.timeout : 10),
        0,
    };

    fd = new_fd.Steal();
    event.Set(fd, SocketEvent::READ);
    event.Add(expect_timeout);

    cancel_ptr = *this;

    pool_ref(&pool);
}

/*
 * lb_monitor_class
 *
 */

static void
expect_monitor_run(EventLoop &event_loop, struct pool &pool,
                   const LbMonitorConfig &config,
                   SocketAddress address,
                   LbMonitorHandler &handler,
                   CancellablePointer &cancel_ptr)
{
    ExpectMonitor *expect = new ExpectMonitor(event_loop, pool, config,
                                              handler,
                                              cancel_ptr);

    const unsigned connect_timeout = config.connect_timeout > 0
        ? config.connect_timeout
        : (config.timeout > 0
           ? config.timeout
           : 30);

    client_socket_new(event_loop, pool, address.GetFamily(), SOCK_STREAM, 0,
                      false,
                      SocketAddress::Null(),
                      address,
                      connect_timeout,
                      *expect, cancel_ptr);
}

const LbMonitorClass expect_monitor_class = {
    .run = expect_monitor_run,
};
