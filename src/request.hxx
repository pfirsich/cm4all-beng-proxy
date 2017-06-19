/*
 * The BENG request struct.  This is only used by the handlers
 * (handler.c, file-handler.c etc.).
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_REQUEST_HXX
#define BENG_PROXY_REQUEST_HXX

#include "uri/uri_parser.hxx"
#include "translation/Request.hxx"
#include "translation/Response.hxx"
#include "translation/Transformation.hxx"
#include "delegate/Handler.hxx"
#include "penv.hxx"
#include "session.hxx"
#include "widget/View.hxx"
#include "http_response.hxx"
#include "glibfwd.hxx"
#include "util/Cancellable.hxx"

#include <exception>

class Istream;
class HttpHeaders;
class StringMap;
struct BpInstance;
struct BpConnection;
struct HttpServerRequest;

struct Request final : HttpResponseHandler, DelegateHandler, Cancellable {
    struct pool &pool;

    BpInstance &instance;
    BpConnection &connection;

    HttpServerRequest &request;
    struct parsed_uri uri;

    StringMap *args = nullptr;

    StringMap *cookies = nullptr;

    /**
     * The name of the session cookie.
     */
    const char *session_cookie;

    SessionId session_id;
    struct session_id_string session_id_string;
    bool send_session_cookie = false;

    /**
     * The realm name of the request.  This is valid only after the
     * translation server has responded, because the translation
     * server may override it.
     *
     * This is set by ApplyTranslateRealm().  We initialize it here to
     * nullptr so ApplyTranslateRealm() can skip a second call when
     * it's already set.
     */
    const char *realm = nullptr;

    /**
     * Is this request "stateless", i.e. is session management
     * disabled?  This is initialized by request_determine_session(),
     * and may be disabled later by handle_translated_request().
     */
    bool stateless;

    struct {
        TranslateRequest request;
        const TranslateResponse *response;

        ResourceAddress address;

        /**
         * The next transformation.
         */
        const Transformation *transformation;

        /**
         * The next transformation from the
         * #TRANSLATE_CONTENT_TYPE_LOOKUP response.  These are applied
         * before other transformations.
         */
        const Transformation *suffix_transformation;

        /**
         * A pointer to the "previous" translate response, non-nullptr
         * only if beng-proxy sends a second translate request with a
         * CHECK packet.
         */
        const TranslateResponse *previous;

        /**
         * Number of CHECK packets followed so far.  This variable is
         * used for loop detection.
         */
        unsigned n_checks;

        unsigned n_internal_redirects;

        unsigned n_read_file;

        /**
         * Number of FILE_NOT_FOUND packets followed so far.  This
         * variable is used for loop detection.
         */
        unsigned n_file_not_found;

        /**
         * Number of #TRANSLATE_DIRECTORY_INDEX packets followed so
         * far.  This variable is used for loop detection.
         */
        unsigned n_directory_index;

        unsigned n_probe_path_suffixes;

        /**
         * The Content-Type returned by suffix_registry_lookup().
         */
        const char *content_type = nullptr;

        char *enotdir_uri;
        const char *enotdir_path_info;

        /**
         * Did we see #TRANSLATE_WANT with #TRANSLATE_USER?  If so,
         * and the user gets modified (see #user_modified), then we
         * need to repeat the initial translation with the new user
         * value.
         */
        bool want_user = false;

        /**
         * Did we receive #TRANSLATE_USER which modified the session's
         * "user" attribute?  If so, then we need to repeat the
         * initial translation with the new user value.
         */
        bool user_modified = false;
    } translate;

    /**
     * Area for handler-specific state variables.  This is a union to
     * save memory.
     */
    union {
        struct {
            const FileAddress *address;
        } file;

        struct {
            const char *path;
        } delegate;
    } handler;

    /**
     * The URI used for the cookie jar.  This is only used by
     * proxy_handler().
     */
    const char *cookie_uri;

    /**
     * The product token (RFC 2616 3.8) being forwarded; nullptr if
     * beng-proxy shall generate one.
     */
    const char *product_token = nullptr;

#ifndef NO_DATE_HEADER
    /**
     * The "date" response header (RFC 2616 14.18) being forwarded;
     * nullptr if beng-proxy shall generate one.
     */
    const char *date = nullptr;
#endif

    /**
     * An identifier for the source stream of the current
     * transformation.  This is used by the filter cache to address
     * resources.
     */
    const char *resource_tag;

    struct processor_env env;

    /**
     * A pointer to the request body, or nullptr if there is none.  Once
     * the request body has been "used", this pointer gets cleared.
     */
    Istream *body;

    /**
     * Shall the Set-Cookie2 header received from the next server be
     * evaluated?
     */
    bool collect_cookies = false;

    /**
     * Is the processor active, and is there a focused widget?
     */
    bool processor_focus;

    /**
     * Was the response already transformed?  The error document only
     * applies to the original, untransformed response.
     */
    bool transformed = false;

    /**
     * Is the pending response compressed?  This flag is used to avoid
     * compressing twice via #TRANSLATE_AUTO_GZIP and others.
     */
    bool compressed = false;

