#include "css_parser.h"
#include "istream.h"

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

static bool should_exit;

/*
 * parser handler
 *
 */

static void
my_parser_url(const struct css_parser_url *url, void *ctx)
{
    (void)ctx;

    printf("%.*s\n", (int)url->value.length, url->value.data);
}

static void
my_parser_eof(void *ctx, off_t length)
{
    (void)ctx;
    (void)length;

    should_exit = true;
}

static __attr_noreturn void
my_parser_error(GError *error, void *ctx)
{
    (void)ctx;

    fprintf(stderr, "ABORT: %s\n", error->message);
    g_error_free(error);
    exit(2);
}

static const struct css_parser_handler my_parser_handler = {
    .url = my_parser_url,
    .eof = my_parser_eof,
    .error = my_parser_error,
};


/*
 * main
 *
 */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    struct pool *root_pool = pool_new_libc(NULL, "root");
    struct pool *pool = pool_new_linear(root_pool, "test", 8192);

    struct istream *istream = istream_file_new(pool, "/dev/stdin", (off_t)-1);
    struct css_parser *parser =
        css_parser_new(pool, istream, &my_parser_handler, NULL);

    while (!should_exit)
        css_parser_read(parser);

    pool_unref(pool);
    pool_unref(root_pool);
    pool_commit();

    pool_recycler_clear();
}
