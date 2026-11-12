#pragma once
#include "legacy_availability.h"

namespace native { namespace legacy {
inline bool departmentText(const QJsonValue &value) {
    if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 300) return false;
    for (const QChar c : value.toString()) if (c.unicode() < 32) return false;
    return true;
}

// Observed response: resCode:0,data:{count,list:[{code,name,subList:[{code,name,dept1Code}]}]}.
// Use actual list entries, never count as a leaf quantity. Keep the existing
// four-key Service cache projection and reject the whole response on ambiguity.
inline QJsonObject parseDepartments(const QJsonObject &envelope, const QString &hospitalCode) {
    if (!validCode(hospitalCode)) return failed(QStringLiteral("legacy_hospital_code_invalid"));
    QJsonValue data; QJsonObject error;
    if (!unwrap(envelope, data, error)) return error;
    if (!data.isObject() || !data.toObject().value(QStringLiteral("list")).isArray())
        return failed(QStringLiteral("legacy_department_schema_invalid"));
    const auto groups = data.toObject().value(QStringLiteral("list")).toArray();
    if (groups.size() > 5000) return failed(QStringLiteral("legacy_department_oversize"));
    QJsonArray departments; QSet<QString> parents, pairs;
    for (const auto &groupValue : groups) {
        if (!groupValue.isObject()) return failed(QStringLiteral("legacy_department_group_invalid"));
        const auto group = groupValue.toObject();
        const auto first = group.value(QStringLiteral("code"));
        const auto firstName = group.value(QStringLiteral("name"));
        if (!first.isString() || !validCode(first.toString()) || !departmentText(firstName)
            || !group.value(QStringLiteral("subList")).isArray())
            return failed(QStringLiteral("legacy_department_group_invalid"));
        if (parents.contains(first.toString())) return failed(QStringLiteral("legacy_department_parent_ambiguous"));
        parents.insert(first.toString());
        const auto leaves = group.value(QStringLiteral("subList")).toArray();
        if (leaves.size() > 5000 || departments.size() + leaves.size() > 5000)
            return failed(QStringLiteral("legacy_department_oversize"));
        for (const auto &leafValue : leaves) {
            if (!leafValue.isObject()) return failed(QStringLiteral("legacy_department_leaf_invalid"));
            const auto leaf = leafValue.toObject();
            const auto code = leaf.value(QStringLiteral("code"));
            if (!code.isString() || !validCode(code.toString()) || !departmentText(leaf.value(QStringLiteral("name"))))
                return failed(QStringLiteral("legacy_department_leaf_invalid"));
            if (leaf.value(QStringLiteral("dept1Code")) != first)
                return failed(QStringLiteral("legacy_department_parent_mismatch"));
            const auto descendants = leaf.value(QStringLiteral("subList"));
            if (!descendants.isUndefined() && (!descendants.isArray() || !descendants.toArray().isEmpty()))
                return failed(QStringLiteral("legacy_department_depth_unsupported"));
            const auto key = first.toString() + QChar(0x1f) + code.toString();
            if (pairs.contains(key)) return failed(QStringLiteral("legacy_department_leaf_ambiguous"));
            pairs.insert(key);
            departments.append(QJsonObject{{QStringLiteral("code"), code},
                {QStringLiteral("name"), leaf.value(QStringLiteral("name"))},
                {QStringLiteral("first_department_code"), first},
                {QStringLiteral("first_department_name"), firstName}});
        }
    }
    Context context; context.hospitalCode = hospitalCode;
    auto result = complete(context, QJsonArray(), QStringLiteral("/mobile/department/hos/list"));
    result[QStringLiteral("departments")] = departments;
    result[QStringLiteral("department_count")] = departments.size();
    result[QStringLiteral("first_department_count")] = groups.size();
    return result;
}
}}
