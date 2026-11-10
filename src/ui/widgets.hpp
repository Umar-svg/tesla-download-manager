#pragma once
// Small reusable widgets: progress-bar delegate and segment map.

#include <QStyledItemDelegate>
#include <QWidget>

#include <optional>
#include <vector>

#include "engine/download_engine.hpp"

namespace tdm::ui {

// Draws DownloadTableModel::ProgressRole as a progress bar.
class ProgressDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
};

// Horizontal map of the selected download: downloaded ranges, active segments.
class SegmentBar : public QWidget {
    Q_OBJECT
public:
    explicit SegmentBar(QWidget* parent = nullptr);
    void setSnapshot(const engine::Snapshot* s);  // nullptr clears
    QSize sizeHint() const override { return {400, 22}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    std::optional<std::uint64_t> total_;
    std::uint64_t downloaded_ = 0;
    bool completed_ = false;
    std::vector<engine::SegmentView> segments_;
};

}  // namespace tdm::ui
