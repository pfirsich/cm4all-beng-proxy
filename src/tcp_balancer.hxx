/*
 * Wrapper for the tcp_stock class to support load balancing.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_TCP_BALANCER_HXX
#define BENG_PROXY_TCP_BALANCER_HXX

#include <inline/compiler.h>

struct hstock;
struct pool;
struct balancer;
struct AddressList;
struct StockGetHandler;
struct StockItem;
struct async_operation_ref;
class SocketAddress;

struct tcp_balancer;

/**
 * Creates a new TCP connection stock.
 *
 * @param pool the memory pool
 * @param tcp_stock the underlying tcp_stock object
 * @param balancer the load balancer object
 * @param limit the maximum number of connections per host
 * @return the new TCP connections stock (this function cannot fail)
 */
struct tcp_balancer *
tcp_balancer_new(struct pool *pool, struct hstock *tcp_stock,
                 struct balancer *balancer);

/**
 * @param session_sticky a portion of the session id that is used to
 * select the worker; 0 means disable stickiness
 * @param timeout the connect timeout for each attempt [seconds]
 */
void
tcp_balancer_get(struct tcp_balancer *tcp_balancer, struct pool *pool,
                 bool ip_transparent,
                 SocketAddress bind_address,
                 unsigned session_sticky,
                 const AddressList *address_list,
                 unsigned timeout,
                 const StockGetHandler *handler, void *handler_ctx,
                 struct async_operation_ref *async_ref);

void
tcp_balancer_put(struct tcp_balancer *tcp_balancer, StockItem &item,
                 bool destroy);

/**
 * Returns the address of the last connection that was established
 * successfully.  This is a dirty hack to allow the #tcp_stock's
 * #stock_handler to find this out.
 */
gcc_pure
SocketAddress
tcp_balancer_get_last();

#endif
