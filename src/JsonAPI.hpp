#ifndef JSON_API_HPP
#define JSON_API_HPP

#include <string>

namespace JsonAPI {
    // Parse request JSON using jct and produce a JSON response string.
    // Returns true on success.
    bool process_json(const std::string &in, std::string &out);
}

#endif // JSON_API_HPP

