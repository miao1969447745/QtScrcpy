#ifndef PHONEFEATURES_H
#define PHONEFEATURES_H
#include <QJsonObject>
namespace PhoneFeatures {
QJsonObject catalog();
bool validate(const QJsonObject &schema, QJsonObject &values, QString &error);
bool prepare(const QString &action, QJsonObject &values, QJsonObject &definition, QString &error);
}
#endif
