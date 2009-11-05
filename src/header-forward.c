/*
 * Which headers should be forwarded to/from remote HTTP servers?
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "header-forward.h"
#include "header-writer.h"
#include "strmap.h"
#include "session.h"
#include "cookie-client.h"
#include "growing-buffer.h"

#ifndef NDEBUG
#include <daemon/log.h>
#endif

static const char *const basic_request_headers[] = {
    "accept",
    "from",
    "cache-control",
    NULL,
};

static const char *const language_request_headers[] = {
    "accept-language",
    NULL,
};

static const char *const body_request_headers[] = {
    "content-encoding",
    "content-language",
    "content-md5",
    "content-range",
    "content-type",
    "content-disposition",
    NULL,
};

static const char *const cookie_request_headers[] = {
    "cookie",
    "cookie2",
    NULL,
};

static const char *const response_headers[] = {
    "age",
    "etag",
    "cache-control",
    "content-encoding",
    "content-language",
    "content-md5",
    "content-range",
    "content-type",
    "content-disposition",
    "last-modified",
    "retry-after",
    "vary",
    "location",
    NULL,
};

static void
headers_copy2(const struct strmap *in, struct strmap *out,
              const char *const* keys)
{
    const char *value;

    for (; *keys != NULL; ++keys) {
        value = strmap_get(in, *keys);
        if (value != NULL)
            strmap_set(out, *keys, value);
    }
}

static void
forward_basic_headers(struct strmap *dest, const struct strmap *src,
                      bool with_body)
{
    headers_copy2(src, dest, basic_request_headers);
    if (with_body)
        headers_copy2(src, dest, body_request_headers);
}

static void
forward_user_agent(struct strmap *dest, const struct strmap *src,
                   bool mangle)
{
    const char *p;

    p = !mangle
        ? strmap_get_checked(src, "user-agent")
        : NULL;
    if (p == NULL)
        p = "beng-proxy v" VERSION;

    strmap_add(dest, "user-agent", p);
}

static void
forward_via(pool_t pool, struct strmap *dest, const struct strmap *src,
            const char *local_host, const char *remote_host,
            bool mangle)
{
    const char *p;

    p = strmap_get_checked(src, "via");
    if (p == NULL) {
        if (local_host != NULL && mangle)
            strmap_add(dest, "via",
                       p_strcat(pool, "1.1 ", local_host, NULL));
    } else {
        if (local_host == NULL || !mangle)
            strmap_add(dest, "via", p);
        else
            strmap_add(dest, "via",
                       p_strcat(pool, p, ", 1.1 ", local_host, NULL));
    }

    p = strmap_get_checked(src, "x-forwarded-for");
    if (p == NULL) {
        if (remote_host != NULL && mangle)
            strmap_add(dest, "x-forwarded-for", remote_host);
    } else {
        if (remote_host == NULL || !mangle)
            strmap_add(dest, "x-forwarded-for", p);
        else
            strmap_add(dest, "x-forwarded-for",
                       p_strcat(pool, p, ", ", remote_host, NULL));
    }
}

struct strmap *
forward_request_headers(pool_t pool, struct strmap *src,
                        const char *local_host, const char *remote_host,
                        bool with_body, bool forward_charset,
                        const struct header_forward_settings *settings,
                        const struct session *session,
                        const char *host_and_port, const char *uri)
{
    struct strmap *dest;
    const char *p;

    assert(settings != NULL);

#ifndef NDEBUG
    if (session != NULL && daemon_log_config.verbose >= 10)
        daemon_log(10, "forward_request_headers remote_host='%s' "
                   "host='%s' uri='%s' session=%x user='%s' cookie='%s'\n",
                   remote_host, host_and_port, uri,
                   session->id, session->user,
                   host_and_port != NULL && uri != NULL
                   ? cookie_jar_http_header_value(session->cookies,
                                                  host_and_port, uri, pool)
                   : NULL);
#endif

    dest = strmap_new(pool, 32);

    if (src != NULL)
        forward_basic_headers(dest, src, with_body);

    p = forward_charset
        ? strmap_get_checked(src, "accept-charset")
        : NULL;
    if (p == NULL)
        p = "utf-8";
    strmap_add(dest, "accept-charset", p);

    if (settings->cookie == HEADER_FORWARD_YES) {
        if (src != NULL)
            headers_copy2(src, dest, cookie_request_headers);
    } else if (settings->cookie == HEADER_FORWARD_MANGLE &&
               session != NULL && host_and_port != NULL && uri != NULL)
        cookie_jar_http_header(session->cookies, host_and_port, uri,
                               dest, pool);

    if (session != NULL && session->language != NULL)
        strmap_add(dest, "accept-language", p_strdup(pool, session->language));
    else if (src != NULL)
        headers_copy2(src, dest, language_request_headers);

    if (session != NULL && session->user != NULL)
        strmap_add(dest, "x-cm4all-beng-user", p_strdup(pool, session->user));

    if (settings->capabilities != HEADER_FORWARD_NO)
        forward_user_agent(dest, src,
                           settings->capabilities == HEADER_FORWARD_MANGLE);

    if (settings->identity != HEADER_FORWARD_NO)
        forward_via(pool, dest, src, local_host, remote_host,
                    settings->identity == HEADER_FORWARD_MANGLE);

    return dest;
}

struct growing_buffer *
forward_print_response_headers(pool_t pool, struct strmap *src)
{
    struct growing_buffer *dest;

    if (src == NULL) {
        dest = growing_buffer_new(pool, 1024);
    } else {
        dest = growing_buffer_new(pool, 2048);
        headers_copy(src, dest, response_headers);
    }

    return dest;
}
