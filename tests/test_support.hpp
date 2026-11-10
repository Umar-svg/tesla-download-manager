#pragma once
// Shared helpers for integration tests: the Python probe server process and SHA-256.

#include <openssl/evp.h>
#include <windows.h>

#include <cstdlib>
#include <span>
#include <string>

#include "core/http_util.hpp"

namespace tdm::test {

class ProbeServer {
public:
    explicit ProbeServer(bool tls) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE out_write = nullptr;
        if (!CreatePipe(&out_read_, &out_write, &sa, 0)) return;
        SetHandleInformation(out_read_, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        std::string cmd = std::string("\"") + TDM_PYTHON + "\" -I \"" + TDM_PROBE_SERVER + "\" --port 0";
        if (tls) cmd += std::string(" --tls-cert \"") + TDM_TEST_CERT + "\" --tls-key \"" + TDM_TEST_KEY + "\"";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out_write;
        si.hStdError = nul;  // tracebacks from rejected TLS handshakes must not fill a pipe
        si.hStdInput = nullptr;
        const BOOL ok = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                       nullptr, &si, &pi_);
        CloseHandle(out_write);
        CloseHandle(nul);
        if (!ok) return;

        std::string line;  // "probe server on http://127.0.0.1:PORT"
        char c;
        DWORD n = 0;
        while (ReadFile(out_read_, &c, 1, &n, nullptr) && n == 1 && c != '\n') line += c;
        const auto colon = line.rfind(':');
        if (colon != std::string::npos) port_ = std::atoi(line.c_str() + colon + 1);
        base_ = std::string(tls ? "https" : "http") + "://127.0.0.1:" + std::to_string(port_);
    }
    ~ProbeServer() {
        if (pi_.hProcess) {
            TerminateProcess(pi_.hProcess, 0);
            WaitForSingleObject(pi_.hProcess, 5000);
            CloseHandle(pi_.hProcess);
            CloseHandle(pi_.hThread);
        }
        if (out_read_) CloseHandle(out_read_);
    }
    bool ok() const { return port_ > 0; }
    core::Url url(const std::string& path) const { return *core::parse_url(base_ + path); }

private:
    PROCESS_INFORMATION pi_{};
    HANDLE out_read_ = nullptr;
    int port_ = 0;
    std::string base_;
};

inline std::string hex(const unsigned char* p, unsigned n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (unsigned i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

struct Sha256 {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    Sha256() { EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr); }
    ~Sha256() { EVP_MD_CTX_free(ctx); }
    void update(std::span<const std::byte> s) { EVP_DigestUpdate(ctx, s.data(), s.size()); }
    std::string final_hex() {
        unsigned char out[32];
        unsigned n = 0;
        EVP_DigestFinal_ex(ctx, out, &n);
        return hex(out, n);
    }
};


}  // namespace tdm::test
