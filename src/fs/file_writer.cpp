#include "fs/file_writer.hpp"

#include <openssl/evp.h>
#include <windows.h>
#include <winioctl.h>

#include <condition_variable>
#include <deque>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <vector>

namespace tdm::fs {
namespace {

std::error_code last_error() { return {static_cast<int>(GetLastError()), std::system_category()}; }

class WinFileWriter final : public FileWriter {
public:
    explicit WinFileWriter(std::size_t budget) : budget_(budget ? budget : (64u << 20)) {}
    ~WinFileWriter() override { (void)close(); }

    std::error_code open(const std::filesystem::path& part, std::optional<std::uint64_t> size,
                         bool keep_existing) override {
        part_ = part;
        h_ = CreateFileW(part.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         keep_existing ? OPEN_EXISTING : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            h_ = nullptr;
            return last_error();
        }
        if (size) {
            if (keep_existing) {
                LARGE_INTEGER cur{};
                if (!GetFileSizeEx(h_, &cur) || static_cast<std::uint64_t>(cur.QuadPart) != *size) {
                    CloseHandle(h_);
                    h_ = nullptr;
                    return std::make_error_code(std::errc::file_too_large);  // size differs: not our part file
                }
            } else {
                DWORD ret = 0;
                // Sparse: unwritten ranges cost no disk space and no zero-filling. Not fatal if unsupported (FAT).
                DeviceIoControl(h_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
                FILE_END_OF_FILE_INFO eof{};
                eof.EndOfFile.QuadPart = static_cast<LONGLONG>(*size);
                if (!SetFileInformationByHandle(h_, FileEndOfFileInfo, &eof, sizeof(eof))) {
                    auto ec = last_error();
                    CloseHandle(h_);
                    h_ = nullptr;
                    return ec;
                }
            }
        }
        stop_ = false;
        thread_ = std::thread([this] { run(); });
        return {};
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override {
        if (data.empty()) return;
        std::lock_guard lk(m_);
        if (err_ || !h_) return;
        q_.push_back(Item{offset, std::vector<std::byte>(data.begin(), data.end())});
        queued_ += data.size();
        ++enqueued_seq_;
        cv_work_.notify_one();
    }

    bool request_room(std::function<void()> on_room) override {
        std::lock_guard lk(m_);
        if (queued_ <= budget_ || !h_) return true;
        waiters_.push_back(std::move(on_room));
        return false;
    }

    std::uint64_t queued_bytes() const override {
        std::lock_guard lk(m_);
        return queued_;
    }

    std::error_code error() const override {
        std::lock_guard lk(m_);
        return err_;
    }

    std::error_code flush() override {
        std::unique_lock lk(m_);
        if (!h_) return err_;
        // Wait only for writes enqueued before this call (FIFO), not for an empty
        // queue: with connections still writing, the queue may never be empty.
        const std::uint64_t target = enqueued_seq_;
        cv_idle_.wait(lk, [this, target] { return completed_seq_ >= target || err_; });
        lk.unlock();  // do not block enqueuers during the (slow) flush
        std::shared_lock hl(handle_mu_);  // close()/discard() cannot close the handle meanwhile
        const HANDLE h = h_;
        const bool ok = !h || FlushFileBuffers(h);
        const auto flush_err = ok ? std::error_code{} : last_error();
        hl.unlock();
        lk.lock();
        if (flush_err && !err_) err_ = flush_err;
        return err_;
    }

    std::error_code close() override {
        if (!h_) return error();
        stop_thread();  // the writer thread drains the whole queue before exiting
        std::error_code ec;
        {
            std::unique_lock hl(handle_mu_);
            const bool ok = FlushFileBuffers(h_);
            const auto flush_err = ok ? std::error_code{} : last_error();
            CloseHandle(h_);
            h_ = nullptr;
            std::lock_guard lk(m_);
            if (flush_err && !err_) err_ = flush_err;
            ec = err_;
        }
        wake_waiters();
        return ec;
    }

    std::error_code finalize(const std::filesystem::path& final_path, std::filesystem::path& actual) override {
        if (auto ec = close()) return ec;
        constexpr int kMaxTransientWaits = 30;  // 25+50+100+200 ms, then 400 ms each: about 10.7 s in total
        int transient_waits = 0;
        for (int attempt = 0; attempt < 1000; ++attempt) {
            auto candidate = unique_path(final_path);
            if (MoveFileExW(part_.c_str(), candidate.c_str(), MOVEFILE_WRITE_THROUGH)) {
                actual = candidate;
                return {};
            }
            const DWORD e = GetLastError();
            if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) continue;  // lost a name race: next free name
            // Another handle without FILE_SHARE_DELETE (antivirus scan, indexer,
            // previewer) blocks the rename. These are transient: back off and
            // retry for up to ~10 s instead of failing a complete download.
            if ((e == ERROR_SHARING_VIOLATION || e == ERROR_ACCESS_DENIED || e == ERROR_LOCK_VIOLATION) &&
                transient_waits < kMaxTransientWaits) {
                Sleep(std::min<DWORD>(25u << std::min(transient_waits, 4), 400));  // 25, 50, 100, 200, 400 ms...
                ++transient_waits;
                --attempt;  // does not consume a name candidate
                continue;
            }
            return {static_cast<int>(e), std::system_category()};
        }
        return std::make_error_code(std::errc::file_exists);
    }

    void discard() override {
        {
            std::lock_guard lk(m_);
            q_.clear();
            queued_ = 0;
            completed_seq_ = enqueued_seq_;
        }
        cv_idle_.notify_all();
        stop_thread();
        {
            std::unique_lock hl(handle_mu_);
            if (h_) {
                CloseHandle(h_);
                h_ = nullptr;
            }
        }
        wake_waiters();
        if (!part_.empty()) DeleteFileW(part_.c_str());
    }

    const std::filesystem::path& part_path() const override { return part_; }

private:
    struct Item {
        std::uint64_t offset;
        std::vector<std::byte> data;
    };

    void run() {
        for (;;) {
            Item item;
            {
                std::unique_lock lk(m_);
                cv_work_.wait(lk, [this] { return stop_ || !q_.empty(); });
                if (q_.empty()) return;  // stop_ and drained
                item = std::move(q_.front());
                q_.pop_front();
                writing_ = true;
            }
            std::error_code ec;
            std::size_t done = 0;
            while (done < item.data.size()) {
                OVERLAPPED ov{};
                const std::uint64_t off = item.offset + done;
                ov.Offset = static_cast<DWORD>(off & 0xFFFFFFFFu);
                ov.OffsetHigh = static_cast<DWORD>(off >> 32);
                const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(item.data.size() - done, 1u << 30));
                DWORD wrote = 0;
                if (!WriteFile(h_, item.data.data() + done, chunk, &wrote, &ov) || wrote == 0) {
                    ec = last_error();
                    break;
                }
                done += wrote;
            }
            std::vector<std::function<void()>> wake;
            {
                std::lock_guard lk(m_);
                if (ec && !err_) err_ = ec;
                if (err_) {  // stop accepting work; drop what is queued so nobody waits forever
                    q_.clear();
                    queued_ = 0;
                    completed_seq_ = enqueued_seq_;
                } else {
                    queued_ -= item.data.size();
                    ++completed_seq_;  // FIFO: every item up to this sequence number is written
                }
                writing_ = false;
                if (queued_ <= budget_ / 2) wake.swap(waiters_);
                cv_idle_.notify_all();
            }
            for (auto& f : wake) f();
        }
    }

