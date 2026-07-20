#pragma once

#include <QJsonObject>
#include <QString>

namespace gbr::Protocol {

inline constexpr int Version = 2;
inline constexpr int SchemaVersion = 1;
inline constexpr int MaxMessageBytes = 256 * 1024;
inline constexpr int MaxServerMessageBytes = 8 * 1024 * 1024;
inline constexpr int MaxIdentifierLength = 64;
inline constexpr int MaxActionLength = 64;
inline constexpr int MaxTokenLength = 4096;
inline constexpr int MaxChatLength = 500;
inline constexpr int MaxJsonDepth = 16;
inline constexpr int MaxJsonNodes = 262144;

struct ValidationResult {
    bool valid = false;
    QString code;
    QString message;

    static ValidationResult success();
    static ValidationResult failure(const QString& code, const QString& message);
};

bool isKnownClientMessageType(const QString& type);
bool isKnownServerMessageType(const QString& type);
ValidationResult validateClientEnvelope(const QJsonObject& envelope);
ValidationResult validateServerEnvelope(const QJsonObject& envelope);
ValidationResult validateClientPayload(const QString& type, const QJsonObject& payload);
ValidationResult validateServerPayload(const QString& type, const QJsonObject& payload);

QJsonObject makeClientEnvelope(const QString& type, const QString& messageId,
                               const QJsonObject& payload);
QJsonObject makeServerEnvelope(const QString& type, quint64 sequence,
                               const QJsonObject& payload);

} // namespace gbr::Protocol
