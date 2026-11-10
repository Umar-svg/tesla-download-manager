#include "net/tls_context.hpp"

#include <openssl/ssl.h>
#include <openssl/x509.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace tdm::net {
namespace {

#ifdef _WIN32
// Copies the Windows trusted roots into OpenSSL's store so HTTPS verification
// matches what the rest of the system trusts.
void load_windows_roots(asio::ssl::context& ctx) {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) return;
    X509_STORE* x509_store = SSL_CTX_get_cert_store(ctx.native_handle());
    PCCERT_CONTEXT cert = nullptr;
    while ((cert = CertEnumCertificatesInStore(store, cert)) != nullptr) {
        const unsigned char* p = cert->pbCertEncoded;
        X509* x = d2i_X509(nullptr, &p, static_cast<long>(cert->cbCertEncoded));
        if (x) {
            X509_STORE_add_cert(x509_store, x);  // duplicates are ignored
            X509_free(x);
        }
    }
    CertCloseStore(store, 0);
}
#endif

}  // namespace

std::shared_ptr<asio::ssl::context> make_tls_context(const TlsOptions& opts) {
    auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
    ctx->set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                     asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                     asio::ssl::context::no_tlsv1_1);
    if (opts.use_system_store) {
#ifdef _WIN32
        load_windows_roots(*ctx);
#else
        ctx->set_default_verify_paths();
#endif
    }
    if (!opts.extra_ca_file.empty()) ctx->load_verify_file(opts.extra_ca_file);
    ctx->set_verify_mode(opts.verify_peer ? asio::ssl::verify_peer : asio::ssl::verify_none);
    return ctx;
}

}  // namespace tdm::net
