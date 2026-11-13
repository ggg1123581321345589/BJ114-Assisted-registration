#pragma once

#include <QtWidgets>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <functional>

// Painted buttons preserve readable dates and doctor labels regardless of the
// generic QPushButton padding configured by the surrounding application.
class BookingCalendarDayButton final : public QAbstractButton {
public:
    QDate date;
    bool inDisplayedMonth = true;
    bool selected = false;
    QStringList doctors;

    explicit BookingCalendarDayButton(QWidget *parent = nullptr) : QAbstractButton(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(48, 42);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
    }
    QSize sizeHint() const override { return QSize(100, 54); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF box = QRectF(rect()).adjusted(2, 2, -2, -2);
        const bool today = date == QDate::currentDate();
        QColor fill = selected ? QColor(35, 86, 232)
                               : (underMouse() ? QColor(237, 243, 255) : QColor(Qt::white));
        if (!isEnabled() && !selected) fill = QColor(245, 245, 245);
        painter.setBrush(fill);
        painter.setPen(selected ? QPen(QColor(16, 54, 167), 2)
                                : (today ? QPen(QColor(79, 107, 255), 2) : QPen(Qt::NoPen)));
        painter.drawRoundedRect(box, 7, 7);
        QColor ink = date.dayOfWeek() >= 6 ? QColor(235, 65, 69) : QColor(40, 48, 62);
        if (!inDisplayedMonth) ink = QColor(180, 184, 192);
        if (selected) ink = Qt::white;
        if (!isEnabled() && !selected) ink = QColor(170, 170, 170);
        painter.setPen(ink);
        QFont numberFont = font();
        numberFont.setPointSizeF(13);
        numberFont.setBold(today || selected);
        painter.setFont(numberFont);
        painter.drawText(QRect(4, 5, width() - (selected ? 22 : 8), 23), Qt::AlignCenter, QString::number(date.day()));
        if (selected) {
            // Selection is painted independently of keyboard focus or hover.
            painter.setPen(QPen(Qt::white, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath check;
            check.moveTo(width() - 17, 13);
            check.lineTo(width() - 14, 16);
            check.lineTo(width() - 9, 10);
            painter.drawPath(check);
        }
        QFont doctorFont = font();
        doctorFont.setPointSizeF(8.5);
        painter.setFont(doctorFont);
        painter.setPen(selected ? QColor(Qt::white) : (inDisplayedMonth ? QColor(70, 82, 106) : QColor(165, 170, 180)));
        const QFontMetrics metrics(doctorFont);
        const int lineHeight = metrics.height();
        const int lineCount = qMin(2, qMax(0, height() - 30) / qMax(1, lineHeight));
        QStringList shown;
        if (lineCount == 1 && !doctors.isEmpty()) shown << doctors.join(QString::fromUtf8("、"));
        else if (lineCount >= 2 && !doctors.isEmpty()) {
            shown << doctors.first();
            if (doctors.size() > 1) shown << doctors.mid(1).join(QString::fromUtf8("、"));
        }
        for (int index = 0; index < shown.size(); ++index)
            painter.drawText(QRect(5, 29 + index * lineHeight, width() - 10, lineHeight),
                             Qt::AlignCenter, metrics.elidedText(shown[index], Qt::ElideRight, qMax(1, width() - 10)));
        if (hasFocus()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(selected ? QColor(Qt::white) : QColor(79, 107, 255), 1, Qt::DotLine));
            painter.drawRoundedRect(box.adjusted(3, 3, -3, -3), 5, 5);
        }
    }
};

// Header-only Qt5 calendar: no Q_OBJECT or extra moc step. Changes are reported
// through selectionChanged; the owner remains responsible for query scheduling.
class BookingCalendar final : public QWidget {
public:
    std::function<void()> selectionChanged;

