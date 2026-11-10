#pragma once

#include <asio/ssl.hpp>

#include <memory>
#include <string>

namespace tdm::net {

struct TlsOptions {
    bool verify_peer = true;          // certificate chain + host name
    bool use_system_store = true;     // Windows "ROOT" certificate store
    std::string extra_ca_file;        // PEM, e.g. a test CA
};

// TLS 1.2+ client context. Throws std::system_error on failure.
std::shared_ptr<asio::ssl::context> make_tls_context(const TlsOptions& opts);

}  // namespace tdm::net
