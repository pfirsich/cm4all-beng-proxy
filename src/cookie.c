/*
 * Cookie management.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "cookie.h"
#include "strutil.h"
#include "strref.h"
#include "strmap.h"
#include "http-string.h"

#include <string.h>

static struct cookie *
cookie_list_find(struct list_head *head, const char *name, size_t name_length)
{
    struct cookie *cookie;

    for (cookie = (struct cookie *)head->next;
         &cookie->siblings != head;
         cookie = (struct cookie *)cookie->siblings.next)
        if (strref_cmp(&cookie->name, name, name_length) == 0)
            return cookie;

    return NULL;
}

static __attr_always_inline void
ltrim(struct strref *s)
{
    while (s->length > 0 && char_is_whitespace(s->data[0])) {
        ++s->data;
        --s->length;
    }
}

static __attr_always_inline void
rtrim(struct strref *s)
{
    while (s->length > 0 && char_is_whitespace(strref_last(s)))
        --s->length;
}

static __attr_always_inline void
trim(struct strref *s)
{
    ltrim(s);
    rtrim(s);
}

static void
parse_key_value(pool_t pool, struct strref *input,
                struct strref *name, struct strref *value)
{
    http_next_token(input, name);
    if (strref_is_empty(name))
        return;

    ltrim(input);
    if (!strref_is_empty(input) && input->data[0] == '=') {
        strref_skip(input, 1);
        http_next_value(pool, input, value);
    } else
        strref_clear(value);
}

static int
parse_next_cookie(pool_t pool, struct list_head *head,
                  struct strref *input)
{
    struct strref name, value;
    struct cookie *cookie;

    parse_key_value(pool, input, &name, &value);
    if (strref_is_empty(&name))
        return 0;

    cookie = cookie_list_find(head, name.data, name.length);
    if (cookie == NULL) {
        cookie = p_malloc(pool, sizeof(*cookie));
        strref_set_dup(pool, &cookie->name, &name);
        cookie->valid_until = (time_t)-1; /* XXX */

        list_add(&cookie->siblings, head);
    }

    strref_set_dup(pool, &cookie->value, &value);

    ltrim(input);
    while (!strref_is_empty(input) && input->data[0] == ';') {
        strref_skip(input, 1);

        parse_key_value(pool, input, &name, &value);
        if (!strref_is_empty(&name)) {
            /* XXX */
        }
    }

    /* XXX: use "expires" and "path" arguments */

    return 1;
}

void
cookie_list_set_cookie2(pool_t pool, struct list_head *head, const char *value)
{
    struct strref input;

    strref_set_c(&input, value);

    while (1) {
        if (!parse_next_cookie(pool, head, &input))
            break;

        if (strref_is_empty(&input))
            return;

        if (input.data[0] != ',')
            break;

        strref_skip(&input, 1);
    }

    /* XXX log error */
}

void
cookie_list_http_header(struct strmap *headers, struct list_head *head,
                        pool_t pool)
{
    struct cookie *cookie;
    char buffer[2048];
    size_t length;

    if (list_empty(head))
        return;

    length = 0;

    for (cookie = (struct cookie *)head->next;
         &cookie->siblings != head;
         cookie = (struct cookie *)cookie->siblings.next) {
        if (sizeof(buffer) - length < cookie->name.length + 1 + cookie->value.length + 2)
            break;
        memcpy(buffer + length, cookie->name.data, cookie->name.length);
        length += cookie->name.length;
        buffer[length++] = '=';
        /* XXX escape? */
        memcpy(buffer + length, cookie->value.data, cookie->value.length);
        length += cookie->value.length;
        buffer[length++] = ';';
        buffer[length++] = ' ';
    }

    strmap_addn(headers, "Cookie2", "$Version=\"1\"");
    strmap_addn(headers, "Cookie", p_strndup(pool, buffer, length));
}

void
cookie_map_parse(struct strmap *cookies, const char *p, pool_t pool)
{
    const char *name, *value, *end;

    assert(cookies != NULL);
    assert(p != NULL);

    while (1) {
        value = strchr(p, '=');
        if (value == NULL)
            break;

        name = p_strndup(pool, p, value - p);

        ++value;

        if (*value == '"') {
            ++value;

            end = strchr(value, '"');
            if (end == NULL)
                break;

            value = p_strndup(pool, value, end - value);

            end = strchr(value, ';');
        } else {
            end = strchr(value, ';');

            if (end == NULL)
                value = p_strdup(pool, value);
            else
                value = p_strndup(pool, value, end - value);
        }

        strmap_addn(cookies, name, value);

        if (end == NULL)
            break;

        p = end + 1;
        while (*p != 0 && char_is_whitespace(*p))
            ++p;
    }
}