    explicit BookingCalendar(QWidget *parent = nullptr) : QWidget(parent),
        month_(QDate(QDate::currentDate().year(), QDate::currentDate().month(), 1)) {
        setObjectName(QStringLiteral("bookingCalendar"));
        setMinimumWidth(390);
        setStyleSheet(QStringLiteral(
            "QWidget#bookingCalendar{background:#f8fbff;border:1px solid #dce7f4;border-radius:10px;}"
            "QComboBox#calendarYear,QComboBox#calendarMonth{background:white;border:1px solid #dce1ea;border-radius:6px;padding:4px 8px;}"
            "QPushButton#calendarPrevious,QPushButton#calendarNext,QPushButton#calendarToday,QPushButton#calendarClear{background:white;border:1px solid #dce1ea;border-radius:6px;padding:4px 9px;}"
            "QLabel#calendarSelectionSummary{color:#52627b;font-size:11px;}"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 7);
        layout->setSpacing(4);
        auto *navigation = new QHBoxLayout;
        auto *previous = new QPushButton(QString::fromUtf8("‹"), this);
        previous->setObjectName(QStringLiteral("calendarPrevious"));
        previous->setToolTip(QString::fromUtf8("上个月"));
        previous->setMaximumWidth(35);
        auto *next = new QPushButton(QString::fromUtf8("›"), this);
        next->setObjectName(QStringLiteral("calendarNext"));
        next->setToolTip(QString::fromUtf8("下个月"));
        next->setMaximumWidth(35);
        year_ = new QComboBox(this);
        year_->setObjectName(QStringLiteral("calendarYear"));
        for (int year = 2000; year <= 2100; ++year)
            year_->addItem(QString::number(year) + QString::fromUtf8("年"), year);
        monthBox_ = new QComboBox(this);
        monthBox_->setObjectName(QStringLiteral("calendarMonth"));
        for (int month = 1; month <= 12; ++month)
            monthBox_->addItem(QString::number(month) + QString::fromUtf8("月"), month);
        auto *today = new QPushButton(QString::fromUtf8("今天"), this);
        today->setObjectName(QStringLiteral("calendarToday"));
        auto *clear = new QPushButton(QString::fromUtf8("清空"), this);
        clear->setObjectName(QStringLiteral("calendarClear"));
        navigation->addWidget(previous);
        navigation->addWidget(year_);
        navigation->addWidget(monthBox_);
        navigation->addWidget(next);
        navigation->addStretch();
        navigation->addWidget(today);
        navigation->addWidget(clear);
        layout->addLayout(navigation);
        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(2);
        grid->setVerticalSpacing(1);
        const QStringList week = {QString::fromUtf8("一"), QString::fromUtf8("二"), QString::fromUtf8("三"),
                                  QString::fromUtf8("四"), QString::fromUtf8("五"), QString::fromUtf8("六"), QString::fromUtf8("日")};
        for (int column = 0; column < 7; ++column) {
            auto *label = new QLabel(week[column], this);
            label->setAlignment(Qt::AlignCenter);
            label->setStyleSheet(column >= 5 ? QStringLiteral("color:#eb4145;") : QStringLiteral("color:#627084;"));
            grid->addWidget(label, 0, column);
            grid->setColumnStretch(column, 1);
        }
        for (int index = 0; index < 42; ++index) {
            auto *day = new BookingCalendarDayButton(this);
            days_.append(day);
            grid->addWidget(day, index / 7 + 1, index % 7);
            connect(day, &QAbstractButton::clicked, this, [this, day] {
                if (selected_.contains(day->date)) selected_.remove(day->date);
                else selected_.insert(day->date);
                refreshDaySelection(day);
                refreshSummary();
                if (selectionChanged) selectionChanged();
            });
        }
        for (int row = 1; row <= 6; ++row) grid->setRowStretch(row, 1);
        layout->addLayout(grid, 1);
        summary_ = new QLabel(this);
        summary_->setObjectName(QStringLiteral("calendarSelectionSummary"));
        summary_->setTextFormat(Qt::PlainText);
        summary_->setWordWrap(true);
        layout->addWidget(summary_);
        connect(previous, &QPushButton::clicked, this, [this] { setDisplayedMonth(month_.addMonths(-1)); });
        connect(next, &QPushButton::clicked, this, [this] { setDisplayedMonth(month_.addMonths(1)); });
        connect(year_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            setDisplayedMonth(QDate(year_->currentData().toInt(), month_.month(), 1));
        });
        connect(monthBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            setDisplayedMonth(QDate(month_.year(), monthBox_->currentData().toInt(), 1));
        });
        connect(today, &QPushButton::clicked, this, [this] {
            const QDate date = QDate::currentDate();
            const bool changed = !selected_.contains(date);
            selected_.insert(date);
            setDisplayedMonth(date);
            if (changed && selectionChanged) selectionChanged();
        });
        connect(clear, &QPushButton::clicked, this, [this] { setSelectedDates({}); });
        selected_.insert(QDate::currentDate());
        refresh();
    }

    QList<QDate> selectedDates() const {
        QList<QDate> values = selected_.values();
        std::sort(values.begin(), values.end());
        return values;
    }
    void setSelectedDates(const QList<QDate> &dates, bool notify = true) {
        QSet<QDate> values;
        for (const auto &date : dates) if (date.isValid()) values.insert(date);
        const bool changed = values != selected_;
        if (!changed) return;
        selected_ = values;
        refreshSelection();
        refreshSummary();
        if (notify && selectionChanged) selectionChanged();
    }

