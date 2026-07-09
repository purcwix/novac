#pragma once
#include <string>
#include <unordered_map>
#include <vector>

namespace novac {

struct FetchRequest {
    std::string method  = "GET";
    std::string url;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
};

struct FetchResponse {
    int         status  = 0;
    bool        ok      = false;
    std::string body;
    std::unordered_map<std::string, std::string> headers;
    std::string statusText;
};

// Parse a URL into parts
struct ParsedUrl {
    std::string scheme;   // http / https
    std::string host;
    std::string port;
    std::string path;
};

ParsedUrl   parseUrl(const std::string& url);
FetchResponse syncFetch(const FetchRequest& req);

} // namespace novac