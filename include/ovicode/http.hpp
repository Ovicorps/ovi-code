#pragma once

#include <string>
#include <vector>

namespace ovicode::http {

struct Response {
    long status = 0;       // HTTP status; 0 if transport error
    std::string body;
    std::string error;     // non-empty on transport error
};

// Synchronous HTTP POST with JSON body. `headers` is a flat list of
// "Header: value" strings. Timeout is in seconds.
Response post(const std::string& url,
              const std::vector<std::string>& headers,
              const std::string& body,
              long timeout_seconds = 120);

}  // namespace ovicode::http
