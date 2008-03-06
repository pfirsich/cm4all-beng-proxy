/*
 * Manage connections to HTTP clients.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_CONNECTION_H
#define __BENG_CONNECTION_H

#include "http-server.h"

#include <inline/list.h>

#include <sys/socket.h>

struct config;

struct client_connection {
    struct list_head siblings;
    struct instance *instance;
    pool_t pool;
    const struct config *config;
    http_server_connection_t http;
};

void
remove_connection(struct client_connection *connection);

void
close_connection(struct client_connection *connection);

void
http_listener_callback(int fd,
                       const struct sockaddr *addr, socklen_t addrlen,
                       void *ctx);

#endif
