#ifndef DBMS_AUTH_JWT_HPP
#define DBMS_AUTH_JWT_HPP

#include "crypto.hpp"
#include "../json.hpp"
#include <chrono>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace dbms::auth {

using json = nlohmann::json;

inline constexpr int64_t kJwtDefaultTtlSeconds = 30LL * 24 * 3600; // 30 суток

class JwtAuth {
public:
    explicit JwtAuth(const std::string& secret_file) : secret_file_(secret_file) {
        load_or_create_secret();
    }

    std::string issue_token(const std::string& username, int64_t ttl_seconds = kJwtDefaultTtlSeconds) const {
        const auto now = std::chrono::system_clock::now();
        const auto iat = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
        const int64_t exp = iat + ttl_seconds;

        json header = {{"alg", "HS256"}, {"typ", "JWT"}};
        json payload = {{"sub", username}, {"iat", iat}, {"exp", exp}};

        const std::string h = base64url_encode(header.dump());
        const std::string p = base64url_encode(payload.dump());
        const std::string signing_input = h + "." + p;
        const auto sig = hmac_sha256(secret_, signing_input);
        const std::string sig_b64 = base64url_encode(std::string(reinterpret_cast<const char*>(sig.data()), sig.size()));
        return signing_input + "." + sig_b64;
    }

    std::optional<std::string> verify_token(const std::string& token) const {
        const size_t dot1 = token.find('.');
        const size_t dot2 = token.find('.', dot1 == std::string::npos ? 0 : dot1 + 1);
        if (dot1 == std::string::npos || dot2 == std::string::npos) {
            return std::nullopt;
        }

        const std::string part0 = token.substr(0, dot1);
        const std::string part1 = token.substr(dot1 + 1, dot2 - dot1 - 1);
        const std::string part2 = token.substr(dot2 + 1);
        const std::string signing_input = part0 + "." + part1;

        const auto expected = hmac_sha256(secret_, signing_input);
        const std::string expected_b64 =
            base64url_encode(std::string(reinterpret_cast<const char*>(expected.data()), expected.size()));

        if (!secure_compare(expected_b64, part2)) {
            return std::nullopt;
        }

        const std::string payload_json = base64url_decode(part1);
        json payload;
        try {
            payload = json::parse(payload_json);
        } catch (...) {
            return std::nullopt;
        }

        if (!payload.contains("sub") || !payload.contains("exp")) {
            return std::nullopt;
        }

        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        const int64_t exp = payload["exp"].get<int64_t>();
        if (exp <= now) {
            return std::nullopt;
        }

        return payload["sub"].get<std::string>();
    }

    const std::string& secret() const { return secret_; }

private:
    std::string secret_file_;
    std::string secret_;

    void load_or_create_secret() {
        std::ifstream in(secret_file_);
        if (in) {
            std::getline(in, secret_);
            if (!secret_.empty()) {
                return;
            }
        }

        auto raw = random_bytes(32);
        secret_ = bytes_to_hex(raw.data(), raw.size());

        std::ofstream out(secret_file_, std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Cannot write JWT secret: " + secret_file_);
        }
        out << secret_;
    }
};

} // namespace dbms::auth

#endif
