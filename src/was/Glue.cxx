/*
 * High level WAS client.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "Glue.hxx"
#include "Stock.hxx"
#include "Launch.hxx"
#include "Client.hxx"
#include "http_response.hxx"
#include "Lease.hxx"
#include "tcp_stock.hxx"
#include "stock/GetHandler.hxx"
#include "stock/Stock.hxx"
#include "stock/Item.hxx"
#include "abort_close.hxx"
#include "spawn/ChildOptions.hxx"
#include "istream/istream.hxx"
#include "istream/istream_hold.hxx"
#include "pool.hxx"
#include "stopwatch.hxx"
#include "util/Cancellable.hxx"
#include "util/ConstBuffer.hxx"
#include "util/StringCompare.hxx"

#include <assert.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

class WasRequest final : public StockGetHandler, WasLease {
    struct pool &pool;

    Stopwatch *const stopwatch;

    StockItem *stock_item;

    http_method_t method;
    const char *uri;
    const char *script_name;
    const char *path_info;
    const char *query_string;
    StringMap &headers;
    Istream *body = nullptr;

    ConstBuffer<const char *> parameters;

    HttpResponseHandler &handler;
    CancellablePointer &cancel_ptr;

public:
    WasRequest(struct pool &_pool,
               Stopwatch *_stopwatch,
               http_method_t _method, const char *_uri,
               const char *_script_name, const char *_path_info,
               const char *_query_string,
               StringMap &_headers,
               ConstBuffer<const char *> _parameters,
               HttpResponseHandler &_handler,
               CancellablePointer &_cancel_ptr)
        :pool(_pool),
         stopwatch(_stopwatch),
         method(_method),
         uri(_uri), script_name(_script_name),
         path_info(_path_info), query_string(_query_string),
         headers(_headers), parameters(_parameters),
         handler(_handler), cancel_ptr(_cancel_ptr) {
    }

    CancellablePointer *SetBody(Istream *_body,
                                CancellablePointer *_cancel_ptr) {
        assert(body == nullptr);

        if (_body != nullptr) {
            body = istream_hold_new(pool, *_body);
            _cancel_ptr = &async_close_on_abort(pool, *body, *_cancel_ptr);
        }

        return _cancel_ptr;
    }

    /* virtual methods from class StockGetHandler */
    void OnStockItemReady(StockItem &item) override;
    void OnStockItemError(std::exception_ptr ep) override;

private:
    /* virtual methods from class WasLease */
    void ReleaseWas(bool reuse) override {
        stock_item->Put(!reuse);
    }

    void ReleaseWasStop(uint64_t input_received) override {
        was_stock_item_stop(*stock_item, input_received);
        stock_item->Put(false);
    }
};

/*
 * stock callback
 *
 */

void
WasRequest::OnStockItemReady(StockItem &item)
{
    stock_item = &item;

    const auto &process = was_stock_item_get(item);

    was_client_request(pool, item.stock.GetEventLoop(), stopwatch,
                       process.control.Get(),
                       process.input.Get(), process.output.Get(),
                       *this,
                       method, uri,
                       script_name, path_info,
                       query_string,
                       headers, body,
                       parameters,
                       handler, cancel_ptr);
}

void
WasRequest::OnStockItemError(std::exception_ptr ep)
{
    if (body != nullptr)
        body->CloseUnused();

    handler.InvokeError(ep);
}

/*
 * constructor
 *
 */

gcc_pure
static const char *
GetComaClass(ConstBuffer<const char *> parameters)
{
    for (const char *i : parameters) {
        const char *result = StringAfterPrefix(i, "COMA_CLASS=");
        if (result != nullptr && *result != 0)
            return result;
    }

    return nullptr;
}

static Stopwatch *
stopwatch_new_was(struct pool &pool, const char *path, const char *uri,
                  const char *path_info,
                  ConstBuffer<const char *> parameters)
{
    assert(path != nullptr);
    assert(uri != nullptr);

    if (!stopwatch_is_enabled())
        return nullptr;

    /* special case for a very common COMA application */
    const char *coma_class = GetComaClass(parameters);
    if (coma_class != nullptr)
        path = coma_class;

    const char *slash = strrchr(path, '/');
    if (slash != nullptr && slash[1] != 0)
        path = slash + 1;

    if (path_info != nullptr && *path_info != 0)
        uri = path_info;

    return stopwatch_new(&pool, p_strcat(&pool, path, " ", uri, nullptr));
}

void
was_request(struct pool &pool, StockMap &was_stock,
            const ChildOptions &options,
            const char *action,
            const char *path,
            ConstBuffer<const char *> args,
            http_method_t method, const char *uri,
            const char *script_name, const char *path_info,
            const char *query_string,
            StringMap &headers, Istream *body,
            ConstBuffer<const char *> parameters,
            HttpResponseHandler &handler,
            CancellablePointer &cancel_ptr)
{
    if (action == nullptr)
        action = path;

    auto request = NewFromPool<WasRequest>(pool, pool,
                                           stopwatch_new_was(pool, path, uri,
                                                             path_info,
                                                             parameters),
                                           method, uri, script_name,
                                           path_info, query_string,
                                           headers, parameters,
                                           handler, cancel_ptr);

    was_stock_get(&was_stock, &pool,
                  options,
                  action, args,
                  *request, *request->SetBody(body, &cancel_ptr));
}
