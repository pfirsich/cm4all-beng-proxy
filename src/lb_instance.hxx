/*
 * Global declarations.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_LB_INSTANCE_H
#define BENG_PROXY_LB_INSTANCE_H

#include "lb_cmdline.hxx"
#include "lb_connection.hxx"
#include "event/Base.hxx"
#include "event/TimerEvent.hxx"
#include "event/SignalEvent.hxx"
#include "event/ShutdownListener.hxx"

#include <forward_list>
#include <map>

class Stock;
struct StockMap;
struct TcpBalancer;
struct LbConfig;
struct LbCertDatabaseConfig;
struct LbControl;
class lb_listener;
class CertCache;

struct lb_instance {
    struct pool *pool;

    struct lb_cmdline cmdline;

    LbConfig *config;

    EventBase event_base;

    uint64_t http_request_counter = 0;

    std::forward_list<LbControl> controls;

    std::forward_list<lb_listener> listeners;

    std::map<std::string, CertCache> cert_dbs;

    TimerEvent launch_worker_event;

    boost::intrusive::list<LbConnection,
                           boost::intrusive::constant_time_size<true>> connections;

    /**
     * Number of #lb_tcp instances.
     */
    unsigned n_tcp_connections = 0;

    bool should_exit = false;
    ShutdownListener shutdown_listener;
    SignalEvent sighup_event;

    /* stock */
    struct balancer *balancer;
    StockMap *tcp_stock;
    TcpBalancer *tcp_balancer;

    Stock *pipe_stock;

    lb_instance();
    ~lb_instance();

    CertCache &GetCertCache(const LbCertDatabaseConfig &cert_db_config);
    void DisconnectCertCaches();

    unsigned FlushSSLSessionCache(long tm);

    static void ShutdownCallback(void *ctx);
};

struct client_connection;

void
init_signals(struct lb_instance *instance);

void
deinit_signals(struct lb_instance *instance);

#endif
