/*
 * Send HTTP requests to a widget server.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "widget-http.h"
#include "processor.h"
#include "widget.h"
#include "session.h"
#include "cookie-client.h"
#include "async.h"
#include "http-util.h"
#include "strref2.h"
#include "strref-pool.h"
#include "dpool.h"
#include "get.h"
#include "filter.h"
#include "header-writer.h"
#include "transformation.h"
#include "global.h"

#include <daemon/log.h>

#include <assert.h>
#include <string.h>

struct embed {
    pool_t pool;

    unsigned num_redirects;

    struct widget *widget;
    struct processor_env *env;
    const char *host_and_port;

    /**
     * the next transformation to be applied to the widget response
     */
    const struct transformation *transformation;

    /**
     * is this widget standalone, i.e. not embedded in another
     * container?
     */
    bool standalone;

    struct http_response_handler_ref handler_ref;
    struct async_operation_ref *async_ref;
};

static const char *const copy_headers[] = {
    "accept",
    "from",
    "cache-control",
    NULL,
};

static const char *const language_headers[] = {
    "accept-language",
    NULL,
};

static const char *const copy_headers_with_body[] = {
    "content-encoding",
    "content-language",
    "content-md5",
    "content-range",
    "content-type",
    NULL,
};

static const char *
uri_host_and_port(pool_t pool, const char *uri)
{
    const char *slash;

    if (memcmp(uri, "http://", 7) != 0)
        return NULL;

    uri += 7;
    slash = strchr(uri, '/');
    if (slash == NULL)
        return uri;

    return p_strndup(pool, uri, slash - uri);
}

static const char *
get_env_request_header(const struct processor_env *env, const char *key)
{
    assert(env != NULL);

    if (env->request_headers == NULL)
        return NULL;

    return strmap_get(env->request_headers, key);
}

static void
headers_copy2(struct strmap *in, struct strmap *out,
              const char *const* keys)
{
    const char *value;

    for (; *keys != NULL; ++keys) {
        value = strmap_get(in, *keys);
        if (value != NULL)
            strmap_set(out, *keys, value);
    }
}

static const char *
uri_path(const char *uri)
{
    const char *p = strchr(uri, ':');
    if (p == NULL || p[1] != '/')
        return uri;
    if (p[2] != '/')
        return p + 1;
    p = strchr(p + 3, '/');
    if (p == NULL)
        return "";
    return p;
}

static struct strmap *
widget_request_headers(struct embed *embed, int with_body)
{
    struct strmap *headers;
    struct session *session;
    const char *p;

    headers = strmap_new(embed->pool, 32);
    strmap_add(headers, "accept-charset", "utf-8");

    if (embed->env->request_headers != NULL) {
        headers_copy2(embed->env->request_headers, headers, copy_headers);
        if (with_body)
            headers_copy2(embed->env->request_headers, headers, copy_headers_with_body);
    }

    session = session_get(embed->env->session_id);

    if (embed->host_and_port != NULL && session != NULL) {
        const char *path = uri_path(widget_address(embed->pool,
                                                   embed->widget)->u.http->uri);

        lock_lock(&session->lock);
        cookie_jar_http_header(session->cookies, embed->host_and_port, path,
                               headers, embed->pool);
        lock_unlock(&session->lock);
    }

    if (session != NULL && session->language != NULL)
        strmap_add(headers, "accept-language", session->language);
    else if (embed->env->request_headers != NULL)
        headers_copy2(embed->env->request_headers, headers, language_headers);

    if (session != NULL && session->user != NULL)
        strmap_add(headers, "x-cm4all-beng-user", session->user);

    p = get_env_request_header(embed->env, "user-agent");
    if (p == NULL)
        p = "beng-proxy v" VERSION;
    strmap_add(headers, "user-agent", p);

    p = get_env_request_header(embed->env, "x-forwarded-for");
    if (p == NULL) {
        if (embed->env->remote_host != NULL)
            strmap_add(headers, "x-forwarded-for", embed->env->remote_host);
    } else {
        if (embed->env->remote_host == NULL)
            strmap_add(headers, "x-forwarded-for", p);
        else
            strmap_add(headers, "x-forwarded-for",
                       p_strcat(embed->pool, p, ", ",
                                embed->env->remote_host, NULL));
    }

    if (embed->widget->headers != NULL) {
        /* copy HTTP request headers from template */
        const struct strmap_pair *pair;

        strmap_rewind(embed->widget->headers);

        while ((pair = strmap_next(embed->widget->headers)) != NULL)
            strmap_add(headers,
                       p_strdup(embed->pool, pair->key),
                       p_strdup(embed->pool, pair->value));
    }

    return headers;
}

static const struct http_response_handler widget_response_handler;

