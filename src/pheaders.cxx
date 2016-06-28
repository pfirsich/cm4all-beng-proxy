/*
 * Process CM4all commands in a HTML stream, e.g. embeddings.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "pheaders.hxx"
#include "header_copy.hxx"
#include "strmap.hxx"

StringMap *
processor_header_forward(struct pool &pool, const StringMap &src)
{
    auto *headers2 = strmap_new(&pool);

    static const char *const copy_headers[] = {
        "content-language",
        "content-type",
        "content-disposition",
        "location",
        nullptr,
    };

    header_copy_list(src, *headers2, copy_headers);

#ifndef NDEBUG
    /* copy Wildfire headers if present (debug build only, to avoid
       overhead on production servers) */
    if (src.Get("x-wf-protocol-1") != nullptr)
        header_copy_prefix(src, *headers2, "x-wf-");
#endif

    /* reportedly, the Internet Explorer caches uncacheable resources
       without revalidating them; only Cache-Control will prevent him
       from showing stale data to the user */
    headers2->Add("cache-control", "no-store");

    return headers2;
}
