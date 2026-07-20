#pragma once

#include <QJsonObject>
#include <QString>

namespace gbr::StateDelta {

bool canCreate(const QJsonObject& base, const QJsonObject& current);
QJsonObject create(const QJsonObject& base, const QJsonObject& current);
bool apply(QJsonObject& state, const QJsonObject& delta, QString* error = nullptr);

} // namespace gbr::StateDelta
