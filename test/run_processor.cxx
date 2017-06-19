#include "StdioSink.hxx"
#include "FailingResourceLoader.hxx"
#include "PInstance.hxx"
#include "fb_pool.hxx"
#include "processor.hxx"
#include "penv.hxx"
#include "uri/uri_parser.hxx"
#include "widget/Inline.hxx"
#include "widget/Widget.hxx"
#include "widget/Class.hxx"
#include "widget/RewriteUri.hxx"
#include "istream/istream_file.hxx"
#include "istream/istream_string.hxx"
#include "util/StringView.hxx"

/*
 * emulate missing libraries
 *
 */

struct tcache *global_translate_cache;

Istream *
embed_inline_widget(struct pool &pool,
                    gcc_unused struct processor_env &env,
                    gcc_unused bool plain_text,
                    Widget &widget)
{
    const char *s = widget.GetIdPath();
    if (s == nullptr)
        s = "widget";

    return istream_string_new(&pool, s);
}

WidgetSession *
widget_get_session(gcc_unused Widget *widget,
                   gcc_unused RealmSession *session,
                   gcc_unused bool create)
{
    return nullptr;
}

enum uri_mode
parse_uri_mode(gcc_unused StringView s)
{
    return URI_MODE_DIRECT;
}

Istream *
rewrite_widget_uri(gcc_unused struct pool &pool,
                   gcc_unused struct processor_env &env,
                   gcc_unused struct tcache &translate_cache,
                   gcc_unused Widget &widget,
                   gcc_unused StringView value,
                   gcc_unused enum uri_mode mode,
                   gcc_unused bool stateful,
                   gcc_unused const char *view,
                   gcc_unused const struct escape_class *escape)
{
    return nullptr;
}

int main(int argc, char **argv) {
    const char *uri;
    bool ret;
    struct parsed_uri parsed_uri;

    (void)argc;
    (void)argv;

    const ScopeFbPoolInit fb_pool_init;
    PInstance instance;

    uri = "/beng.html";
    ret = parsed_uri.Parse(uri);
    if (!ret) {
        fprintf(stderr, "uri_parse() failed\n");
        exit(2);
    }

    Widget widget(instance.root_pool, &root_widget_class);

    SessionId session_id;
    session_id.Generate();

    FailingResourceLoader resource_loader;
    struct processor_env env(instance.root_pool, instance.event_loop,
                             resource_loader, resource_loader,
                             nullptr, nullptr,
                             "localhost:8080",
                             "localhost:8080",
                             "/beng.html",
                             "http://localhost:8080/beng.html",
                             &parsed_uri,
                             nullptr,
                             nullptr,
                             session_id, "foo",
                             HTTP_METHOD_GET, nullptr);

    Istream *result =
        processor_process(instance.root_pool,
                          *istream_file_new(instance.event_loop,
                                            instance.root_pool,
                                            "/dev/stdin", (off_t)-1,
                                            NULL),
                          widget, env, PROCESSOR_CONTAINER);

    StdioSink sink(*result);
    sink.LoopRead();
}
