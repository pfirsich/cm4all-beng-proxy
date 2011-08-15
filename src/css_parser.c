/*
 * Simple parser for CSS (Cascading Style Sheets).
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "css_parser.h"
#include "pool.h"
#include "istream.h"
#include "strutil.h"

enum css_parser_state {
    CSS_PARSER_NONE,
    CSS_PARSER_BLOCK,
    CSS_PARSER_DISCARD_QUOTED,
    CSS_PARSER_PRE_VALUE,
    CSS_PARSER_VALUE,
    CSS_PARSER_PRE_URL,
    CSS_PARSER_URL,
};

struct css_parser {
    struct pool *pool;

    struct istream *input;
    off_t position;

    const struct css_parser_handler *handler;
    void *handler_ctx;

    /* internal state */
    enum css_parser_state state;

    char quote;

    size_t value_length;
    char value[64];

    off_t url_start;
    size_t url_length;
    char url[1024];
};

static const char *
skip_whitespace(const char *p, const char *end)
{
    while (p < end && char_is_whitespace(*p))
        ++p;

    return p;
}

static size_t
css_parser_feed(struct css_parser *parser, const char *start, size_t length)
{
    assert(parser != NULL);
    assert(parser->input != NULL);
    assert(start != NULL);
    assert(length > 0);

    const char *buffer = start, *end = start + length, *p;
    size_t nbytes;
    struct css_parser_url url;

    while (buffer < end) {
        switch (parser->state) {
        case CSS_PARSER_NONE:
            p = memchr(buffer, '{', end - buffer);
            if (p == NULL) {
                nbytes = end - start;
                parser->position += (off_t)nbytes;
                return nbytes;
            }

            parser->state = CSS_PARSER_BLOCK;
            buffer = p + 1;
            break;

        case CSS_PARSER_BLOCK:
            do {
                switch (*buffer) {
                case '}':
                    /* end of block */
                    parser->state = CSS_PARSER_NONE;
                    break;

                case ':':
                    /* colon introduces property value */
                    parser->state = CSS_PARSER_PRE_VALUE;
                    break;

                case '\'':
                case '"':
                    parser->state = CSS_PARSER_DISCARD_QUOTED;
                    parser->quote = *buffer;
                    break;
                }

                ++buffer;
            } while (buffer < end && parser->state == CSS_PARSER_BLOCK);
            break;

        case CSS_PARSER_DISCARD_QUOTED:
            p = memchr(buffer, parser->quote, end - buffer);
            if (p == NULL) {
                nbytes = end - start;
                parser->position += (off_t)nbytes;
                return nbytes;
            }

            parser->state = CSS_PARSER_BLOCK;
            buffer = p + 1;
            break;

        case CSS_PARSER_PRE_VALUE:
            buffer = skip_whitespace(buffer, end);
            if (buffer < end) {
                switch (*buffer) {
                case '}':
                    /* end of block */
                    parser->state = CSS_PARSER_NONE;
                    ++buffer;
                    break;

                case ';':
                    parser->state = CSS_PARSER_BLOCK;
                    ++buffer;
                    break;

                default:
                    parser->state = CSS_PARSER_VALUE;
                    parser->value_length = 0;
                }
            }

            break;

        case CSS_PARSER_VALUE:
            do {
                switch (*buffer) {
                case '}':
                    /* end of block */
                    parser->state = CSS_PARSER_NONE;
                    break;

                case ';':
                    parser->state = CSS_PARSER_BLOCK;
                    break;

                case '\'':
                case '"':
                    parser->state = CSS_PARSER_DISCARD_QUOTED;
                    parser->quote = *buffer;
                    break;

                default:
                    if (parser->value_length >= sizeof(parser->value))
                        break;

                    parser->value[parser->value_length++] = *buffer;
                    if (parser->value_length == 4 &&
                        memcmp(parser->value, "url(", 4) == 0)
                        parser->state = CSS_PARSER_PRE_URL;
                }

                ++buffer;
            } while (buffer < end && parser->state == CSS_PARSER_VALUE);
            break;

        case CSS_PARSER_PRE_URL:
            buffer = skip_whitespace(buffer, end);
            if (buffer < end) {
                switch (*buffer) {
                case '}':
                    /* end of block */
                    parser->state = CSS_PARSER_NONE;
                    ++buffer;
                    break;

                case '\'':
                case '"':
                    parser->state = CSS_PARSER_URL;
                    parser->quote = *buffer++;
                    parser->url_start = parser->position + (off_t)(buffer - start);
                    parser->url_length = 0;
                    break;

                default:
                    parser->state = CSS_PARSER_BLOCK;
                }
            }

            break;

        case CSS_PARSER_URL:
            p = memchr(buffer, parser->quote, end - buffer);
            if (p == NULL) {
                size_t copy = end - buffer;
                if (copy > sizeof(parser->url) - parser->url_length)
                    copy = sizeof(parser->url) - parser->url_length;
                memcpy(parser->url + parser->url_length, buffer, copy);
                parser->url_length += copy;

                nbytes = end - start;
                parser->position += (off_t)nbytes;
                return nbytes;
            }

            /* found the end of the URL - copy the rest, and invoke
               the handler method "url()" */
            nbytes = p - buffer;
            if (nbytes > sizeof(parser->url) - parser->url_length)
                nbytes = sizeof(parser->url) - parser->url_length;
            memcpy(parser->url + parser->url_length, buffer, nbytes);
            parser->url_length += nbytes;

            buffer = p + 1;
            parser->state = CSS_PARSER_BLOCK;

            url.start = parser->url_start;
            url.end = parser->position + (off_t)(p - start);
            strref_set(&url.value, parser->url, parser->url_length);
            parser->handler->url(&url, parser->handler_ctx);
            if (parser->input == NULL)
                return 0;

            break;
        }
    }

    assert(parser->input != NULL);

    parser->position += length;
    return length;
}

