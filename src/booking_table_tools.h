#ifndef BOOKING_TABLE_TOOLS_H
#define BOOKING_TABLE_TOOLS_H

#include <QtWidgets>
#include <cmath>
#include <functional>

// Keep JSON booking payloads in Qt::UserRole; this separate role holds an
// optional custom value for sorting a formatted cell.
class BookingTableItem : public QTableWidgetItem {
public:
    enum { SortValueRole = Qt::UserRole + 20 };
    explicit BookingTableItem(const QString &text, int column = -1)
        : QTableWidgetItem(text) { Q_UNUSED(column); }
    QTableWidgetItem *clone() const override { return new BookingTableItem(*this); }
    bool operator<(const QTableWidgetItem &other) const override {
        const auto left = sortKey(*this), right = sortKey(other);
        // A category ordering keeps comparison transitive with mixed numbers,
        // unknown labels and blanks. Plain pairwise numeric comparison does not.
        if (left.kind != right.kind) return left.kind < right.kind;
        if (left.kind == 0 && left.number != right.number)
            return left.number < right.number;
        return QString::localeAwareCompare(left.text, right.text) < 0;
    }
private:
    struct SortKey { int kind; double number; QString text; };
    static SortKey sortKey(const QTableWidgetItem &item) {
        const QVariant value = item.data(SortValueRole);
        const QString text = value.isValid() ? value.toString() : item.text();
        const QString trimmed = text.trimmed();
        static const QRegularExpression pattern(
            QStringLiteral("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)$"));
        bool valid = false;
        const double number = trimmed.toDouble(&valid);
        if (pattern.match(trimmed).hasMatch() && valid && std::isfinite(number))
            return {0, number, text};
        return {trimmed.isEmpty() ? 2 : 1, 0.0, text};
    }
};

// Local-only header controls; no Q_OBJECT or moc step is required. Construct
// after assigning header labels. Disable sorting while replacing table rows,
// restore it afterwards, then refresh() to apply filters to the new records.
class BookingTableTools : public QObject {
public:
    explicit BookingTableTools(QTableWidget *table) : QObject(table), table_(table) {
        setObjectName(QStringLiteral("bookingTableTools"));
        captureNewHeaders();
        auto *header = table_->horizontalHeader();
        header->setSectionsClickable(true);
        header->setSortIndicatorShown(true);
        header->setSortIndicator(-1, Qt::AscendingOrder);
        table_->setSortingEnabled(true);
        header->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(header, &QWidget::customContextMenuRequested, this,
                         [this](const QPoint &point) { showMenu(point); });
        // Hiding is positional in QTableWidget; reapply after sorting so the
        // same records, rather than the old row positions, remain hidden.
        QObject::connect(table_->model(), &QAbstractItemModel::layoutChanged,
                         this, [this] { applyFilters(); });
        updateHeaders();
    }
    std::function<void()> filtersChanged;
    void setFilter(int column, const QString &text) {
        captureNewHeaders();
        if (column < 0 || column >= filters_.size()) return;
        filters_[column] = text.trimmed();
        applyFilters();
    }
    QString filter(int column) const {
        return column >= 0 && column < filters_.size() ? filters_[column] : QString();
    }
    void clearFilters() {
        for (QString &value : filters_) value.clear();
        globalSearch_.clear();
        applyFilters();
    }
    void globalSearch(const QString &text) {
        globalSearch_ = text.trimmed();
        applyFilters();
    }
    QString globalSearchText() const { return globalSearch_; }
    void refresh() { captureNewHeaders(); applyFilters(); }
    void applyFilters() {
        captureNewHeaders();
        bool hideSelection = false;
        for (int row = 0; row < table_->rowCount(); ++row) {
            bool matches = true;
            bool globalMatches = globalSearch_.isEmpty();
            for (int column = 0; column < table_->columnCount(); ++column) {
                const auto *item = table_->item(row, column);
                const QString text = item ? item->text() : QString();
                if (!filters_[column].isEmpty() &&
                    !text.contains(filters_[column], Qt::CaseInsensitive))
                    matches = false;
                if (!globalMatches && text.contains(globalSearch_, Qt::CaseInsensitive))
                    globalMatches = true;
            }
            const bool hidden = !matches || !globalMatches;
            table_->setRowHidden(row, hidden);
            if (hidden && row == table_->currentRow()) hideSelection = true;
            if (hidden) {
                for (int column = 0; column < table_->columnCount(); ++column) {
                    const auto *item = table_->item(row, column);
                    if (item && item->isSelected()) hideSelection = true;
                }
            }
        }
        // A filtered-out slot cannot remain an invisible booking target.
        if (hideSelection) {
            table_->clearSelection();
            table_->setCurrentItem(nullptr);
        }
        updateHeaders();
        if (filtersChanged) filtersChanged();
    }
private:
    QTableWidget *table_;
    QStringList names_;
    QVector<QString> filters_;
    QString globalSearch_;
    void captureNewHeaders() {
        while (names_.size() < table_->columnCount()) {
            const int column = names_.size();
            const auto *item = table_->horizontalHeaderItem(column);
            names_.append(item ? item->text() : QString::number(column + 1));
        }
        filters_.resize(table_->columnCount());
    }
    void updateHeaders() {
        for (int column = 0; column < table_->columnCount(); ++column) {
            auto *item = table_->horizontalHeaderItem(column);
            if (!item) {
                item = new QTableWidgetItem;
                table_->setHorizontalHeaderItem(column, item);
            }
            const bool filtered = !filters_[column].isEmpty();
            item->setText(names_[column] + (filtered ? QString::fromUtf8(" [筛选]") : QString()));
            item->setToolTip(QString::fromUtf8("左键：升序 / 降序排序；右键：筛选此列。") +
                (filtered ? QString::fromUtf8("\n当前包含：%1").arg(filters_[column]) : QString()));
        }
    }
    void showMenu(const QPoint &point) {
        const int column = table_->horizontalHeader()->logicalIndexAt(point);
        if (column < 0 || column >= table_->columnCount()) return;
        QMenu menu(table_);
        auto *ascending = menu.addAction(QString::fromUtf8("升序排序"));
        auto *descending = menu.addAction(QString::fromUtf8("降序排序"));
        menu.addSeparator();
        auto *contains = menu.addAction(QString::fromUtf8("筛选：包含文字…"));
        auto *clearColumn = menu.addAction(QString::fromUtf8("清除此列筛选"));
        clearColumn->setEnabled(!filter(column).isEmpty());
        auto *clearAll = menu.addAction(QString::fromUtf8("清除全部筛选与搜索"));
        const auto *chosen = menu.exec(table_->horizontalHeader()->mapToGlobal(point));
        if (chosen == ascending) table_->sortItems(column, Qt::AscendingOrder);
        else if (chosen == descending) table_->sortItems(column, Qt::DescendingOrder);
        else if (chosen == contains) {
            bool accepted = false;
            const QString text = QInputDialog::getText(table_, QString::fromUtf8("筛选 %1").arg(names_[column]),
                QString::fromUtf8("只显示包含以下文字的行（忽略大小写；多列条件同时满足）："),
                QLineEdit::Normal, filter(column), &accepted);
            if (accepted) setFilter(column, text);
        } else if (chosen == clearColumn) setFilter(column, QString());
        else if (chosen == clearAll) clearFilters();
    }
};
#endif // BOOKING_TABLE_TOOLS_H