#ifndef NDEBUG
    bool response_sent = false;
#endif

    CancellablePointer cancel_ptr;

    Request(BpInstance &_instance, BpConnection &_connection,
            HttpServerRequest &_request);

    void ParseArgs();

    /**
     * Submit the #TranslateResponse to the translation cache.
     */
    void SubmitTranslateRequest();

    void OnTranslateResponse(const TranslateResponse &response);
    void OnTranslateResponseAfterAuth(const TranslateResponse &response);
    void OnTranslateResponse2(const TranslateResponse &response);

    /**
     * Enable the "stateless" flag, which disables session management
     * permanently for this request.
     */
    void MakeStateless() {
        session_id.Clear();
        stateless = true;
    }

    /**
     * Apply and verify #TRANSLATE_REALM.
     */
    void ApplyTranslateRealm(const TranslateResponse &response,
                             ConstBuffer<void> auth_base);

    /**
     * Copy the packets #TRANSLATE_SESSION, #TRANSLATE_USER,
     * #TRANSLATE_LANGUAGE from the #TranslateResponse to the
     * #session.
     *
     * @return the session
     */
    RealmSessionLease ApplyTranslateSession(const TranslateResponse &response);

    bool CheckHandleReadFile(const TranslateResponse &response);
    bool CheckHandleProbePathSuffixes(const TranslateResponse &response);
    bool CheckHandleRedirect(const TranslateResponse &response);
    bool CheckHandleBounce(const TranslateResponse &response);
    bool CheckHandleStatus(const TranslateResponse &response);
    bool CheckHandleMessage(const TranslateResponse &response);
    bool CheckHandleRedirectBounceStatus(const TranslateResponse &response);

    /**
     * Handle #TRANSLATE_AUTH.
     */
    void HandleAuth(const TranslateResponse &response);

    /**
     * Handle the request by forwarding it to the given address.
     */
    void HandleAddress(const ResourceAddress &address);

    bool IsTransformationEnabled() const {
        return translate.response->views->transformation != nullptr;
    }

    /**
     * Returns true if the first transformation (if any) is the
     * processor.
     */
    bool IsProcessorFirst() const {
        return IsTransformationEnabled() &&
            translate.response->views->transformation->type
            == Transformation::Type::PROCESS;
    }

    bool IsProcessorEnabled() const;

    bool HasTransformations() const {
        return translate.transformation != nullptr ||
            translate.suffix_transformation != nullptr;
    }

    void CancelTransformations() {
        translate.transformation = nullptr;
        translate.suffix_transformation = nullptr;
    }

    const Transformation *PopTransformation() {
        const Transformation *t = translate.suffix_transformation;
        if (t != nullptr)
            translate.suffix_transformation = t->next;
        else {
            t = translate.transformation;
            if (t != nullptr)
                translate.transformation = t->next;
        }

        return t;
    }

    /**
     * Discard the request body if it was not used yet.  Call this
     * before sending the response to the HTTP server library.
     */
    void DiscardRequestBody();

private:
    const StringMap *GetCookies();
    const char *GetCookieSessionId();
    const char *GetUriSessionId();

    SessionLease LoadSession(const char *_session_id);

public:
    void DetermineSession();

    SessionLease GetSession() const {
        return SessionLease(session_id);
    }

    RealmSessionLease GetRealmSession() const;

    SessionLease MakeSession();
    RealmSessionLease MakeRealmSession();

    void IgnoreSession();
    void DiscardSession();

    const char *GetCookieURI() const {
        return cookie_uri;
    }

    const char *GetCookieHost() const;
    void CollectCookies(const StringMap &headers);

    /* virtual methods from class Cancellable */
    void Cancel() override {
        DiscardRequestBody();

        /* forward the abort to the http_server library */
        cancel_ptr.Cancel();
    }

    /* virtual methods from class HttpResponseHandler */
    void OnHttpResponse(http_status_t status, StringMap &&headers,
                        Istream *body) override;
    void OnHttpError(GError *error) override;

    /* virtual methods from class DelegateHandler */
    void OnDelegateSuccess(int fd) override;
    void OnDelegateError(GError *error) override;
};

void
response_dispatch(Request &request,
                  http_status_t status, HttpHeaders &&headers,
                  Istream *body);

void
response_dispatch_message(Request &request, http_status_t status,
                          const char *msg);

void
response_dispatch_message2(Request &request, http_status_t status,
                           HttpHeaders &&headers, const char *msg);

void
response_dispatch_error(Request &request, GError *error);

void
response_dispatch_log(Request &request, http_status_t status,
                      const char *log_msg);

void
response_dispatch_log(Request &request, http_status_t status,
                      const char *msg, const char *log_msg);

void
response_dispatch_log(Request &request, http_status_t status,
                      const char *msg,
                      std::exception_ptr ep);

void
response_dispatch_redirect(Request &request, http_status_t status,
                           const char *location, const char *msg);

#endif
