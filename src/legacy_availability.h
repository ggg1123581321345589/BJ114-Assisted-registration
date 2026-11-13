#pragma once

// Read-only adapter for the official /mobile legacy response contract.
// Pure Qt Core functions: no network, cookies, font decoding or order submission.
// Reference: app.e9876633.js (SOURCE_STATUS, numeric resCode envelope),
// hos-calendar.5bf79a9b.js (source-list-item parity branches and duty groups).
#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace native { namespace legacy {

struct Context {
    QString hospitalCode;
    QString firstDepartmentCode;
    QString departmentCode;
    QString departmentName;
    // Supplied by the caller if a query timestamp is needed. No clock is read.
    QString queriedAt;
};

inline QString text(const char *utf8) { return QString::fromUtf8(utf8); }

inline bool validDate(const QString &date) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9]{4}-[0-9]{2}-[0-9]{2}$"));
    const QDate parsed = QDate::fromString(date, QStringLiteral("yyyy-MM-dd"));
    return pattern.match(date).hasMatch() && parsed.isValid()
        && parsed.toString(QStringLiteral("yyyy-MM-dd")) == date;
}

inline bool validCode(const QString &value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_.:-]{1,128}$"));
    return pattern.match(value).hasMatch();
}

inline bool validContext(const Context &context) {
    return validCode(context.hospitalCode) && validCode(context.firstDepartmentCode)
        && validCode(context.departmentCode) && context.departmentName.size() <= 256
        && context.queriedAt.size() <= 128;
}

inline QJsonObject failed(const QString &failure, const QString &message = QString()) {
    QJsonObject result{{QStringLiteral("status"), QStringLiteral("failed")},
                       {QStringLiteral("business_success"), false},
                       {QStringLiteral("failure"), failure},
                       {QStringLiteral("rows"), QJsonArray()},
                       {QStringLiteral("source_system"), QStringLiteral("legacy")},
                       {QStringLiteral("read_only"), true},
                       {QStringLiteral("booking_supported"), false}};
    if (!message.isEmpty()) result[QStringLiteral("official_message")] = message;
    return result;
}

inline QString officialMessage(const QJsonObject &envelope) {
    const auto msg = envelope.value(QStringLiteral("msg"));
    if (msg.isString()) return msg.toString().left(1000);
    const auto message = envelope.value(QStringLiteral("message"));
    return message.isString() ? message.toString().left(1000) : QString();
}

// JS's legacy f93c handler uses strict numeric resCode === 0, not "0"/false.
inline bool unwrap(const QJsonObject &envelope, QJsonValue &data, QJsonObject &error) {
    const auto code = envelope.value(QStringLiteral("resCode"));
    if (!code.isDouble() || !std::isfinite(code.toDouble())
        || std::floor(code.toDouble()) != code.toDouble()) {
        error = failed(QStringLiteral("legacy_envelope_invalid"), officialMessage(envelope));
        return false;
    }
    if (code.toDouble() != 0.0) {
        error = failed(QStringLiteral("legacy_business_response_rejected"), officialMessage(envelope));
        error[QStringLiteral("official_business_code")] = code;
        return false;
    }
    if (!envelope.contains(QStringLiteral("data"))) {
        error = failed(QStringLiteral("legacy_data_missing"), officialMessage(envelope));
        return false;
    }
    data = envelope.value(QStringLiteral("data"));
    return true;
}

inline QJsonObject baseRow(const Context &context, const QString &isoDate,
                          const QString &kind, const QString &endpoint) {
    QJsonObject row{{QStringLiteral("hospital_code"), context.hospitalCode},
                    {QStringLiteral("first_department_code"), context.firstDepartmentCode},
                    {QStringLiteral("department_code"), context.departmentCode},
                    {QStringLiteral("department_name"), context.departmentName},
                    {QStringLiteral("date"), QDate::fromString(isoDate, QStringLiteral("yyyy-MM-dd")).toString(QStringLiteral("yyyyMMdd"))},
                    // target must copy this server-format date, never an inferred ID.
                    {QStringLiteral("duty_date"), isoDate},
                    {QStringLiteral("doctor_name"), QString()},
                    {QStringLiteral("period"), QString()},
                    {QStringLiteral("remaining"), QJsonValue(QJsonValue::Null)},
                    {QStringLiteral("row_kind"), kind},
                    {QStringLiteral("source_system"), QStringLiteral("legacy")},
                    {QStringLiteral("source_endpoint"), endpoint},
                    {QStringLiteral("read_only"), true},
                    {QStringLiteral("booking_supported"), false}};
    if (!context.queriedAt.isEmpty()) row[QStringLiteral("queried_at")] = context.queriedAt;
    return row;
}

