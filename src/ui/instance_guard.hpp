#pragma once
// Single-instance guard over a per-user local socket (a named pipe on Windows).
// The first instance listens; a later launch connects, hands over its message
// (e.g. a URL from the command line) and exits.

#include <QObject>
#include <QString>

class QLocalServer;

namespace tdm::ui {

class InstanceGuard : public QObject {
    Q_OBJECT
public:
    explicit InstanceGuard(QString key, QObject* parent = nullptr);

    // true: this process is the primary instance and now listens.
    // false: another instance is running and received `message`.
    bool tryBecomePrimary(const QString& message);

    static QString defaultKey();  // per user, so different accounts don't collide

signals:
    void messageReceived(const QString& message);  // "" = just bring the window forward

private:
    bool sendToPrimary(const QString& message);
    QString key_;
    QLocalServer* server_ = nullptr;
};

}  // namespace tdm::ui