static bool
widget_response_redirect(struct embed *embed, const char *location,
                         istream_t body)
{
    struct resource_address address_buffer;
    const struct resource_address *address;
    struct session *session;
    struct uri_with_address *uwa;
    struct strmap *headers;
    struct strref strref_buffer;
    const struct strref *p;

    if (embed->num_redirects >= 8)
        return false;

    if (embed->widget->class->address.type != RESOURCE_ADDRESS_HTTP)
        /* a static or CGI widget cannot send redirects */
        return false;

    address = resource_address_apply(embed->pool,
                                     widget_address(embed->pool, embed->widget),
                                     location, strlen(location),
                                     &address_buffer);
    if (address == NULL)
        return false;

    p = resource_address_relative(&embed->widget->class->address, address,
                                  &strref_buffer);
    if (p == NULL)
        return false;

    session = embed->widget->class->stateful
        ? session_get(embed->env->session_id)
        : NULL;
    widget_copy_from_location(embed->widget, session,
                              p->data, p->length, embed->pool);

    ++embed->num_redirects;

    if (body != NULL)
        istream_close(body);

    uwa = uri_address_dup(embed->pool, embed->widget->class->address.u.http);
    uwa->uri = location;

    headers = widget_request_headers(embed, 0);

    resource_get(global_http_cache, global_tcp_stock, global_fcgi_stock,
                 embed->pool,
                 HTTP_METHOD_GET, address, headers, NULL,
                 &widget_response_handler, embed,
                 embed->async_ref);

    return true;
}

/**
 * Ensure that a widget has the correct type for embedding it into a
 * HTML/XML document.  Returns NULL (and closes body) if that is
 * impossible.
 */
static istream_t
widget_response_format(pool_t pool, const struct widget *widget,
                       struct strmap **headers_r, istream_t body)
{
    struct strmap *headers = *headers_r;
    const char *content_type;
    struct strref *charset, charset_buffer;

    assert(body != NULL);

    content_type = headers == NULL
        ? NULL : strmap_get(headers, "content-type");

    if (content_type == NULL || strncmp(content_type, "text/", 5) != 0) {
        daemon_log(2, "widget '%s' sent non-text response\n",
                   widget_path(widget));
        istream_close(body);
        return NULL;
    }

    charset = http_header_param(&charset_buffer, content_type, "charset");
    if (charset != NULL && strref_lower_cmp_literal(charset, "utf-8") != 0 &&
        strref_lower_cmp_literal(charset, "utf8") != 0) {
        /* beng-proxy expects all widgets to send their HTML code in
           utf-8; this widget however used a different charset.
           Automatically convert it with istream_iconv */
        const char *charset2 = strref_dup(pool, charset);
        istream_t ic = istream_iconv_new(pool, body, "utf-8", charset2);
        if (ic == NULL) {
            daemon_log(2, "widget '%s' sent unknown charset '%s'\n",
                       widget_path(widget), charset2);
            istream_close(body);
            return NULL;
        }

        daemon_log(6, "widget '%s': charset conversion '%s' -> utf-8\n",
                   widget_path(widget), charset2);
        body = ic;

        headers = strmap_dup(pool, headers);
        strmap_set(headers, "content-type", "text/html; charset=utf-8");
    }

    if (strncmp(content_type + 5, "html", 4) != 0 &&
        strncmp(content_type + 5, "xml", 3) != 0) {
        /* convert text to HTML */

        daemon_log(6, "widget '%s': converting text to HTML\n",
                   widget_path(widget));

        body = istream_html_escape_new(pool, body);
        body = istream_cat_new(pool,
                               istream_string_new(pool,
                                                  "<pre class=\"beng_text_widget\">"),
                               body,
                               istream_string_new(pool, "</pre>"),
                               NULL);
    }

    *headers_r = headers;
    return body;
}

/**
 * The widget response is going to be embedded into a template; check
 * its content type and run the processor (if applicable).
 */
static void
widget_response_process(struct embed *embed,
                        struct strmap *headers, istream_t body,
                        unsigned options)
{
    processor_new(embed->pool, headers, body,
                  embed->widget, embed->env, options,
                  &widget_response_handler, embed,
                  embed->async_ref);
}

/**
 * Apply a transformation to the widget response and hand it back to
 * widget_response_handler.
 */
static void
widget_response_transform(struct embed *embed,
                          struct strmap *headers, istream_t body,
                          const struct transformation *transformation)
{
    assert(body != NULL);
    assert(transformation != NULL);
    assert(embed->transformation == transformation->next);

    switch (transformation->type) {
    case TRANSFORMATION_PROCESS:
        widget_response_process(embed, headers, body,
                                transformation->u.processor.options);
        break;

    case TRANSFORMATION_FILTER:
        filter_new(global_http_cache, global_tcp_stock, global_fcgi_stock,
                   embed->pool,
                   &transformation->u.filter,
                   headers != NULL ? headers_dup(embed->pool, headers) : NULL,
                   body,
                   &widget_response_handler, embed,
                   embed->async_ref);
        break;
    }
}

