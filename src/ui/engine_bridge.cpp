#include "ui/engine_bridge.hpp"

#include <QMetaObject>

namespace tdm::ui {

EngineBridge::EngineBridge(engine::EngineOptions opts, QObject* parent) : QObject(parent) {
    qRegisterMetaType<tdm::engine::Snapshot>("tdm::engine::Snapshot");
    qRegisterMetaType<QList<tdm::engine::Snapshot>>("QList<tdm::engine::Snapshot>");
    engine_ = std::make_unique<engine::Engine>(std::move(opts));
    engine_->subscribe([this](const engine::Snapshot& s) { onEngineSnapshot(s); });
}

EngineBridge::~EngineBridge() {
    shutdown();  // joins engine threads: no observer call can follow
}

void EngineBridge::shutdown() {
    if (engine_) engine_->shutdown();
}

void EngineBridge::onEngineSnapshot(const engine::Snapshot& s) {
    bool post = false;
    {
        std::lock_guard lk(m_);
        ++received_;
        pending_[s.id] = s;  // newest wins; a `removed` snapshot also replaces older ones
        if (!flush_queued_) {
            flush_queued_ = true;
            post = true;
        }
    }
    if (post) QMetaObject::invokeMethod(this, &EngineBridge::flush, Qt::QueuedConnection);
}

void EngineBridge::flush() {
    QList<engine::Snapshot> batch;
    {
        std::lock_guard lk(m_);
        flush_queued_ = false;
        ++flushes_;
        batch.reserve(static_cast<qsizetype>(pending_.size()));
        for (auto& [id, s] : pending_) batch.push_back(std::move(s));
        pending_.clear();
    }
    if (!batch.isEmpty()) emit snapshotsReady(batch);
}

std::uint64_t EngineBridge::snapshotsReceived() const {
    std::lock_guard lk(m_);
    return received_;
}

std::uint64_t EngineBridge::flushesDelivered() const {
    std::lock_guard lk(m_);
    return flushes_;
}

}  // namespace tdm::ui
