#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantMap>

namespace gbr {

namespace CommandAction {
inline constexpr auto AssignTarget = "assignTarget";
inline constexpr auto SetFlightPlan = "setFlightPlan";
inline constexpr auto EngageTarget = "engageTarget";
inline constexpr auto MoveTo = "moveTo";
inline constexpr auto Withdraw = "withdraw";
inline constexpr auto SetSpeed = "setSpeed";
inline constexpr auto Pursue = "pursue";
inline constexpr auto GuideAttack = "guideAttack";
inline constexpr auto SetSchedule = "setSchedule";
inline constexpr auto Halt = "halt";
}

namespace CommandCode {
inline constexpr auto Ok = "OK";
inline constexpr auto UnknownAction = "UNKNOWN_ACTION";
inline constexpr auto InvalidArgument = "INVALID_ARGUMENT";
inline constexpr auto UnitNotFound = "UNIT_NOT_FOUND";
inline constexpr auto UnitDead = "UNIT_DEAD";
inline constexpr auto UnitNotMovable = "UNIT_NOT_MOVABLE";
inline constexpr auto WrongUnitKind = "WRONG_UNIT_KIND";
inline constexpr auto TargetNotFound = "TARGET_NOT_FOUND";
inline constexpr auto TargetDead = "TARGET_DEAD";
inline constexpr auto FriendlyTarget = "FRIENDLY_TARGET";
inline constexpr auto NoCommandPost = "NO_COMMAND_POST";
inline constexpr auto OutOfBounds = "OUT_OF_BOUNDS";
inline constexpr auto PermissionDenied = "PERMISSION_DENIED";
inline constexpr auto RevisionMismatch = "REVISION_MISMATCH";
inline constexpr auto RateLimited = "RATE_LIMITED";
inline constexpr auto DuplicateCommand = "DUPLICATE_COMMAND";
inline constexpr auto NotAuthenticated = "NOT_AUTHENTICATED";
inline constexpr auto SimulationStateInvalid = "SIMULATION_STATE_INVALID";
inline constexpr auto InternalError = "INTERNAL_ERROR";
}

struct CommandResult {
    bool accepted = false;
    QString code = QString::fromLatin1(CommandCode::InternalError);
    QString message;
    qint64 appliedAtTick = -1;

    static CommandResult success(qint64 tick, const QString& message = QStringLiteral("命令已接受")) {
        return {true, QString::fromLatin1(CommandCode::Ok), message, tick};
    }

    static CommandResult rejected(const char* code, const QString& message, qint64 tick = -1) {
        return {false, QString::fromLatin1(code), message, tick};
    }

    QVariantMap toVariantMap() const {
        return {{QStringLiteral("accepted"), accepted},
                {QStringLiteral("code"), code},
                {QStringLiteral("message"), message},
                {QStringLiteral("appliedAtTick"), appliedAtTick}};
    }

    QJsonObject toJson() const {
        return QJsonObject::fromVariantMap(toVariantMap());
    }
};

} // namespace gbr