/*
 * istream handler
 *
 */

static size_t
css_parser_input_data(const void *data, size_t length, void *ctx)
{
    struct css_parser *parser = ctx;

    pool_ref(parser->pool);
    size_t nbytes = css_parser_feed(parser, data, length);
    pool_unref(parser->pool);

    return nbytes;
}

static void
css_parser_input_eof(void *ctx)
{
    struct css_parser *parser = ctx;

    assert(parser->input != NULL);

    parser->input = NULL;
    parser->handler->eof(parser->handler_ctx, parser->position);
    pool_unref(parser->pool);
}

static void
css_parser_input_abort(GError *error, void *ctx)
{
    struct css_parser *parser = ctx;

    assert(parser->input != NULL);

    parser->input = NULL;
    parser->handler->error(error, parser->handler_ctx);
    pool_unref(parser->pool);
}

static const struct istream_handler css_parser_input_handler = {
    .data = css_parser_input_data,
    .eof = css_parser_input_eof,
    .abort = css_parser_input_abort,
};

/*
 * constructor
 *
 */

struct css_parser *
css_parser_new(struct pool *pool, struct istream *input,
               const struct css_parser_handler *handler, void *handler_ctx)
{
    assert(pool != NULL);
    assert(input != NULL);
    assert(handler != NULL);
    assert(handler->eof != NULL);
    assert(handler->error != NULL);

    pool_ref(pool);

    struct css_parser *parser = p_malloc(pool, sizeof(*parser));
    parser->pool = pool;

    istream_assign_handler(&parser->input, input,
                           &css_parser_input_handler, parser,
                           0);
    parser->position = 0;

    parser->handler = handler;
    parser->handler_ctx = handler_ctx;

    parser->state = CSS_PARSER_NONE;

    return parser;
}

void
css_parser_close(struct css_parser *parser)
{
    assert(parser != NULL);
    assert(parser->input != NULL);

    istream_close(parser->input);
    pool_unref(parser->pool);
}

void
css_parser_read(struct css_parser *parser)
{
    assert(parser != NULL);
    assert(parser->input != NULL);

    istream_read(parser->input);
}