    // Replacement semantics prevent another hospital's names leaking into the
    // calendar. Pass accumulated multi-day rows to retain earlier annotations.
    void setDoctorRows(const QJsonArray &rows) {
        QMap<QDate, QStringList> values;
        for (const auto &value : rows) {
            const auto row = value.toObject();
            const QDate date = QDate::fromString(row.value(QStringLiteral("date")).toString(), QStringLiteral("yyyyMMdd"));
            const QString name = row.value(QStringLiteral("doctor_name")).toString().trimmed();
            if (date.isValid() && !name.isEmpty() && !values[date].contains(name)) values[date].append(name);
        }
        for (auto iterator = values.begin(); iterator != values.end(); ++iterator)
            iterator.value().sort(Qt::CaseInsensitive);
        if (values == doctors_) return;
        doctors_ = values;
        for (auto *day : days_) refreshDayDoctors(day, doctors_.value(day->date));
    }
    void clearDoctors() { setDoctorRows(QJsonArray{}); }
    QDate displayedMonth() const { return month_; }
    void setDisplayedMonth(QDate date) {
        if (!date.isValid()) return;
        const QDate displayed(date.year(), date.month(), 1);
        if (month_ == displayed) {
            refreshSelection();
            refreshSummary();
            return;
        }
        month_ = displayed;
        refresh();
    }

private:
    QDate month_;
    QSet<QDate> selected_;
    QMap<QDate, QStringList> doctors_;
    QComboBox *year_ = nullptr, *monthBox_ = nullptr;
    QLabel *summary_ = nullptr;
    QList<BookingCalendarDayButton *> days_;

    void refresh() {
        const QSignalBlocker yearSignals(year_), monthSignals(monthBox_);
        int yearIndex = year_->findData(month_.year());
        if (yearIndex < 0) {
            year_->addItem(QString::number(month_.year()) + QString::fromUtf8("年"), month_.year());
            yearIndex = year_->count() - 1;
        }
        year_->setCurrentIndex(yearIndex);
        monthBox_->setCurrentIndex(month_.month() - 1);
        const QDate start = month_.addDays(1 - month_.dayOfWeek());
        for (int index = 0; index < days_.size(); ++index) {
            auto *day = days_[index];
            day->date = start.addDays(index);
            day->inDisplayedMonth = day->date.year() == month_.year() && day->date.month() == month_.month();
            const QString key = day->date.toString(QStringLiteral("yyyyMMdd"));
            day->setObjectName(QStringLiteral("calendarDay_") + key);
            day->setProperty("calendarDate", key);
            refreshDaySelection(day, true);
            refreshDayDoctors(day, doctors_.value(day->date), true);
        }
        refreshSummary();
    }

    void refreshDaySelection(BookingCalendarDayButton *day, bool force = false) {
        const bool selected = selected_.contains(day->date);
        if (!force && day->selected == selected) return;
        day->selected = selected;
        day->setProperty("calendarSelected", selected);
        day->setAccessibleDescription(selected ? QString::fromUtf8("已选择；点击取消") : QString::fromUtf8("未选择；点击选择"));
        day->update();
    }

    void refreshSelection() {
        for (auto *day : days_) refreshDaySelection(day);
    }

    void refreshDayDoctors(BookingCalendarDayButton *day, const QStringList &names, bool force = false) {
        if (!force && day->doctors == names) return;
        day->doctors = names;
        day->setProperty("doctorNames", names);
        day->setText(day->date.toString(QStringLiteral("yyyy-MM-dd")) + QStringLiteral(" ") + names.join(QString::fromUtf8("、")));
        day->setAccessibleName(day->text());
        day->setToolTip(day->date.toString(QStringLiteral("yyyy-MM-dd")) + QString::fromUtf8("\n") +
                        (names.isEmpty() ? QString::fromUtf8("暂无已查询的医生信息") : names.join(QString::fromUtf8("、"))) +
                        QString::fromUtf8("\n点击选择或取消，可跨月多选"));
        day->update();
    }

    void refreshSummary() {
        QStringList dates;
        for (const auto &date : selectedDates()) dates.append(date.toString(QStringLiteral("MM-dd")));
        const QString full = dates.join(QString::fromUtf8("、"));
        const QString preview = dates.size() > 7 ? dates.mid(0, 7).join(QString::fromUtf8("、")) + QString::fromUtf8(" …") : full;
        summary_->setText(selected_.isEmpty() ? QString::fromUtf8("点击日期可多选；医生信息在查询后显示。")
                         : QString::fromUtf8("已选 %1 天：%2；点击日期可取消").arg(selected_.size()).arg(preview));
        QStringList fullDates;
        for (const auto &date : selectedDates()) fullDates.append(date.toString(QStringLiteral("yyyy-MM-dd")));
        summary_->setToolTip(fullDates.join(QString::fromUtf8("、")));
    }
};
