#include "phonefeatures.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <cmath>

QJsonObject PhoneFeatures::catalog()
{
    QFile file(":/phone-features.json");
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

static bool validValue(const QJsonObject &schema, QJsonValue &value, QString &error)
{
    const QString type = schema.value("type").toString();
    if (type == "boolean") {
        if (!value.isBool()) { error = "Expected boolean."; return false; }
    } else if (type == "integer") {
        const double number = value.toDouble();
        if (!value.isDouble() || !std::isfinite(number) || std::floor(number) != number
                || number < schema.value("minimum").toDouble() || number > schema.value("maximum").toDouble()) {
            error = "Integer is outside its allowed range."; return false;
        }
    } else if (type == "string") {
        const QString text = value.toString();
        if (!value.isString() || text.contains(QChar(0)) || text.size() < schema.value("minLength").toInt()
                || text.size() > schema.value("maxLength").toInt()) { error = "Invalid string length or NUL."; return false; }
        if (schema.contains("enum") && !schema.value("enum").toArray().contains(value)) { error = "Unknown string choice."; return false; }
    } else if (type == "object") {
        if (!value.isObject()) { error = "Expected parameter object."; return false; }
        QJsonObject object = value.toObject();
        if (!PhoneFeatures::validate(schema, object, error)) return false;
        value = object;
    } else if (type == "array") {
        if (!value.isArray()) { error = "Expected array."; return false; }
        QJsonArray array = value.toArray();
        if (array.size() < schema.value("minItems").toInt() || array.size() > schema.value("maxItems").toInt()) {
            error = "Invalid array length."; return false;
        }
        QSet<QString> unique;
        for (QJsonValue item : array) {
            if (!validValue(schema.value("items").toObject(), item, error)) return false;
            if (schema.value("uniqueItems").toBool() && unique.contains(item.toString())) { error = "Duplicate target."; return false; }
            unique.insert(item.toString());
        }
    } else { error = "Unsupported schema type."; return false; }
    return true;
}

bool PhoneFeatures::validate(const QJsonObject &schema, QJsonObject &values, QString &error)
{
    const QJsonObject properties = schema.value("properties").toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!properties.contains(it.key())) { error = "Unknown parameter: " + it.key(); return false; }
        QJsonValue value = it.value();
        if (!validValue(properties.value(it.key()).toObject(), value, error)) { error = it.key() + ": " + error; return false; }
        it.value() = value;
    }
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!values.contains(it.key()) && it.value().toObject().contains("default"))
            values.insert(it.key(), it.value().toObject().value("default"));
    }
    for (const QJsonValue &key : schema.value("required").toArray()) {
        if (!values.contains(key.toString())) { error = "Missing parameter: " + key.toString(); return false; }
    }
    return true;
}

bool PhoneFeatures::prepare(const QString &action, QJsonObject &values, QJsonObject &definition, QString &error)
{
    definition = catalog().value("actions").toObject().value(action).toObject();
    if (definition.isEmpty()) { error = "Unsupported phone action."; return false; }
    QJsonObject schema = definition.value("parameters").toObject();
    schema.insert("required", definition.value("required"));
    return validate(schema, values, error);
}
