#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QtGlobal>

namespace gbr::Protocol {

inline constexpr int Version = 3;
inline constexpr int SchemaVersion = 2;
inline constexpr int MaxMessageBytes = 256 * 1024;
inline constexpr int MaxServerMessageBytes = 8 * 1024 * 1024;
inline constexpr int MaxIdentifierLength = 64;
inline constexpr int MaxActionLength = 64;
inline constexpr int MaxTokenLength = 4096;
inline constexpr int MaxChatLength = 500;
inline constexpr int MaxMapLabelLength = 128;
inline constexpr int MaxJsonDepth = 16;
inline constexpr int MaxJsonNodes = 262144;
inline constexpr int MaxRoomNameLength = 96;
inline constexpr int MaxSeatIdLength = 64;
inline constexpr int MaxDdsTicketLength = 128;
inline constexpr qint64 MaxSafeJsonInteger = 9007199254740991LL;

struct ValidationResult {
    bool valid = false;
    QString code;
    QString message;

    static ValidationResult success();
    static ValidationResult failure(const QString& code, const QString& message);
};

struct SeatProjection {
    QString seatId;
    QString seatType;
    QString side;
    int slot = 0;
    int capacity = 0;
    bool occupied = false;
    QString displayName;
    bool ready = false;
    bool connected = false;
    bool deployed = false;
    bool pendingTransfer = false;
    bool redeployRequested = false;
    QString unitId;
    QString selectedTemplate;
    QString unitName;
};

struct RoomLifecycleProjection {
    QString phase = QStringLiteral("preparing");
    QString roomId;
    QString roomName;
    QString roomStatus;
    bool redReady = false;
    bool blueReady = false;
    bool running = false;
    bool readyForSim = false;
    QString cpIssues;
    double simTime = 0.0;
    double speed = 1.0;
    qint64 scenarioRevision = 0;
    qint64 stateRevision = 0;
    QList<SeatProjection> seats;
};

struct SnapshotProjection {
    RoomLifecycleProjection lifecycle;
};

struct SeatDirectoryProjection {
    QString roomId;
    QString yourSeatId;
    QList<SeatProjection> seats;
};

struct DeploymentPromptProjection {
    QString unitId;
    QString seatId;
    QString targetSeatId;
    QString message;
};

struct IntelShareProjection {
    QString senderSeatId;
    QString targetId;
    QString sharedAt;
    QString note;
};

struct CommandResultProjection {
    QString commandId;
    bool accepted = false;
    QString code;
    QString message;
    double serverTime = 0.0;
};

struct TransferEventProjection {
    QString kind;
    qint64 revision = 0;
    qint64 requestRevision = 0;
    qint64 userId = 0;
    QString sourceSeatId;
    QString targetSeatId;
    QString templateId;
    QString reason;
};

ValidationResult projectRoomLifecycle(const QJsonObject& roomState,
                                      RoomLifecycleProjection* projection);
ValidationResult projectSnapshot(const QJsonObject& payload, SnapshotProjection* projection);
ValidationResult projectSeatDirectory(const QJsonObject& payload,
                                      SeatDirectoryProjection* projection);
ValidationResult projectDeploymentPrompt(const QJsonObject& payload,
                                         DeploymentPromptProjection* projection);
ValidationResult projectIntelShare(const QJsonObject& payload,
                                   IntelShareProjection* projection);
ValidationResult projectCommandResult(const QJsonObject& payload,
                                      CommandResultProjection* projection);
ValidationResult projectTransferEvent(const QJsonObject& payload,
                                      TransferEventProjection* projection);
ValidationResult projectServerEvent(const QJsonObject& payload);
QVariantList seatVariants(const QList<SeatProjection>& seats);

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

// 联网模式的统一战位消息名。当前权威数据面使用 WebSocket。
// 传输，但两者都使用同一套 envelope，保证服务器权限和客户端状态机一致。
inline constexpr const char* RoomListMessage = "roomList";
inline constexpr const char* JoinRoomMessage = "joinRoom";
inline constexpr const char* LeaveRoomMessage = "leaveRoom";
inline constexpr const char* ClaimSeatMessage = "claimSeat";
inline constexpr const char* ReleaseSeatMessage = "releaseSeat";
inline constexpr const char* SeatReadyMessage = "seatReady";
inline constexpr const char* DeploymentMessage = "deployment";
inline constexpr const char* RequestRedeployMessage = "requestRedeploy";
inline constexpr const char* RedeployMessage = "redeploy";
inline constexpr const char* ShareIntelMessage = "shareIntel";
inline constexpr const char* MapMarkMessage = "mapMark";
inline constexpr const char* HeartbeatMessage = "heartbeat";
inline constexpr const char* RoomDirectoryMessage = "roomDirectory";
inline constexpr const char* SeatStateMessage = "seatState";
inline constexpr const char* DeploymentPromptMessage = "deploymentPrompt";
inline constexpr const char* IntelShareEventMessage = "intelShare";

} // namespace gbr::Protocol
