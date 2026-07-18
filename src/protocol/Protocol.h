#pragma once

#include <QJsonObject>
#include <QString>

namespace gbr {

namespace ProtocolType {
inline constexpr auto Hello = "hello";
inline constexpr auto Welcome = "welcome";
inline constexpr auto Snapshot = "snapshot";
inline constexpr auto Delta = "delta";
inline constexpr auto Command = "command";
inline constexpr auto CommandResult = "commandResult";
inline constexpr auto Event = "event";
inline constexpr auto ResyncRequest = "resyncRequest";
inline constexpr auto Ping = "ping";
inline constexpr auto Pong = "pong";
inline constexpr auto Error = "error";
}

struct ProtocolLimits {
    static constexpr int Version = 1;
    static constexpr qsizetype MaxPacketBytes = 256 * 1024;
    static constexpr qsizetype MaxSnapshotBytes = 8 * 1024 * 1024;
    static constexpr int MaxStringLength = 4096;
    static constexpr int MaxActionLength = 64;
    static constexpr int MaxIdLength = 128;
};

struct ProtocolParseResult {
    bool accepted = false;
    QString code;
    QString message;
    QJsonObject envelope;
};

class ProtocolCodec {
public:
    static ProtocolParseResult parse(const QByteArray& bytes,
                                     qsizetype maxBytes = ProtocolLimits::MaxPacketBytes);
    static QByteArray encode(const QJsonObject& envelope);
    static QJsonObject envelope(const QString& type,
                                const QJsonObject& payload,
                                const QString& messageId,
                                const QString& sessionId = QString(),
                                const QString& clientId = QString(),
                                qint64 sequence = -1,
                                qint64 scenarioRevision = -1,
                                qint64 serverTick = -1);
    static bool isKnownType(const QString& type);
};

} // namespace gbr
