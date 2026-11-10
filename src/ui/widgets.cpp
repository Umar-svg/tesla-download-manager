#include "ui/widgets.hpp"

#include <QApplication>
#include <QPainter>
#include <QStyleOptionProgressBar>

#include "ui/download_model.hpp"

namespace tdm::ui {

void ProgressDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const {
    const double v = index.data(DownloadTableModel::ProgressRole).toDouble();
    if (v < 0) return QStyledItemDelegate::paint(p, opt, index);  // unknown size: plain text
    QStyleOptionProgressBar bar;
    bar.rect = opt.rect.adjusted(2, 3, -2, -3);
    bar.minimum = 0;
    bar.maximum = 1000;
    bar.progress = static_cast<int>(v * 1000.0);
    bar.text = index.data(Qt::DisplayRole).toString();
    bar.textVisible = true;
    bar.textAlignment = Qt::AlignCenter;
    bar.state = opt.state | QStyle::State_Horizontal;
    QApplication::style()->drawControl(QStyle::CE_ProgressBar, &bar, p);
}

SegmentBar::SegmentBar(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(18);
    setToolTip(tr("Downloaded ranges (green) and active connections (blue)"));
}

void SegmentBar::setSnapshot(const engine::Snapshot* s) {
    if (!s) {
        total_.reset();
        segments_.clear();
        downloaded_ = 0;
        completed_ = false;
    } else {
        total_ = s->total;
        segments_ = s->segments;
        downloaded_ = s->downloaded;
        completed_ = s->state == db::State::Completed;
    }
    update();
}

void SegmentBar::paintEvent(QPaintEvent*) {
    QPainter p(this);
    const QRect r = rect().adjusted(0, 0, -1, -1);
    p.fillRect(r, palette().color(QPalette::Base));
    p.setPen(palette().color(QPalette::Mid));
    p.drawRect(r);
    if (!total_ || *total_ == 0) {
        if (completed_) p.fillRect(r.adjusted(1, 1, 0, 0), QColor(76, 175, 80));
        return;
    }
    const double w = r.width() - 1;
    auto x_of = [&](std::uint64_t off) {
        return r.left() + 1 + static_cast<int>(w * static_cast<double>(off) / static_cast<double>(*total_));
    };
    if (completed_ || segments_.empty()) {
        const auto end = completed_ ? *total_ : std::min(downloaded_, *total_);
        p.fillRect(QRect(QPoint(x_of(0), r.top() + 1), QPoint(x_of(end), r.bottom())), QColor(76, 175, 80));
        return;
    }
    for (const auto& s : segments_) {
        if (s.pos > s.begin)
            p.fillRect(QRect(QPoint(x_of(s.begin), r.top() + 1), QPoint(x_of(s.pos), r.bottom())), QColor(76, 175, 80));
        if (s.active && s.pos < s.end) {  // a thin marker where a connection is writing
            const int x = x_of(s.pos);
            p.fillRect(QRect(x, r.top() + 1, 2, r.height() - 1), QColor(33, 150, 243));
        }
    }
}

}  // namespace tdm::ui
