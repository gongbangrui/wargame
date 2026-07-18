#include "Protocol.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <cmath>

namespace gbr {

namespace {

ProtocolParseResult reject(const QString& code, const QString& message) {
    return {false, code, message, {}};
}

bool validOptionalString(const QJsonObject& object, const QString& key, int maxLength) {
    if (!object.contains(key)) return true;
    return object.value(key).isString()
        && object.value(key).toString().size() <= maxLength;
}

bool validOptionalInteger(const QJsonObject& object, const QString& key) {
    if (!object.contains(key)) return true;
    if (!object.value(key).isDouble()) return false;
    const double value = object.value(key).toDouble();
    return std::isfinite(value) && std::floor(value) == value && value >= -1.0;
}

} // namespace

bool ProtocolCodec::isKnownType(const QString& type) {
    static const QSet<QString> types{
        QString::fromLatin1(ProtocolType::Hello),
        QString::fromLatin1(ProtocolType::Welcome),
        QString::fromLatin1(ProtocolType::Snapshot),
        QString::fromLatin1(ProtocolType::Delta),
        QString::fromLatin1(ProtocolType::Command),
        QString::fromLatin1(ProtocolType::CommandResult),
        QString::fromLatin1(ProtocolType::Event),
        QString::fromLatin1(ProtocolType::ResyncRequest),
        QString::fromLatin1(ProtocolType::Ping),
        QString::fromLatin1(ProtocolType::Pong),
        QString::fromLatin1(ProtocolType::Error),
    };
    return types.contains(type);
}

ProtocolParseResult ProtocolCodec::parse(const QByteArray& bytes, qsizetype maxBytes) {
    if (bytes.isEmpty()) return reject(QStringLiteral("EMPTY_PACKET"), QStringLiteral("消息不能为空"));
    if (bytes.size() > maxBytes) return reject(QStringLiteral("PACKET_TOO_LARGE"), QStringLiteral("消息超过大小限制"));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reject(QStringLiteral("MALFORMED_JSON"), QStringLiteral("JSON 格式无效"));
    }
    const QJsonObject object = document.object();
    static const QSet<QString> allowedFields{
        QStringLiteral("protocolVersion"), QStringLiteral("type"),
        QStringLiteral("sessionId"), QStringLiteral("clientId"),
        QStringLiteral("messageId"), QStringLiteral("sequence"),
        QStringLiteral("scenarioRevision"), QStringLiteral("serverTick"),
        QStringLiteral("sentAt"), QStringLiteral("payload"),
    };
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowedFields.contains(it.key())) {
            return reject(QStringLiteral("UNKNOWN_FIELD"),
                          QStringLiteral("不支持的协议字段: %1").arg(it.key()));
        }
    }
    if (!object.value(QStringLiteral("protocolVersion")).isDouble()
        || object.value(QStringLiteral("protocolVersion")).toInt(-1) != ProtocolLimits::Version) {
        return reject(QStringLiteral("UNSUPPORTED_VERSION"), QStringLiteral("协议版本不受支持"));
    }
    const QString type = object.value(QStringLiteral("type")).toString();
    if (type.isEmpty() || type.size() > ProtocolLimits::MaxActionLength || !isKnownType(type)) {
        return reject(QStringLiteral("UNKNOWN_TYPE"), QStringLiteral("消息类型不受支持"));
    }
    if (!validOptionalString(object, QStringLiteral("messageId"), ProtocolLimits::MaxIdLength)
        || object.value(QStringLiteral("messageId")).toString().isEmpty()
        || !validOptionalString(object, QStringLiteral("sessionId"), ProtocolLimits::MaxIdLength)
        || !validOptionalString(object, QStringLiteral("clientId"), ProtocolLimits::MaxIdLength)
        || !validOptionalString(object, QStringLiteral("sentAt"), 64)
        || !validOptionalInteger(object, QStringLiteral("sequence"))
        || !validOptionalInteger(object, QStringLiteral("scenarioRevision"))
        || !validOptionalInteger(object, QStringLiteral("serverTick"))) {
        return reject(QStringLiteral("INVALID_ENVELOPE"), QStringLiteral("协议包字段类型或长度无效"));
    }
    if (!object.value(QStringLiteral("payload")).isObject()) {
        return reject(QStringLiteral("INVALID_PAYLOAD"), QStringLiteral("payload 必须是对象"));
    }
    return {true, QStringLiteral("OK"), {}, object};
}

QByteArray ProtocolCodec::encode(const QJsonObject& envelope) {
    return QJsonDocument(envelope).toJson(QJsonDocument::Compact);
}

QJsonObject ProtocolCodec::envelope(const QString& type,
                                    const QJsonObject& payload,
                                    const QString& messageId,
                                    const QString& sessionId,
                                    const QString& clientId,
                                    qint64 sequence,
                                    qint64 scenarioRevision,
                                    qint64 serverTick) {
    QJsonObject object{
        {QStringLiteral("protocolVersion"), ProtocolLimits::Version},
        {QStringLiteral("type"), type},
        {QStringLiteral("messageId"), messageId},
        {QStringLiteral("sentAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("payload"), payload},
    };
    if (!sessionId.isEmpty()) object.insert(QStringLiteral("sessionId"), sessionId);
    if (!clientId.isEmpty()) object.insert(QStringLiteral("clientId"), clientId);
    if (sequence >= 0) object.insert(QStringLiteral("sequence"), sequence);
    if (scenarioRevision >= 0) object.insert(QStringLiteral("scenarioRevision"), scenarioRevision);
    if (serverTick >= 0) object.insert(QStringLiteral("serverTick"), serverTick);
    return object;
}

} // namespace gbr
