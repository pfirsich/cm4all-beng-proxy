/*
 * Emulation layer for Google gadgets.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef __BENG_GOOGLE_GADGET_INTERNALH
#define __BENG_GOOGLE_GADGET_INTERNALH

#include "google-gadget.h"
#include "async.h"
#include "http-response.h"
#include "strref.h"

struct google_gadget {
    pool_t pool;
    struct processor_env *env;
    struct widget *widget;

    struct async_operation async_operation;

    istream_t delayed, subst;

    struct http_response_handler_ref response_handler;

    /** reference to an operation which we are waiting for; either the
        HTTP client loading the gadget, or the HTTP client loading the
        locale */
    struct async_operation_ref async_ref;

    struct parser *parser;

    struct {
        enum {
            TAG_NONE,
            TAG_LOCALE,
            TAG_CONTENT,
        } tag;

        enum {
            TYPE_NONE,
            TYPE_URL,
            TYPE_HTML,
            TYPE_HTML_INLINE,
        } type;

        bool sending_content:1, in_parser:1;

        const char *url;
    } from_parser;

    bool has_locale:1, waiting_for_locale:1;

    struct {
        struct parser *parser;
        bool in_msg_tag;
        const char *key;
        struct strref value;
    } msg;

    struct istream output;
};

void
google_gadget_msg_eof(struct google_gadget *gg);

void
google_gadget_msg_abort(struct google_gadget *gg);

void
google_gadget_msg_load(struct google_gadget *gg, const char *url);

void
google_gadget_msg_close(struct google_gadget *gg);

#endif
