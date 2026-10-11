#include "ovicode/http.hpp"

#include <curl/curl.h>

#include <chrono>
#include <mutex>

#include "ovicode/metrics.hpp"

namespace ovicode::http {

namespace {

std::once_flag g_init_flag;
constexpr std::size_t kMaxResponseBytes = 8 * 1024 * 1024;

struct ResponseBuffer {
    std::string* body;
    bool exceeded = false;
};

void initOnce() {
    std::call_once(g_init_flag, [] {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    });
}

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buffer = static_cast<ResponseBuffer*>(userdata);
    const std::size_t bytes = size * nmemb;
    if (bytes > kMaxResponseBytes - buffer->body->size()) {
        buffer->exceeded = true;
        return 0;
    }
    buffer->body->append(ptr, bytes);
    return bytes;
}

int progressCb(void* userdata, curl_off_t, curl_off_t, curl_off_t,
               curl_off_t) {
    const auto* stop = static_cast<const std::stop_token*>(userdata);
    return stop->stop_requested() ? 1 : 0;
}

}  // namespace

Response post(const std::string& url,
              const std::vector<std::string>& headers,
              const std::string& body, long timeout_seconds,
              std::stop_token stop) {
    metrics::ScopedTimer timer(metrics::Distribution::HttpDurationUs);
    metrics::add(metrics::Counter::HttpRequests);
    metrics::observe(metrics::Distribution::HttpRequestBodyBytes, body.size());
    initOnce();
    Response r;
    ResponseBuffer response_buffer{&r.body};
    if (stop.stop_requested()) {
        r.cancelled = true;
        return r;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        r.error = "curl_easy_init failed";
        return r;
    }
    struct curl_slist* hdrs = nullptr;
    for (const auto& h : headers) hdrs = curl_slist_append(hdrs, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buffer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &stop);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    // Fail fast on unreachable hosts (DNS / TCP) rather than waiting for the
    // full per-request timeout — keeps the TUI's worker thread responsive.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ovi-code/0.1 (+libcurl)");

    CURLcode code = curl_easy_perform(curl);
    if (response_buffer.exceeded) {
        r.error = "Response exceeded the 8 MiB limit";
        metrics::add(metrics::Counter::HttpTransportErrors);
    } else if (code == CURLE_ABORTED_BY_CALLBACK && stop.stop_requested()) {
        r.cancelled = true;
    } else if (code != CURLE_OK) {
        r.error = curl_easy_strerror(code);
        metrics::add(metrics::Counter::HttpTransportErrors);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &r.status);
        if (r.status < 200 || r.status >= 300) {
            metrics::add(metrics::Counter::HttpNon2xx);
        }
    }
    metrics::observe(metrics::Distribution::HttpResponseBodyBytes,
                     r.body.size());
    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return r;
}

}  // namespace ovicode::http
