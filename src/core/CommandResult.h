#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantMap>

namespace gbr {

struct CommandResult {
    bool accepted = false;
    QString code;
    QString message;

    QVariantMap toVariantMap() const;
    QJsonObject toJson() const;

    static CommandResult ok(const QString& message = QStringLiteral("命令已接收"));
    static CommandResult reject(const QString& code, const QString& message);
};

namespace CommandCode {
inline constexpr auto Ok = "OK";
inline constexpr auto UnknownAction = "UNKNOWN_ACTION";
inline constexpr auto InvalidArgument = "INVALID_ARGUMENT";
inline constexpr auto UnitNotFound = "UNIT_NOT_FOUND";
inline constexpr auto UnitDestroyed = "UNIT_DESTROYED";
inline constexpr auto UnitNotMovable = "UNIT_NOT_MOVABLE";
inline constexpr auto InvalidUnitKind = "INVALID_UNIT_KIND";
inline constexpr auto InvalidTarget = "INVALID_TARGET";
inline constexpr auto CommandPostUnavailable = "COMMAND_POST_UNAVAILABLE";
} // namespace CommandCode

} // namespace gbr
