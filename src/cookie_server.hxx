/*
 * Handle cookies sent by the HTTP client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_COOKIE_SERVER_HXX
#define BENG_PROXY_COOKIE_SERVER_HXX

#include "util/Compiler.h"

struct pool;
class StringMap;

/**
 * Parse a Cookie request header and store all cookies in the
 * specified strmap.
 */
gcc_pure
StringMap
cookie_map_parse(struct pool &pool, const char *p);

/**
 * Remove cookies with the specified name from a Cookie request
 * header.  Returns the input string if such a cookie was not found,
 * or a newly allocated string.  Returns nullptr when no cookies
 * remain after removing the excluded cookie.
 */
const char *
cookie_exclude(const char *p, const char *exclude, struct pool *pool);

#endif
