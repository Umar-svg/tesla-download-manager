#pragma once
// The only connection between the Qt UI and the Qt-free engine.
//
// Threading: engine observers run on engine threads. The bridge stores the
// latest snapshot per download in a mutex-protected buffer and keeps at most
// ONE queued flush in flight; the flush runs on the bridge's (UI) thread and
// emits one batch. However often the engine reports, the UI thread receives
// at most one event per flush cycle and only the newest state of each row.
//
// Lifetime: ~EngineBridge calls engine shutdown (which joins every engine
// thread) before any member is destroyed, so no observer can touch the
// bridge afterwards. A flush still queued at destruction is discarded by Qt
// because the bridge is its context object. Construct the bridge before the
// widgets that use it so it is destroyed after them.

#include <QList>
#include <QObject>

#include <map>
#include <memory>
#include <mutex>

#include "engine/engine.hpp"
#include "ui/download_model.hpp"

namespace tdm::ui {

class EngineBridge : public QObject {
    Q_OBJECT
public:
    // Throws (e.g. db::DbError when the database cannot be opened).
    explicit EngineBridge(engine::EngineOptions opts, QObject* parent = nullptr);
    ~EngineBridge() override;

    engine::DownloadEngine& engine() noexcept { return *engine_; }
    void shutdown();  // idempotent

    // Statistics for tests: snapshots received from the engine vs. Qt events delivered.
    std::uint64_t snapshotsReceived() const;
    std::uint64_t flushesDelivered() const;

signals:
    void snapshotsReady(const QList<tdm::engine::Snapshot>& batch);

private:
    void onEngineSnapshot(const engine::Snapshot& s);  // engine threads
    void flush();                                       // bridge thread

    std::unique_ptr<engine::Engine> engine_;
    mutable std::mutex m_;
    std::map<engine::DownloadId, engine::Snapshot> pending_;  // latest per id
    bool flush_queued_ = false;
    std::uint64_t received_ = 0;
    std::uint64_t flushes_ = 0;
};

}  // namespace tdm::ui
