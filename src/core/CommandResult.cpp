#include "CommandResult.h"

namespace gbr {

QVariantMap CommandResult::toVariantMap() const {
    return {{QStringLiteral("accepted"), accepted},
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message}};
}

QJsonObject CommandResult::toJson() const {
    return {{QStringLiteral("accepted"), accepted},
            {QStringLiteral("code"), code},
            {QStringLiteral("message"), message}};
}

CommandResult CommandResult::ok(const QString& message) {
    return {true, QString::fromLatin1(CommandCode::Ok), message};
}

CommandResult CommandResult::reject(const QString& code, const QString& message) {
    return {false, code, message};
}

} // namespace gbr
