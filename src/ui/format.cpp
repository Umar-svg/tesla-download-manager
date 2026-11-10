#include "ui/format.hpp"

#include <QObject>
#include <QRegularExpression>
#include <QUrl>

#include <cmath>

namespace tdm::ui {

QString formatBytes(std::uint64_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1000.0 && u < 4) {
        v /= 1000.0;
        ++u;
    }
    if (u == 0) return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(v, 0, 'f', v < 10 ? 2 : (v < 100 ? 1 : 0)).arg(QLatin1String(units[u]));
}

QString formatSpeed(double bps) {
    if (bps < 1.0) return {};
    return formatBytes(static_cast<std::uint64_t>(bps)) + QStringLiteral("/s");
}

QString formatEta(std::optional<std::uint64_t> total, std::uint64_t downloaded, double bps) {
    if (!total || bps < 1.0 || downloaded >= *total) return {};
    const auto secs = static_cast<std::uint64_t>(std::ceil(static_cast<double>(*total - downloaded) / bps));
    const auto h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString stateName(db::State s) {
    switch (s) {
        case db::State::Queued: return QObject::tr("Queued");
        case db::State::Probing: return QObject::tr("Connecting");
        case db::State::Downloading: return QObject::tr("Downloading");
        case db::State::Waiting: return QObject::tr("Retrying soon");
        case db::State::Paused: return QObject::tr("Paused");
        case db::State::Verifying: return QObject::tr("Verifying");
        case db::State::Completed: return QObject::tr("Completed");
        case db::State::Failed: return QObject::tr("Failed");
        case db::State::Cancelled: return QObject::tr("Cancelled");
    }
    return {};
}

QString fromUtf8(const std::string& s) { return QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())); }

std::string toUtf8(const QString& s) {
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), static_cast<std::size_t>(b.size()));
}

std::optional<std::uint64_t> parseRate(const QString& text) {
    static const QRegularExpression re(QStringLiteral(R"(^\s*(\d+(?:\.\d+)?)\s*([kKmMgG]?)[bB]?(?:/s)?\s*$)"));
    const auto t = text.trimmed();
    if (t.isEmpty()) return 0;
    const auto m = re.match(t);
    if (!m.hasMatch()) return std::nullopt;
    double v = m.captured(1).toDouble();
    const QString unit = m.captured(2).toLower();
    if (unit == QLatin1String("k")) v *= 1000;
    else if (unit == QLatin1String("m")) v *= 1000 * 1000;
    else if (unit == QLatin1String("g")) v *= 1000.0 * 1000 * 1000;
    if (v > 1e13) return std::nullopt;
    return static_cast<std::uint64_t>(v);
}

bool looksLikeUrl(const QString& text) {
    const QUrl u(text.trimmed(), QUrl::StrictMode);
    return u.isValid() && (u.scheme() == QLatin1String("http") || u.scheme() == QLatin1String("https")) &&
           !u.host().isEmpty();
}

bool isSha256Hex(const QString& text) {
    static const QRegularExpression re(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return re.match(text.trimmed()).hasMatch();
}

}  // namespace tdm::ui
