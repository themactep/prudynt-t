#pragma once

// RFC 2617 / RFC 7616 Digest authentication helpers for the built-in RTSP
// server (and reusable for HTTP).  Only MD5 + qop="auth" is implemented:
// that is the universally supported profile across RTSP clients.

#include "util/Md5.hpp"

#include <cctype>
#include <string>

namespace simple_rtsp {

struct DigestCredentials {
    std::string username;
    std::string realm;
    std::string nonce;
    std::string uri;
    std::string response;
    std::string algorithm;
    std::string qop;
    std::string nc;
    std::string cnonce;
    std::string opaque;
};

inline bool iequals(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Parse the parameter list that follows the "Digest " scheme token in an
// Authorization / Proxy-Authorization header.  Handles quoted values
// (including backslash escapes) and bare tokens.  Keys are matched
// case-insensitively.
inline bool parseDigestParameters(const char *params, DigestCredentials &out) {
    if (!params) return false;
    const char *p = params;
    bool any = false;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        std::string key;
        while (*p && *p != '=' && *p != ' ' && *p != '\t' && *p != ',')
            key += *p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') {
            // Malformed; skip to next comma.
            while (*p && *p != ',') p++;
            continue;
        }
        p++; // '='
        while (*p == ' ' || *p == '\t') p++;

        std::string value;
        if (*p == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) p++;
                value += *p++;
            }
            if (*p == '"') p++;
        } else {
            while (*p && *p != ',' && *p != ' ' && *p != '\t') value += *p++;
        }

        if (iequals(key, "username")) out.username = value;
        else if (iequals(key, "realm")) out.realm = value;
        else if (iequals(key, "nonce")) out.nonce = value;
        else if (iequals(key, "uri")) out.uri = value;
        else if (iequals(key, "response")) out.response = value;
        else if (iequals(key, "algorithm")) out.algorithm = value;
        else if (iequals(key, "qop")) out.qop = value;
        else if (iequals(key, "nc")) out.nc = value;
        else if (iequals(key, "cnonce")) out.cnonce = value;
        else if (iequals(key, "opaque")) out.opaque = value;
        any = true;
    }
    return any;
}

// Digest response per RFC 2617 section 3.2.2.1.  When qop is empty the
// RFC 2069 (legacy) formula is used.
inline std::string digestResponse(const std::string &username,
                                  const std::string &realm,
                                  const std::string &password,
                                  const std::string &method,
                                  const std::string &uri,
                                  const std::string &nonce,
                                  const std::string &nc,
                                  const std::string &cnonce,
                                  const std::string &qop) {
    const std::string ha1 =
        util::md5Hex(username + ":" + realm + ":" + password);
    const std::string ha2 = util::md5Hex(method + ":" + uri);
    if (!qop.empty())
        return util::md5Hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" +
                            qop + ":" + ha2);
    return util::md5Hex(ha1 + ":" + nonce + ":" + ha2);
}

} // namespace simple_rtsp
