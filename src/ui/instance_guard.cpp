#include "ui/instance_guard.hpp"

#include <QCryptographicHash>
#include <QLocalServer>
#include <QLocalSocket>

namespace tdm::ui {

InstanceGuard::InstanceGuard(QString key, QObject* parent) : QObject(parent), key_(std::move(key)) {}

QString InstanceGuard::defaultKey() {
    const QByteArray user = qEnvironmentVariable("USERDOMAIN").toUtf8() + '\\' + qEnvironmentVariable("USERNAME").toUtf8();
    return QStringLiteral("TeslaDM-") +
           QString::fromLatin1(QCryptographicHash::hash(user, QCryptographicHash::Sha256).toHex().left(16));
}

bool InstanceGuard::sendToPrimary(const QString& message) {
    QLocalSocket s;
    s.connectToServer(key_);
    if (!s.waitForConnected(500)) return false;
    s.write(message.toUtf8() + '\n');
    s.flush();
    s.waitForBytesWritten(2000);
    s.disconnectFromServer();
    return true;
}

bool InstanceGuard::tryBecomePrimary(const QString& message) {
    if (sendToPrimary(message)) return false;
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);  // only this user may connect
    if (!server_->listen(key_)) {
        // Lost a start-up race (or a stale endpoint): try the other instance once more.
        if (sendToPrimary(message)) return false;
        QLocalServer::removeServer(key_);
        if (!server_->listen(key_)) return true;  // run anyway; the guard is best effort
    }
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* c = server_->nextPendingConnection()) {
            auto drain = [this, c] {
                while (c->canReadLine()) emit messageReceived(QString::fromUtf8(c->readLine()).trimmed());
            };
            connect(c, &QLocalSocket::readyRead, this, drain);
            connect(c, &QLocalSocket::disconnected, this, [c, drain] {
                drain();  // data that arrived together with the disconnect
                c->deleteLater();
            });
            drain();  // data that arrived before the signals were connected
        }
    });
    return true;
}

}  // namespace tdm::ui
