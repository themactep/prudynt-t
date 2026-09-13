#pragma once

// Self-contained MD5 (RFC 1321) used for RTSP/HTTP Digest authentication.
// Deliberately dependency-free: the firmware does not enable OpenSSL for
// every camera, and prudynt links no crypto library of its own.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace util {

class Md5 {
public:
    static constexpr size_t kDigestSize = 16;

    Md5() { reset(); }

    void reset() {
        a_ = 0x67452301u;
        b_ = 0xefcdab89u;
        c_ = 0x98badcfeu;
        d_ = 0x10325476u;
        bitLen_ = 0;
        bufLen_ = 0;
    }

    void update(const void *data, size_t len) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        bitLen_ += static_cast<uint64_t>(len) * 8;

        if (bufLen_ > 0) {
            size_t need = 64 - bufLen_;
            size_t take = len < need ? len : need;
            std::memcpy(buf_ + bufLen_, p, take);
            bufLen_ += take;
            p += take;
            len -= take;
            if (bufLen_ == 64) {
                transform(buf_);
                bufLen_ = 0;
            }
        }
        while (len >= 64) {
            transform(p);
            p += 64;
            len -= 64;
        }
        if (len > 0) {
            std::memcpy(buf_, p, len);
            bufLen_ = len;
        }
    }

    void update(const std::string &s) { update(s.data(), s.size()); }

    // Writes the raw 16-byte digest into out[].
    void final(uint8_t out[kDigestSize]) {
        uint64_t bits = bitLen_;
        static const uint8_t pad[64] = {0x80};

        uint8_t lenBytes[8];
        for (int i = 0; i < 8; i++)
            lenBytes[i] = static_cast<uint8_t>(bits >> (8 * i));

        // Pad to 56 mod 64 (leaving room for the 8-byte length).
        size_t padLen = (bufLen_ < 56) ? (56 - bufLen_) : (120 - bufLen_);
        update(pad, padLen);
        update(lenBytes, sizeof(lenBytes));

        uint32_t st[4] = {a_, b_, c_, d_};
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                out[i * 4 + j] = static_cast<uint8_t>(st[i] >> (8 * j));
    }

    // Convenience: lowercase hex digest.
    static std::string hex(const uint8_t digest[kDigestSize]) {
        static const char kHex[] = "0123456789abcdef";
        std::string out;
        out.resize(kDigestSize * 2);
        for (size_t i = 0; i < kDigestSize; i++) {
            out[i * 2]     = kHex[digest[i] >> 4];
            out[i * 2 + 1] = kHex[digest[i] & 0x0f];
        }
        return out;
    }

    static std::string hash(const void *data, size_t len) {
        Md5 ctx;
        ctx.update(data, len);
        uint8_t digest[kDigestSize];
        ctx.final(digest);
        return hex(digest);
    }

    static std::string hash(const std::string &s) {
        return hash(s.data(), s.size());
    }

private:
    static uint32_t rotl(uint32_t x, uint32_t n) {
        return (x << n) | (x >> (32 - n));
    }

    void transform(const uint8_t block[64]) {
        static const uint32_t kK[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
            0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
            0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
            0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
            0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
            0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
            0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
            0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
            0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
            0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
            0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
            0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
            0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
            0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
            0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
            0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
        static const uint8_t kS[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

        uint32_t m[16];
        for (int i = 0; i < 16; i++) {
            m[i] = static_cast<uint32_t>(block[i * 4]) |
                   (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
                   (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
                   (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
        }

        uint32_t a = a_, b = b_, c = c_, d = d_;
        for (int i = 0; i < 64; i++) {
            uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) & 15;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) & 15;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) & 15;
            }
            f += a + kK[i] + m[g];
            a = d;
            d = c;
            c = b;
            b += rotl(f, kS[i]);
        }
        a_ += a;
        b_ += b;
        c_ += c;
        d_ += d;
    }

    uint32_t a_ = 0;
    uint32_t b_ = 0;
    uint32_t c_ = 0;
    uint32_t d_ = 0;
    uint64_t bitLen_ = 0;
    uint8_t buf_[64] = {};
    size_t bufLen_ = 0;
};

// Hex MD5 of a string.
inline std::string md5Hex(const std::string &s) { return Md5::hash(s); }

} // namespace util