    void stop_thread() {
        {
            std::lock_guard lk(m_);
            stop_ = true;
        }
        cv_work_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void wake_waiters() {
        std::vector<std::function<void()>> wake;
        {
            std::lock_guard lk(m_);
            wake.swap(waiters_);
        }
        for (auto& f : wake) f();
    }

    const std::size_t budget_;
    std::filesystem::path part_;
    std::atomic<HANDLE> h_{nullptr};
    std::shared_mutex handle_mu_;  // shared: FlushFileBuffers; exclusive: CloseHandle
    std::thread thread_;
    mutable std::mutex m_;
    std::condition_variable cv_work_, cv_idle_;
    std::deque<Item> q_;
    std::uint64_t queued_ = 0;
    bool writing_ = false;
    std::uint64_t enqueued_seq_ = 0;   // items ever enqueued
    std::uint64_t completed_seq_ = 0;  // items written (or dropped after an error/discard)
    bool stop_ = false;
    std::error_code err_;
    std::vector<std::function<void()>> waiters_;
};

}  // namespace

std::unique_ptr<FileWriter> make_file_writer(std::size_t budget_bytes) {
    return std::make_unique<WinFileWriter>(budget_bytes);
}

std::filesystem::path unique_path(const std::filesystem::path& desired) {
    std::error_code ec;
    if (!std::filesystem::exists(desired, ec)) return desired;
    const auto dir = desired.parent_path();
    const auto stem = desired.stem().wstring();
    const auto ext = desired.extension().wstring();
    for (int i = 1; i < 10000; ++i) {
        auto p = dir / (stem + L" (" + std::to_wstring(i) + L")" + ext);
        if (!std::filesystem::exists(p, ec)) return p;
    }
    return desired;
}

std::string sha256_file(const std::filesystem::path& path, std::error_code& ec) {
    ec.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        ec = last_error();
        return {};
    }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    std::vector<unsigned char> buf(1u << 20);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            ec = last_error();
            break;
        }
        if (got == 0) break;
        EVP_DigestUpdate(ctx, buf.data(), got);
    }
    CloseHandle(h);
    unsigned char out[EVP_MAX_MD_SIZE];
    unsigned n = 0;
    EVP_DigestFinal_ex(ctx, out, &n);
    EVP_MD_CTX_free(ctx);
    if (ec) return {};
    static const char* digits = "0123456789abcdef";
    std::string hex;
    for (unsigned i = 0; i < n; ++i) {
        hex += digits[out[i] >> 4];
        hex += digits[out[i] & 15];
    }
    return hex;
}

}  // namespace tdm::fs
