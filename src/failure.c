/*
 * Remember which servers (socket addresses) failed recently.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "failure.h"
#include "expiry.h"
#include "address-envelope.h"

#include <daemon/log.h>

#include <assert.h>
#include <string.h>
#include <time.h>
#include <errno.h>

struct failure {
    struct failure *next;

    time_t expires;

    enum failure_status status;

    struct address_envelope envelope;
};

#define FAILURE_SLOTS 64

struct failure_list {
    pool_t pool;

    struct failure *slots[FAILURE_SLOTS];
};

static struct failure_list fl;

static inline unsigned
calc_hash(const struct sockaddr *addr, size_t addrlen)
{
    const char *p = (const char*)addr;
    unsigned hash = 5381;

    assert(p != NULL);

    while (addrlen-- > 0)
        hash = (hash << 5) + hash + *p++;

    return hash;
}

void
failure_init(pool_t pool)
{
    fl.pool = pool_new_libc(pool, "failure_list");
    memset(fl.slots, 0, sizeof(fl.slots));
}

void
failure_deinit(void)
{
    pool_unref(fl.pool);
}

void
failure_set(const struct sockaddr *addr, size_t addrlen,
            enum failure_status status)
{
    unsigned slot = calc_hash(addr, addrlen) % FAILURE_SLOTS;
    struct failure *failure;
    struct timespec now;
    int ret;

    assert(addr != NULL);
    assert(addrlen >= sizeof(failure->envelope.address));
    assert(status > FAILURE_OK);

    ret = clock_gettime(CLOCK_MONOTONIC, &now);
    if (ret < 0) {
        daemon_log(1, "clock_gettime(CLOCK_MONOTONIC) failed: %s\n",
                   strerror(errno));
        return;
    }

    for (failure = fl.slots[slot]; failure != NULL; failure = failure->next) {
        if (failure->envelope.length == addrlen &&
            memcmp(&failure->envelope.address, addr, addrlen) == 0) {
            /* this address is already in our list */
            if (status < failure->status && !is_expired(failure->expires))
                /* don't update if the current status is more serious
                   than the new one */
                return;

            failure->expires = now.tv_sec + 20;
            failure->status = status;
            return;
        }
    }

    /* insert new failure object into the linked list */

    failure = p_malloc(fl.pool, sizeof(*failure)
                       - sizeof(failure->envelope.address) + addrlen);
    failure->expires = now.tv_sec + 20;
    failure->status = status;
    failure->envelope.length = addrlen;
    memcpy(&failure->envelope.address, addr, addrlen);

    failure->next = fl.slots[slot];
    fl.slots[slot] = failure;
}

void
failure_remove(const struct sockaddr *addr, size_t addrlen)
{
    unsigned slot = calc_hash(addr, addrlen) % FAILURE_SLOTS;
    struct failure **failure_r, *failure;

    assert(addr != NULL);
    assert(addrlen >= sizeof(failure->envelope.address));

    for (failure_r = &fl.slots[slot], failure = *failure_r;
         failure != NULL;
         failure_r = &failure->next, failure = *failure_r) {
        if (failure->envelope.length == addrlen &&
            memcmp(&failure->envelope.address, addr, addrlen) == 0) {
            /* found it: remove it */

            *failure_r = failure->next;
            p_free(fl.pool, failure);
            return;
        }
    }
}

enum failure_status
failure_get_status(const struct sockaddr *address, size_t length)
{
    unsigned slot = calc_hash(address, length) % FAILURE_SLOTS;
    struct failure *failure;

    assert(address != NULL);
    assert(length >= sizeof(failure->envelope.address));

    for (failure = fl.slots[slot]; failure != NULL; failure = failure->next)
        if (failure->envelope.length == length &&
            memcmp(&failure->envelope.address, address, length) == 0)
            return !is_expired(failure->expires)
                ? failure->status
                : FAILURE_OK;

    return FAILURE_OK;
}
