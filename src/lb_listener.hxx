/*
 * Listener on a TCP port.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_LB_LISTENER_HXX
#define BENG_PROXY_LB_LISTENER_HXX

#include "Logger.hxx"
#include "net/ServerSocket.hxx"

struct SslFactory;
struct LbListenerConfig;
struct LbInstance;

class LbListener final : Logger, public ServerSocket {
    LbInstance &instance;

    const LbListenerConfig &config;

    SslFactory *ssl_factory = nullptr;

public:
    LbListener(LbInstance &_instance,
               const LbListenerConfig &_config);
    ~LbListener();

    void Setup();

    unsigned FlushSSLSessionCache(long tm);

protected:
    void OnAccept(UniqueSocketDescriptor &&fd, SocketAddress address) override;
    void OnAcceptError(std::exception_ptr ep) override;

    /* virtual methods from class Logger */
    std::string MakeLogName() const noexcept override;
};

#endif
