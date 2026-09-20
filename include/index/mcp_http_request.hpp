#pragma once

#include <map>
#include <string>

namespace qt::index {

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> query;
    std::string body;
    unsigned version = 11;
    bool keep_alive = false;
};

}  // namespace qt::index