/**
 * A response was received from the widget server; apply
 * transformations (if enabled) and return it to our handler.  This
 * function will be called (semi-)recursively for every transformation
 * in the chain.
 */
static void
widget_response_dispatch(struct embed *embed, http_status_t status,
                         struct strmap *headers, istream_t body)
{
    const struct transformation *transformation = embed->transformation;

    if (transformation != NULL && body != NULL) {
        /* transform this response */

        embed->transformation = transformation->next;

        widget_response_transform(embed, headers, body, transformation);
    } else {
        /* no transformation left */

        if (body != NULL && !embed->widget->from_request.raw &&
            !embed->standalone) {
            /* check if the content-type is correct for embedding into
               a template, and convert if possible */
            body = widget_response_format(embed->pool, embed->widget,
                                          &headers, body);
            if (body == NULL) {
                http_response_handler_invoke_abort(&embed->handler_ref);
                return;
            }
        }

        /* finally pass the response to our handler */
        http_response_handler_invoke_response(&embed->handler_ref,
                                              status, headers, body);
    }
}

static void
widget_response_response(http_status_t status, struct strmap *headers,
                         istream_t body, void *ctx)
{
    struct embed *embed = ctx;
    /*const char *translate;*/

    if (headers != NULL) {
        if (embed->host_and_port != NULL) {
            const char *cookies = strmap_get(headers, "set-cookie2");
            if (cookies == NULL)
                cookies = strmap_get(headers, "set-cookie");
            if (cookies != NULL) {
                struct session *session = session_get(embed->env->session_id);
                if (session != NULL) {
                    lock_lock(&session->lock);
                    cookie_jar_set_cookie2(session->cookies, cookies,
                                           embed->host_and_port);
                    lock_unlock(&session->lock);
                }
            }
        }

        /*
        translate = strmap_get(headers, "x-cm4all-beng-translate");
        if (translate != NULL) {
            struct session *session = session_get(embed->env->session_id);
            if (session != NULL)
                session->translate = d_strdup(session->pool, translate);
        }
        */

        if (http_status_is_redirect(status)) {
            const char *location = strmap_get(headers, "location");
            if (location != NULL &&
                widget_response_redirect(embed, location, body)) {
                return;
            }
        }
    }

    widget_response_dispatch(embed, status, headers, body);
}

static void
widget_response_abort(void *ctx)
{
    struct embed *embed = ctx;

    http_response_handler_invoke_abort(&embed->handler_ref);
}

static const struct http_response_handler widget_response_handler = {
    .response = widget_response_response,
    .abort = widget_response_abort,
};


/*
 * constructor
 *
 */

void
widget_http_request(pool_t pool, struct widget *widget,
                    struct processor_env *env,
                    const struct http_response_handler *handler,
                    void *handler_ctx,
                    struct async_operation_ref *async_ref)
{
    const struct transformation_view *view;
    struct embed *embed;
    struct strmap *headers;

    assert(widget != NULL);
    assert(widget->class != NULL);

    view = transformation_view_lookup(widget->class->views,
                                      widget_get_view_name(widget));
    if (view == NULL) {
        daemon_log(3, "unknown view name for class '%s': '%s'\n",
                   widget->class_name, widget_get_view_name(widget));
        http_response_handler_direct_abort(handler, handler_ctx);
        return;
    }

    embed = p_malloc(pool, sizeof(*embed));
    embed->pool = pool;

    embed->num_redirects = 0;
    embed->widget = widget;
    embed->env = env;
    embed->host_and_port =
        embed->widget->class->address.type == RESOURCE_ADDRESS_HTTP
        ? uri_host_and_port(pool, embed->widget->class->address.u.http->uri)
        : NULL;
    embed->transformation = embed->widget->from_request.raw
        ? NULL : view->transformation;
    embed->standalone = embed->widget->from_request.proxy ||
        embed->widget->from_request.proxy_ref != NULL;

    headers = widget_request_headers(embed, widget->from_request.body != NULL);

    http_response_handler_set(&embed->handler_ref, handler, handler_ctx);
    embed->async_ref = async_ref;

    resource_get(global_http_cache, global_tcp_stock, global_fcgi_stock,
                 pool,
                 widget->from_request.method,
                 widget_address(pool, widget),
                 headers,
                 widget->from_request.body,
                 &widget_response_handler, embed, async_ref);
}