inline QString calendarStatusText(const QString &status) {
    if (status == QStringLiteral("AVAILABLE")) return text("有号");
    if (status == QStringLiteral("SOLD_OUT")) return text("约满");
    if (status == QStringLiteral("NO_INVENTORY")) return text("无号");
    if (status == QStringLiteral("CALLCENTER_AVAILABLE")) return text("有号");
    if (status == QStringLiteral("ALTERNATE")) return text("候补");
    if (status == QStringLiteral("WAIT_OPEN") || status == QStringLiteral("TOMORROW_OPEN"))
        return text("即将放号");
    return QString();
}

inline bool calendarHasDoctorList(const QString &status) {
    return status == QStringLiteral("AVAILABLE") || status == QStringLiteral("SOLD_OUT")
        || status == QStringLiteral("CALLCENTER_AVAILABLE") || status == QStringLiteral("ALTERNATE");
}

inline QJsonObject complete(const Context &context, const QJsonArray &rows,
                            const QString &endpoint) {
    return {{QStringLiteral("status"), QStringLiteral("complete")},
            {QStringLiteral("business_success"), true},
            {QStringLiteral("failure"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("hospital_code"), context.hospitalCode},
            {QStringLiteral("rows"), rows},
            {QStringLiteral("source_system"), QStringLiteral("legacy")},
            {QStringLiteral("source_endpoint"), endpoint},
            {QStringLiteral("read_only"), true},
            {QStringLiteral("booking_supported"), false}};
}

// Input is the full successful /mobile/product/list envelope.
// The entire response is rejected atomically on a malformed row or duplicate date.
inline QJsonObject parseCalendar(const QJsonObject &envelope, const Context &context) {
    if (!validContext(context)) return failed(QStringLiteral("legacy_context_invalid"));
    QJsonValue data;QJsonObject error;
    if (!unwrap(envelope, data, error)) return error;
    if (!data.isObject() || !data.toObject().value(QStringLiteral("calendars")).isArray())
        return failed(QStringLiteral("legacy_calendar_schema_invalid"));
    const auto calendars = data.toObject().value(QStringLiteral("calendars")).toArray();
    if (calendars.size() > 366) return failed(QStringLiteral("legacy_calendar_oversize"));
    QJsonArray rows;QSet<QString> seen;
    for (const auto &value : calendars) {
        if (!value.isObject()) return failed(QStringLiteral("legacy_calendar_row_invalid"));
        const auto source = value.toObject();
        const auto dateValue = source.value(QStringLiteral("dutyDate"));
        const auto statusValue = source.value(QStringLiteral("status"));
        if (!dateValue.isString() || !validDate(dateValue.toString()))
            return failed(QStringLiteral("legacy_calendar_date_invalid"));
        const QString date = dateValue.toString();
        if (seen.contains(date)) return failed(QStringLiteral("legacy_calendar_date_ambiguous"));
        seen.insert(date);
        if (!statusValue.isString() || calendarStatusText(statusValue.toString()).isEmpty())
            return failed(QStringLiteral("legacy_calendar_status_invalid"));
        const auto week = source.value(QStringLiteral("weekDesc"));
        if (!week.isUndefined() && !week.isNull() && !week.isString())
            return failed(QStringLiteral("legacy_calendar_week_invalid"));
        auto row = baseRow(context, date, QStringLiteral("department_calendar"), QStringLiteral("/mobile/product/list"));
        row[QStringLiteral("status")] = calendarStatusText(statusValue.toString());
        row[QStringLiteral("legacy_status")] = statusValue;
        row[QStringLiteral("doctor_list_visible")] = calendarHasDoctorList(statusValue.toString());
        if (week.isString()) row[QStringLiteral("week_description")] = week;
        row[QStringLiteral("legacy_source")] = source;
        rows.append(row);
    }
    return complete(context, rows, QStringLiteral("/mobile/product/list"));
}

// IDs in optional display metadata are copied verbatim and never synthesized.
inline bool optionalText(const QJsonObject &object, const QString &key, int maximum = 512) {
    const auto value = object.value(key);
    return value.isUndefined() || value.isNull() || (value.isString() && value.toString().size() <= maximum);
}

// Parity flags can be JSON integers or decimal strings, matching JS % coercion
// on known well-formed inputs. Missing/null/booleans/fractional/negative inputs
// are rejected rather than being coerced into a false "web available" state.
inline bool parity(const QJsonValue &value, bool &odd) {
    if (value.isDouble()) {
        const double n = value.toDouble();
        if (!std::isfinite(n) || n < 0 || n > 9007199254740991.0 || std::floor(n) != n) return false;
        odd = std::fmod(n, 2.0) != 0.0;
        return true;
    }
    if (value.isString()) {
        const QString n = value.toString();
        static const QRegularExpression pattern(QStringLiteral("^[0-9]{1,16}$"));
        if (!pattern.match(n).hasMatch()) return false;
        bool ok = false;const qulonglong integer = n.toULongLong(&ok);
        if (!ok || integer > 9007199254740991ULL) return false;
        odd = ((integer & 1ULL) != 0);
        return true;
    }
    return false;
}

inline QString dutyText(const QString &dutyCode) {
    if (dutyCode == QStringLiteral("MORNING")) return text("上午");
    if (dutyCode == QStringLiteral("AFTERNOON")) return text("下午");
    if (dutyCode == QStringLiteral("EVENING")) return text("晚上");
    if (dutyCode == QStringLiteral("ALL")) return text("全天");
    return QString();
}

inline bool validMagicValue(const QJsonValue &value) {
    // ncode/fcode are MagicNumber text rendered with the supplied custom font.
    // Retain the original value; do not decode glyphs or infer an integer count.
    if (value.isUndefined() || value.isNull()) return true;
    if (value.isString()) return value.toString().size() <= 4096;
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0;
}

// Input is /mobile/product/detail's full envelope; target is the exact selected
// calendar dutyDate. Source rows have no trustworthy intrinsic date, so both
// request binding and (if returned) explicit dutyDate are validated.
// One row per official doctor entry is emitted; period[] is retained unchanged.
inline QJsonObject parseDetail(const QJsonObject &envelope, const Context &context,
                              const QString &target) {
    if (!validContext(context)) return failed(QStringLiteral("legacy_context_invalid"));
    if (!validDate(target)) return failed(QStringLiteral("legacy_target_date_invalid"));
    QJsonValue data;QJsonObject error;
    if (!unwrap(envelope, data, error)) return error;
    if (!data.isArray()) return failed(QStringLiteral("legacy_detail_schema_invalid"));
    const auto groups = data.toArray();
    if (groups.size() > 32) return failed(QStringLiteral("legacy_detail_oversize"));
    QJsonArray rows;int doctorCount = 0;
    for (const auto &value : groups) {
        if (!value.isObject()) return failed(QStringLiteral("legacy_detail_group_invalid"));
        const auto group = value.toObject();
        const auto duty = group.value(QStringLiteral("dutyCode"));
        if (!duty.isString() || dutyText(duty.toString()).isEmpty()
            || !group.value(QStringLiteral("detail")).isArray())
            return failed(QStringLiteral("legacy_detail_group_invalid"));
        const auto groupDate = group.value(QStringLiteral("dutyDate"));
        if (!groupDate.isUndefined() && (!groupDate.isString() || groupDate.toString() != target))
            return failed(QStringLiteral("legacy_detail_date_mismatch"));
        if (!optionalText(group, QStringLiteral("dutyImgUrl"), 2048))
            return failed(QStringLiteral("legacy_detail_font_invalid"));
        for (const auto &entry : group.value(QStringLiteral("detail")).toArray()) {
            if (++doctorCount > 10000) return failed(QStringLiteral("legacy_detail_oversize"));
            if (!entry.isObject()) return failed(QStringLiteral("legacy_doctor_schema_invalid"));
            const auto source = entry.toObject();
            const auto sourceDate = source.value(QStringLiteral("dutyDate"));
            if (!sourceDate.isUndefined() && (!sourceDate.isString() || sourceDate.toString() != target))
                return failed(QStringLiteral("legacy_detail_date_mismatch"));
            for (const auto &key : {QStringLiteral("doctorName"), QStringLiteral("doctorTitleName"),
                                   QStringLiteral("doctorCode"), QStringLiteral("uniqProductKey")})
                if (!optionalText(source, key)) return failed(QStringLiteral("legacy_doctor_text_invalid"));
            const QString name = source.value(QStringLiteral("doctorName")).toString();
            const QString title = source.value(QStringLiteral("doctorTitleName")).toString();
            // The page conditionally renders name, but always renders title.
            // Use the real title as the label if name is absent; never "医生"+ID.
            const QString label = name.isEmpty() ? title : name;
            if (label.isEmpty()) return failed(QStringLiteral("legacy_doctor_label_missing"));
            bool web = false, phone = false, alternate = false;
            if (!parity(source.value(QStringLiteral("wnumber")), web)
                || !parity(source.value(QStringLiteral("znumber")), phone)
                || !parity(source.value(QStringLiteral("hnumber")), alternate))
                return failed(QStringLiteral("legacy_doctor_parity_invalid"));
            if (!source.value(QStringLiteral("period")).isArray())
                return failed(QStringLiteral("legacy_period_schema_invalid"));
            const auto periods = source.value(QStringLiteral("period")).toArray();
            if (periods.size() > 1000) return failed(QStringLiteral("legacy_period_oversize"));
            if (!validMagicValue(source.value(QStringLiteral("ncode")))
                || !validMagicValue(source.value(QStringLiteral("fcode"))))
                return failed(QStringLiteral("legacy_magic_value_invalid"));
            for (const auto &partValue : periods) {
                if (!partValue.isObject()) return failed(QStringLiteral("legacy_period_schema_invalid"));
                const auto part = partValue.toObject();
                const auto view = part.value(QStringLiteral("dutyTimeView"));
                if (!view.isString() || view.toString().isEmpty() || view.toString().size() > 256
                    || !validMagicValue(part.value(QStringLiteral("ncode"))))
                    return failed(QStringLiteral("legacy_period_schema_invalid"));
            }
            auto row = baseRow(context, target, QStringLiteral("legacy_doctor"), QStringLiteral("/mobile/product/detail"));
            row[QStringLiteral("doctor_name")] = label;
            row[QStringLiteral("doctor_name_raw")] = source.value(QStringLiteral("doctorName")).isString() ? source.value(QStringLiteral("doctorName")) : QJsonValue(QString());
            row[QStringLiteral("doctor_title")] = title;
            row[QStringLiteral("registration_fee")] = QJsonValue(QJsonValue::Null);
            row[QStringLiteral("registration_fee_status")] = source.value(QStringLiteral("fcode")).isNull() || source.value(QStringLiteral("fcode")).isUndefined() ? QStringLiteral("not_provided") : QStringLiteral("official_encoded");
            row[QStringLiteral("doctor_label_source")] = name.isEmpty() ? QStringLiteral("doctorTitleName") : QStringLiteral("doctorName");
            row[QStringLiteral("period")] = dutyText(duty.toString());
            row[QStringLiteral("legacy_duty_code")] = duty;
            row[QStringLiteral("status")] = web ? text("有号") : phone ? text("电话预约") : alternate ? text("候补") : text("约满");
            row[QStringLiteral("legacy_channel")] = web ? QStringLiteral("web") : phone ? QStringLiteral("phone") : alternate ? QStringLiteral("alternate") : QStringLiteral("sold_out");
            row[QStringLiteral("legacy_parity")] = QJsonObject{{QStringLiteral("web_odd"), web}, {QStringLiteral("phone_odd"), phone}, {QStringLiteral("alternate_odd"), alternate}};
            row[QStringLiteral("legacy_periods")] = periods;
            row[QStringLiteral("legacy_source")] = source;
            row[QStringLiteral("remaining_evidence")] = QStringLiteral("not_decoded_magic_number_font");
            if (group.value(QStringLiteral("dutyImgUrl")).isString()) row[QStringLiteral("legacy_font_url")] = group.value(QStringLiteral("dutyImgUrl"));
            // Deliberately do not assign product_status or booking_payload.
            rows.append(row);
        }
    }
    auto result = complete(context, rows, QStringLiteral("/mobile/product/detail"));
    result[QStringLiteral("selected_duty_date")] = target;
    result[QStringLiteral("doctor_count")] = doctorCount;
    return result;
}

}} // namespace native::legacy
