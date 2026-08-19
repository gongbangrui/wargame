#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QtGlobal>

namespace gbr::Protocol {

// v7 is the current VMF contract. v6 contains the intelligence-ledger
// fields and v4 is the pre-ledger contract; both remain valid during a
// rolling deployment so a new client can downgrade without losing its room.
inline constexpr int Version = 7;
inline constexpr int SchemaVersion = 7;
inline constexpr int IntelVersion = 5;
inline constexpr int IntelSchemaVersion = 5;
inline constexpr int PreviousVersion = 6;
inline constexpr int PreviousSchemaVersion = 6;
inline constexpr int LegacyVersion = 4;
inline constexpr int LegacySchemaVersion = 4;
inline constexpr int MaxMessageBytes = 256 * 1024;
inline constexpr int MaxServerMessageBytes = 8 * 1024 * 1024;
inline constexpr int MaxVmfWireBytes = 128 * 1024;
inline constexpr int MaxIdentifierLength = 64;
inline constexpr int MaxActionLength = 64;
inline constexpr int MaxTokenLength = 4096;
inline constexpr int MaxChatLength = 500;
// v4 used the chat-sized compatibility field for shareIntel notes and
// accepted up to 1024 characters. Keep that limit on the legacy wire only;
// the v5 intelligence ledger remains bounded by MaxIntelNoteLength below.
inline constexpr int LegacyShareIntelNoteLength = 1024;
inline constexpr int MaxMapLabelLength = 128;
inline constexpr int MaxJsonDepth = 16;
inline constexpr int MaxJsonNodes = 262144;
inline constexpr int MaxRoomNameLength = 96;
inline constexpr int MaxRoomDescriptionLength = 512;
inline constexpr int MaxSeatIdLength = 64;
inline constexpr int MaxDdsTicketLength = 128;
inline constexpr int MaxIntelNoteLength = 500;
inline constexpr int MaxIntelTitleLength = 64;
inline constexpr int MaxIntelTimestampLength = 64;
inline constexpr int MaxIntelKnownAttributes = 32;
inline constexpr int MaxIntelAttributeValueLength = 128;
inline constexpr int MaxIntelPropagationSources = 32;
inline constexpr int MaxIntelShareTargets = 64;
inline constexpr int MaxIntelRecords = 4096;
inline constexpr int MaxIntelHistoryPageSize = 200;
inline constexpr int MaxIntelCursorLength = 256;
inline constexpr int MaxIntelSearchLength = 128;
inline constexpr int MaxProjectiles = 512;
inline constexpr int MaxProjectileRecords = MaxProjectiles * 2;
inline constexpr int MaxChangedUnitIds = 512;
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
    QString controllerType = QStringLiteral("human");
};

struct RoomLifecycleProjection {
    QString phase = QStringLiteral("preparing");
    QString roomId;
    QString roomName;
    QString roomDescription;
    QString scenarioId = QStringLiteral("default");
    QString roomStatus;
    QString roomMode = QStringLiteral("pvp");
    QString aiDifficulty = QStringLiteral("normal");
    QString aiEngine = QStringLiteral("rules");
    qint64 configVersion = 1;
    bool observer = false;
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
    QJsonObject seatLimits;
    QJsonObject seatParameters;
    QJsonObject vmfWorkflow;
    QJsonObject vmfWorkflows;
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
    QString intelId;
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
ValidationResult validateSnapshotState(const QJsonObject& payload,
                                       int schemaVersion = SchemaVersion);
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
bool isSupportedWireVersion(int protocolVersion, int schemaVersion);
ValidationResult validateClientEnvelope(const QJsonObject& envelope);
ValidationResult validateServerEnvelope(const QJsonObject& envelope);
ValidationResult validateClientEnvelopeForVersion(const QJsonObject& envelope);
ValidationResult validateServerEnvelopeForVersion(const QJsonObject& envelope);
ValidationResult validateClientPayload(const QString& type, const QJsonObject& payload);
ValidationResult validateServerPayload(const QString& type, const QJsonObject& payload);
ValidationResult validateClientPayloadForVersion(const QString& type,
                                                 const QJsonObject& payload,
                                                 int schemaVersion);
ValidationResult validateServerPayloadForVersion(const QString& type,
                                                 const QJsonObject& payload,
                                                 int schemaVersion);

QJsonObject makeClientEnvelope(const QString& type, const QString& messageId,
                               const QJsonObject& payload);
QJsonObject makeClientEnvelopeForVersion(const QString& type, const QString& messageId,
                                         const QJsonObject& payload,
                                         int protocolVersion, int schemaVersion);
QJsonObject makeServerEnvelope(const QString& type, quint64 sequence,
                               const QJsonObject& payload);
QJsonObject makeServerEnvelopeForVersion(const QString& type, quint64 sequence,
                                         const QJsonObject& payload,
                                         int protocolVersion, int schemaVersion);

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
inline constexpr const char* CreateIntelReportMessage = "createIntelReport";
inline constexpr const char* RequestIntelHistoryMessage = "requestIntelHistory";
inline constexpr const char* MapMarkMessage = "mapMark";
inline constexpr const char* SetObserverTrajectoriesMessage = "setObserverTrajectories";
inline constexpr const char* SetObserverTrailsMessage = "setObserverTrails";
inline constexpr const char* HeartbeatMessage = "heartbeat";
inline constexpr const char* RoomDirectoryMessage = "roomDirectory";
inline constexpr const char* SeatStateMessage = "seatState";
inline constexpr const char* DeploymentPromptMessage = "deploymentPrompt";
inline constexpr const char* IntelShareEventMessage = "intelShare";
inline constexpr const char* IntelHistoryPageMessage = "intelHistoryPage";
inline constexpr const char* VmfMessage = "vmfMessage";
inline constexpr const char* VmfEventMessage = "vmfEvent";

inline constexpr int MaxObserverTrajectoryUnits = 8;
inline constexpr int MaxObserverTrajectoryPoints = 90;

} // namespace gbr::Protocol
