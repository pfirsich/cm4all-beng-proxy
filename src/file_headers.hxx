/*
 * Handle the request/response headers for static files.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_FILE_HEADERS_HXX
#define BENG_PROXY_FILE_HEADERS_HXX

#include "http/Range.hxx"

#include <chrono>

#include <sys/types.h>

class GrowingBuffer;
struct Request;
struct stat;

struct file_request {
    HttpRangeRequest range;

    explicit file_request(off_t _size):range(_size) {}
};

bool
file_evaluate_request(Request &request2,
                      int fd, const struct stat &st,
                      struct file_request &file_request);

void
file_cache_headers(GrowingBuffer &headers,
                   int fd, const struct stat &st,
                   std::chrono::seconds expires_relative);

void
file_response_headers(GrowingBuffer &headers,
                      const char *override_content_type,
                      int fd, const struct stat &st,
                      std::chrono::seconds expires_relative,
                      bool processor_enabled, bool processor_first);

#endif
