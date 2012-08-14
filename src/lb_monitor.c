/*
 * Generic monitor class.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "lb_monitor.h"
#include "lb_config.h"
#include "async.h"
#include "pool.h"
#include "failure.h"

#include <daemon/log.h>

#include <event.h>

struct lb_monitor {
    struct pool *pool;

    const char *name;
    const struct lb_monitor_config *config;
    const struct sockaddr *address;
    size_t address_length;
    const struct lb_monitor_class *class;

    struct timeval interval;
    struct event timer_event;

    struct pool_mark mark;
    struct async_operation_ref async_ref;

    bool state;
    bool fade;
};

static void
monitor_handler_success(void *ctx)
{
    struct lb_monitor *monitor = ctx;
    async_ref_clear(&monitor->async_ref);

    if (!monitor->state)
        daemon_log(5, "monitor recovered: %s\n", monitor->name);
    else if (monitor->fade)
        daemon_log(5, "monitor finished fade: %s\n", monitor->name);
    else
        daemon_log(6, "monitor ok: %s\n", monitor->name);

    monitor->state = true;

    failure_unset(monitor->address, monitor->address_length,
                  FAILURE_MONITOR);

    if (monitor->fade) {
        monitor->fade = false;
        failure_unset(monitor->address, monitor->address_length,
                      FAILURE_FADE);
    }

    evtimer_add(&monitor->timer_event, &monitor->interval);
}

static void
monitor_handler_fade(void *ctx)
{
    struct lb_monitor *monitor = ctx;
    async_ref_clear(&monitor->async_ref);

    if (!monitor->fade)
        daemon_log(5, "monitor fade: %s\n", monitor->name);
    else
        daemon_log(6, "monitor still fade: %s\n", monitor->name);

    monitor->fade = true;
    failure_set(monitor->address, monitor->address_length,
                FAILURE_FADE, 300);

    evtimer_add(&monitor->timer_event, &monitor->interval);
}

static void
monitor_handler_timeout(void *ctx)
{
    struct lb_monitor *monitor = ctx;
    async_ref_clear(&monitor->async_ref);

    daemon_log(monitor->state ? 3 : 6,
               "monitor timeout: %s\n", monitor->name);

    monitor->state = false;
    failure_set(monitor->address, monitor->address_length,
                FAILURE_MONITOR, 0);

    evtimer_add(&monitor->timer_event, &monitor->interval);
}

static void
monitor_handler_error(GError *error, void *ctx)
{
    struct lb_monitor *monitor = ctx;
    async_ref_clear(&monitor->async_ref);

    if (monitor->state)
        daemon_log(2, "monitor error: %s: %s\n",
                   monitor->name, error->message);
    else
        daemon_log(4, "monitor error: %s: %s\n",
                   monitor->name, error->message);
    g_error_free(error);

    monitor->state = false;
    failure_set(monitor->address, monitor->address_length,
                FAILURE_MONITOR, 0);

    evtimer_add(&monitor->timer_event, &monitor->interval);
}

static const struct lb_monitor_handler monitor_handler = {
    .success = monitor_handler_success,
    .fade = monitor_handler_fade,
    .timeout = monitor_handler_timeout,
    .error = monitor_handler_error,
};

static void
lb_monitor_timer_callback(G_GNUC_UNUSED int fd, G_GNUC_UNUSED short event,
                          void *ctx)
{
    struct lb_monitor *monitor = ctx;
    daemon_log(6, "running monitor %s\n", monitor->name);

    struct pool *pool = pool_new_linear(monitor->pool, "monitor_run", 8192);
    monitor->class->run(pool, monitor->config,
                        monitor->address, monitor->address_length,
                        &monitor_handler, monitor,
                        &monitor->async_ref);
    pool_unref(pool);
}

struct lb_monitor *
lb_monitor_new(struct pool *pool, const char *name,
               const struct lb_monitor_config *config,
               const struct sockaddr *address, size_t address_length,
               const struct lb_monitor_class *class)
{
    pool_ref(pool);
    struct lb_monitor *monitor = p_malloc(pool, sizeof(*monitor));
    monitor->pool = pool;
    monitor->name = name;
    monitor->config = config;
    monitor->address = address;
    monitor->address_length = address_length;
    monitor->class = class;
    monitor->interval.tv_sec = config->interval;
    monitor->interval.tv_usec = 0;
    evtimer_set(&monitor->timer_event, lb_monitor_timer_callback, monitor);
    async_ref_clear(&monitor->async_ref);
    monitor->state = true;
    monitor->fade = false;

    static const struct timeval immediately = { .tv_sec = 0 };
    evtimer_add(&monitor->timer_event, &immediately);

    return monitor;
}

void
lb_monitor_free(struct lb_monitor *monitor)
{
    event_del(&monitor->timer_event);

    if (async_ref_defined(&monitor->async_ref))
        async_abort(&monitor->async_ref);

    pool_unref(monitor->pool);
}

bool
lb_monitor_get_state(const struct lb_monitor *monitor)
{
    return monitor->state;
}
