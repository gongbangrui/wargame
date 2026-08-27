#include "SimulationController.h"
#include "../core/UnitBase.h"
#include "../core/SnapshotCodec.h"
#include "../protocol/Protocol.h"

#include <QJsonArray>
#include <QDateTime>
#include <QSet>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <qt6keychain/keychain.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace gbr {

namespace {

constexpr auto kPasswordService = "org.gbr.wargame";

QString rememberedPasswordKey(const QString& server, const QString& username) {
    const QByteArray identity = server.trimmed().toUtf8() + '\0' + username.trimmed().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
}

QJsonObject projectVmfWorkflow(const QJsonObject& source) {
    static const QStringList fields{
        QStringLiteral("taskId"), QStringLiteral("stage"), QStringLiteral("side"),
        QStringLiteral("targetId"), QStringLiteral("attackerId"),
        QStringLiteral("guideId"), QStringLiteral("correlationId"),
        QStringLiteral("createdAt"), QStringLiteral("updatedAt")};
    QJsonObject projected;
    for (const QString& field : fields) {
        if (source.contains(field)) projected.insert(field, source.value(field));
    }
    return projected;
}

ScenarioUnit scenarioUnitFromVariantMap(const QVariantMap& data, bool generateId) {
    ScenarioUnit unit;
    unit.id = data.value(QStringLiteral("id")).toString().trimmed();
    if (generateId && unit.id.isEmpty()) {
        unit.id = QStringLiteral("u_%1").arg(QDateTime::currentMSecsSinceEpoch());
    }
    unit.vmfUrn = data.value(QStringLiteral("vmfUrn")).toString().trimmed();
    if (unit.vmfUrn.isEmpty() && !unit.id.isEmpty()) {
        unit.vmfUrn = QStringLiteral("urn:gbr:wargame:unit:%1").arg(unit.id);
    }
    unit.callsign = data.value(QStringLiteral("callsign")).toString();
    unit.kind = data.value(QStringLiteral("kind"), QStringLiteral("commandpost")).toString();
    unit.side = data.value(QStringLiteral("side"), QStringLiteral("red")).toString();
    if (data.contains(QStringLiteral("x"))) unit.pos.x = data.value(QStringLiteral("x")).toDouble();
    if (data.contains(QStringLiteral("y"))) unit.pos.y = data.value(QStringLiteral("y")).toDouble();
    if (data.contains(QStringLiteral("alt"))) unit.pos.alt = data.value(QStringLiteral("alt")).toDouble();
    if (data.contains(QStringLiteral("detectRange"))) unit.detectRange = data.value(QStringLiteral("detectRange")).toDouble();
    if (data.contains(QStringLiteral("attackRange"))) unit.attackRange = data.value(QStringLiteral("attackRange")).toDouble();
    if (data.contains(QStringLiteral("commRange"))) unit.commRange = data.value(QStringLiteral("commRange")).toDouble();
    if (data.contains(QStringLiteral("speed"))) unit.speed = data.value(QStringLiteral("speed")).toDouble();
    if (data.contains(QStringLiteral("maxHp"))) unit.maxHp = data.value(QStringLiteral("maxHp")).toDouble();
    if (data.contains(QStringLiteral("armor"))) unit.armor = data.value(QStringLiteral("armor")).toDouble();
    if (data.contains(QStringLiteral("repairRate"))) unit.repairRate = data.value(QStringLiteral("repairRate")).toDouble();
    if (data.contains(QStringLiteral("subsystemRepairRate"))) unit.subsystemRepairRate = data.value(QStringLiteral("subsystemRepairRate")).toDouble();
    if (data.contains(QStringLiteral("attackPower"))) unit.attackPower = data.value(QStringLiteral("attackPower")).toDouble();
    if (data.contains(QStringLiteral("ammoCapacity"))) unit.ammoCapacity = data.value(QStringLiteral("ammoCapacity")).toInt();
    if (data.contains(QStringLiteral("initialAmmo"))) unit.initialAmmo = data.value(QStringLiteral("initialAmmo")).toInt();
    if (data.contains(QStringLiteral("hitProbability"))) unit.hitProbability = data.value(QStringLiteral("hitProbability")).toDouble();
    if (data.contains(QStringLiteral("optimalRange"))) unit.optimalRange = data.value(QStringLiteral("optimalRange")).toDouble();
    if (data.contains(QStringLiteral("minAttackRange"))) unit.minAttackRange = data.value(QStringLiteral("minAttackRange")).toDouble();
    if (data.contains(QStringLiteral("cooldownSec"))) unit.cooldownSec = data.value(QStringLiteral("cooldownSec")).toDouble();
    if (data.contains(QStringLiteral("damageMin"))) unit.damageMin = data.value(QStringLiteral("damageMin")).toDouble();
    if (data.contains(QStringLiteral("damageMax"))) unit.damageMax = data.value(QStringLiteral("damageMax")).toDouble();
    if (data.contains(QStringLiteral("rangeFalloff"))) unit.rangeFalloff = data.value(QStringLiteral("rangeFalloff")).toDouble();
    if (data.contains(QStringLiteral("fuelCapacitySec"))) unit.fuelCapacitySec = data.value(QStringLiteral("fuelCapacitySec")).toDouble();
    if (data.contains(QStringLiteral("initialFuelSec"))) unit.initialFuelSec = data.value(QStringLiteral("initialFuelSec")).toDouble();
    if (data.contains(QStringLiteral("rearmDurationSec"))) unit.rearmDurationSec = data.value(QStringLiteral("rearmDurationSec")).toDouble();
    if (unit.kind == QLatin1String("attackuav")) {
        if (!data.contains(QStringLiteral("optimalRange"))) unit.optimalRange = unit.attackRange;
        if (!data.contains(QStringLiteral("damageMin"))) unit.damageMin = unit.attackPower;
        if (!data.contains(QStringLiteral("damageMax"))) unit.damageMax = unit.attackPower;
    }
    for (const auto& value : data.value(QStringLiteral("schedule")).toList()) {
        const auto pointMap = value.toMap();
        SchedulePoint point;
        point.time = pointMap.value(QStringLiteral("time")).toDouble();
        point.x = pointMap.value(QStringLiteral("x")).toDouble();
        point.y = pointMap.value(QStringLiteral("y")).toDouble();
        unit.schedule.push_back(point);
    }
    std::sort(unit.schedule.begin(), unit.schedule.end(),
              [](const SchedulePoint& a, const SchedulePoint& b) {
                  return a.time < b.time;
              });
    return unit;
}

} // namespace

SimulationController::SimulationController(QObject* parent) : QObject(parent) {
    m_engine.loadDefaultScenario();
    m_focusedSide = "red";
    const QVariantList storedServers = loadSetting(QStringLiteral("network/serverHistory"), QVariantList{}).toList();
    for (const QVariant& value : storedServers) {
        const QString server = value.toString().trimmed();
        if (!server.isEmpty() && !m_serverHistory.contains(server)) m_serverHistory.append(server);
    }
    setViewMode("commandpost-red");

    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, [this]() { m_networkClient.close(true); }, Qt::DirectConnection);
    }

    connect(&m_engine, &SimulationEngine::simTimeChanged, this, &SimulationController::simTimeForward);
    connect(&m_engine, &SimulationEngine::runningChanged, this, &SimulationController::runningForward);
    connect(&m_engine, &SimulationEngine::unitsChanged, this, [this]() {
        invalidateCaches();
        ++m_unitStateRevision;
        emit unitsForward();
    });
    connect(&m_engine, &SimulationEngine::projectilesChanged, this, [this]() {
        if (!isNetworked()) emit projectilesForward();
    });
    connect(&m_engine, &SimulationEngine::messagesChanged, this, &SimulationController::messagesForward);
    connect(&m_engine, &SimulationEngine::vmfWorkflowChanged, this,
            &SimulationController::vmfWorkflowChanged);
    connect(&m_networkClient, &NetworkClient::vmfTaskResultReceived, this,
            [this](const QJsonObject& result) {
                if (result.value(QStringLiteral("status")).toString()
                        == QLatin1String("rejected")) {
                    emit errorForward(result.value(QStringLiteral("code")).toString());
                }
                emit vmfTasksChanged();
            });
    connect(&m_networkClient, &NetworkClient::vmfTraceReceived, this,
            [this](const QJsonObject& trace) {
                m_remoteVmfTrace = trace;
                emit vmfTraceChanged();
            });
    connect(&m_networkClient, &NetworkClient::demoStateReceived, this,
            [this](const QJsonObject& state) {
                if (m_remoteDemoState == state) return;
                m_remoteDemoState = state;
                emit demoStateChanged();
            });
    connect(&m_networkClient, &NetworkClient::demoTraceReceived, this,
            [this](const QJsonObject& trace) {
                m_remoteVmfTrace = trace;
                emit vmfTraceChanged();
            });
    connect(&m_networkClient, &NetworkClient::demoResultReceived, this,
            [this](const QJsonObject& result) {
                const QJsonObject state = result.value(QStringLiteral("state")).toObject();
                if (!state.isEmpty() && state != m_remoteDemoState) {
                    m_remoteDemoState = state;
                    emit demoStateChanged();
                }
                emit demoCommandCompleted();
            });
    connect(&m_networkClient, &NetworkClient::demoErrorReceived, this,
            [this](const QJsonObject& error) {
                emit errorForward(error.value(QStringLiteral("message")).toString());
                emit demoCommandCompleted();
            });
    connect(&m_engine, &SimulationEngine::timelineChanged, this, &SimulationController::timelineForward);
    connect(&m_engine, &SimulationEngine::mapChanged, this, &SimulationController::mapInfoForward);
    connect(&m_engine, &SimulationEngine::readyForSimChanged, this, &SimulationController::readyForSimForward);
    connect(&m_engine, &SimulationEngine::targetDestroyedVisual, this, &SimulationController::targetDestroyedVisual);
    connect(&m_engine, &SimulationEngine::eventPosted, this,
            [this](const QString& title, const QString& body, const QString& level,
                   const QString& sourceUnitId) {
                if (!isNetworked()) emit eventForward(title, body, level, sourceUnitId);
            });
    connect(&m_engine, &SimulationEngine::unitDestroyed, this, &SimulationController::onUnitDestroyed);
    connect(&m_engine, &SimulationEngine::errorOccurred, this, [this](const QString& message) {
        if (!isNetworked()) emit errorForward(message);
    });
    connect(&m_engine, &SimulationEngine::simulationEnded, this,
            [this](const QString& winner, const QString& loser) {
                if (!isNetworked()) emit simEndForward(winner, loser);
            });

    connect(&m_networkClient, &NetworkClient::stateChanged, this,
            [this](const QString& state, const QString& message) {
                m_networkState = state;
                m_networkStatus = message;
                emit networkStatusChanged();
            });
    connect(&m_networkClient, &NetworkClient::diagnosticsChanged, this,
            [this](const QString& state, const QString& message, int accountLatency, int gameLatency) {
                m_networkDiagnosticState = state;
                m_networkDiagnosticMessage = message;
                m_accountLatencyMs = accountLatency;
                m_gameLatencyMs = gameLatency;
                emit networkDiagnosticsChanged();
            });
    connect(&m_networkClient, &NetworkClient::authenticated, this,
            [this](const QString& username, const QString& displayName,
                   const QString& role, const QString& seatId, const QString& server) {
                const bool wasNetworked = isNetworked();
                m_username = username;
                m_displayName = displayName;
                const QString normalizedRole = role.trimmed().toLower();
                m_userRole = normalizedRole == QLatin1String("room_admin")
                        || normalizedRole == QLatin1String("editor")
                    ? QStringLiteral("room_admin") : QStringLiteral("player");
                m_currentSeatId = seatId;
                m_isObserver = false;
                m_observerJoinPending = false;
                m_currentRoomId.clear();
                m_currentSeatType.clear();
                m_currentSeatSide.clear();
                m_roomName.clear();
                m_roomDescription.clear();
                m_scenarioId = QStringLiteral("default");
                m_onlineSeatLimits = {};
                m_onlineSeatParameters = {};
                m_lastCommandId.clear();
                m_lastCommandCode.clear();
                m_lastCommandAction.clear();
                m_lastCommandStatus.clear();
                m_lastCommandMessage.clear();
                m_communicationState = QStringLiteral("disconnected");
                m_onlineStage = QStringLiteral("roomSelect");
                m_serverAddress = server;
                rememberServerAddress(server);
                m_remoteScenarioRevision = -1;
                m_sessionMode = QStringLiteral("online");
                applyRoleView();
                emit sessionChanged();
                emit roomStateChanged();
                if (!wasNetworked) emit networkedChanged();
                emit onlineStateChanged();
                m_networkClient.requestRooms();
            });
    connect(&m_networkClient, &NetworkClient::roomDirectoryReceived, this,
            [this](const QJsonArray& rooms) {
                m_onlineRooms = rooms.toVariantList();
                const bool hadActiveRoom = !m_currentRoomId.isEmpty();
                const bool activeRoomStillListed = hadActiveRoom
                    && std::any_of(rooms.cbegin(), rooms.cend(), [this](const QJsonValue& value) {
                           return value.toObject().value(QStringLiteral("roomId")).toString()
                               == m_currentRoomId;
                       });
                if (hadActiveRoom && !activeRoomStillListed) {
                    clearOnlineRoomDerivedState(false);
                }
                emit onlineRoomsChanged();
            });
    connect(&m_networkClient, &NetworkClient::seatStateReceived, this,
            [this](const QJsonObject& state) {
                if (m_isObserver || m_observerJoinPending) return;
                Protocol::SeatDirectoryProjection directory;
                const Protocol::ValidationResult validation =
                    Protocol::projectSeatDirectory(state, &directory);
                if (!validation.valid) {
                    m_remoteLastError = validation.message;
                    emit errorForward(validation.message);
                    return;
                }
                m_onlineSeats = Protocol::seatVariants(directory.seats);
                m_currentRoomId = directory.roomId;
                const QString& yours = directory.yourSeatId;
                if (isRoomAdmin()) {
                    m_currentSeatId.clear();
                    m_currentSeatType.clear();
                    m_currentSeatSide.clear();
                    m_seatReady = false;
                    m_isObserver = false;
                    m_onlineStage = QStringLiteral("roomAdmin");
                } else if (!yours.isEmpty()) {
                    m_currentSeatId = yours;
                    const QStringList parts = yours.split(QLatin1Char('_'));
                    m_currentSeatSide = parts.value(0);
                    m_currentSeatType = parts.value(1);
                    for (const QVariant& value : m_onlineSeats) {
                        const QVariantMap seat = value.toMap();
                        if (seat.value(QStringLiteral("seatId")).toString() == m_currentSeatId) {
                            m_seatReady = seat.value(QStringLiteral("ready")).toBool();
                            break;
                        }
                    }
                    // A confirmed seat is the transition into the operational
                    // page. Keep this stable when later seat-state broadcasts
                    // refresh the directory for another client.
                    m_onlineStage = (m_matchPhase == QLatin1String("running")
                                     || m_matchPhase == QLatin1String("paused")
                                     || m_matchPhase == QLatin1String("finished"))
                        ? QStringLiteral("battle") : QStringLiteral("deployment");
                } else {
                    m_currentSeatId.clear();
                    m_currentSeatType.clear();
                    m_currentSeatSide.clear();
                    m_seatReady = false;
                    m_isObserver = false;
                    m_onlineStage = m_currentRoomId.isEmpty()
                        ? QStringLiteral("roomSelect") : QStringLiteral("seatSelect");
                }
                applyRoleView();
                emit onlineSeatsChanged();
                emit roomStateChanged();
                emit onlineStateChanged();
                emit sessionChanged();
            });
    connect(&m_networkClient, &NetworkClient::deploymentPromptReceived, this,
            [this](const QJsonObject& prompt) {
                Protocol::DeploymentPromptProjection projection;
                const Protocol::ValidationResult validation =
                    Protocol::projectDeploymentPrompt(prompt, &projection);
                if (!validation.valid) {
                    m_remoteLastError = validation.message;
                    emit errorForward(validation.message);
                    return;
                }
                emit deploymentPrompt(QVariantMap{{QStringLiteral("unitId"), projection.unitId},
                                                  {QStringLiteral("seatId"), projection.seatId},
                                                  {QStringLiteral("targetSeatId"), projection.targetSeatId},
                                                  {QStringLiteral("message"), projection.message}});
            });
    connect(&m_networkClient, &NetworkClient::intelShareReceived, this,
            [this](const QJsonObject& share) {
                Protocol::IntelShareProjection projection;
                const Protocol::ValidationResult validation =
                    Protocol::projectIntelShare(share, &projection);
                if (!validation.valid) {
                    m_remoteLastError = validation.message;
                    emit errorForward(validation.message);
                    return;
                }
                emit intelShareReceived(QVariantMap{{QStringLiteral("senderSeatId"), projection.senderSeatId},
                                                    {QStringLiteral("intelId"), projection.intelId},
                                                    {QStringLiteral("targetId"), projection.targetId},
                                                    {QStringLiteral("sharedAt"), projection.sharedAt},
                                                    {QStringLiteral("note"), projection.note}});
            });
    connect(&m_networkClient, &NetworkClient::intelHistoryPageReceived, this,
            [this](const QJsonObject& page) {
                // A room/seat reset invalidates any page that was still in flight.
                // The wire payload intentionally has no client request id, so the
                // controller accepts pages only while its single history request is live.
                if (m_onlineIntelHistoryRequestId.isEmpty()) return;
                Protocol::IntelHistoryPage projected;
                const Protocol::ValidationResult validation = Protocol::fromJson(page, &projected);
                if (!validation.valid) {
                    m_onlineIntelHistoryRequestId.clear();
                    m_onlineIntelHistoryAppendPending = false;
                    m_onlineIntelHistory.clear();
                    m_onlineIntelHistoryHasMore = false;
                    m_onlineIntelHistoryCursor.clear();
                    emit onlineIntelHistoryChanged();
                    m_remoteLastError = validation.message;
                    emit errorForward(validation.message);
                    return;
                }
                if (!m_onlineIntelHistoryAppendPending) m_onlineIntelHistory.clear();
                QSet<QString> existingHistoryIds;
                for (const QVariant& value : m_onlineIntelHistory) {
                    existingHistoryIds.insert(value.toMap().value(QStringLiteral("historyId"))
                                                  .toString());
                }
                for (const auto& entry : projected.entries) {
                    if (existingHistoryIds.contains(entry.historyId)) continue;
                    m_onlineIntelHistory.append(entry.toJson().toVariantMap());
                    existingHistoryIds.insert(entry.historyId);
                }
                m_onlineIntelHistoryAppendPending = false;
                m_onlineIntelHistoryHasMore = projected.hasMore;
                m_onlineIntelHistoryCursor = projected.nextCursor;
                emit onlineIntelHistoryChanged();
            });
    connect(&m_networkClient, &NetworkClient::transferEventReceived, this,
            [this](const QJsonObject& event) {
                Protocol::TransferEventProjection transfer;
                const Protocol::ValidationResult validation =
                    Protocol::projectTransferEvent(event, &transfer);
                if (!validation.valid) {
                    m_remoteLastError = validation.message;
                    emit errorForward(validation.message);
                    return;
                }
                const qint64 userId = transfer.userId;
                if (transfer.kind == QLatin1String("transferRequested")
                    && m_currentSeatType == QLatin1String("commander")
                    && transfer.sourceSeatId.startsWith(m_currentSeatSide + QLatin1Char('_'))) {
                    for (qsizetype i = m_pendingSeatTransfers.size() - 1; i >= 0; --i) {
                        if (m_pendingSeatTransfers.at(i).toMap()
                                .value(QStringLiteral("userId")).toLongLong() == userId) {
                            m_pendingSeatTransfers.removeAt(i);
                        }
                    }
                    m_pendingSeatTransfers.append(event.toVariantMap());
                    emit pendingSeatTransfersChanged();
                } else if (transfer.kind == QLatin1String("transferCompleted")
                           || transfer.kind == QLatin1String("transferRejected")) {
                    bool removed = false;
                    for (qsizetype i = m_pendingSeatTransfers.size() - 1; i >= 0; --i) {
                        if (m_pendingSeatTransfers.at(i).toMap()
                                .value(QStringLiteral("userId")).toLongLong() == userId) {
                            m_pendingSeatTransfers.removeAt(i);
                            removed = true;
                        }
                    }
                    if (removed) emit pendingSeatTransfersChanged();
                }
                emit transferEventReceived(event.toVariantMap());
            });
    connect(&m_networkClient, &NetworkClient::snapshotReceived,
            this, &SimulationController::applyRemoteSnapshot);
    connect(&m_networkClient, &NetworkClient::vmfEventReceived, this,
            [this](const QJsonObject& event) {
                const QJsonObject workflow = event.value(QStringLiteral("workflow")).toObject();
                if (workflow.isEmpty() || workflow == m_remoteVmfWorkflow) return;
                m_remoteVmfWorkflow = workflow;
                emit vmfWorkflowChanged();
            });
    connect(&m_networkClient, &NetworkClient::deltaSnapshotReceived, this,
            [this](const QJsonObject& payload, const QStringList& changedUnitIds) {
                applyRemoteState(payload, changedUnitIds, true);
            });
    connect(&m_networkClient, &NetworkClient::chatHistoryReceived, this,
            [this](const QJsonArray& messages) {
                m_chatMessages = messages.toVariantList();
                emit chatMessagesChanged();
            });
    connect(&m_networkClient, &NetworkClient::chatReceived, this,
            [this](const QJsonObject& message) {
                m_chatMessages.append(message.toVariantMap());
                while (m_chatMessages.size() > 100) m_chatMessages.removeFirst();
                emit chatMessagesChanged();
            });
    connect(&m_networkClient, &NetworkClient::eventReceived, this,
            [this](const QJsonObject& event) {
        const QString kind = event.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("simulationEnded") || kind == QLatin1String("forfeit")) {
            m_matchPhase = QStringLiteral("finished");
            m_onlineStage = m_isObserver ? QStringLiteral("observer")
                : m_currentSeatId.isEmpty() ? QStringLiteral("seatSelect") : QStringLiteral("battle");
            emit roomStateChanged();
            emit onlineStateChanged();
            emit simEndForward(event.value(QStringLiteral("winner")).toString(),
                               event.value(QStringLiteral("loser")).toString());
        } else if (kind == QLatin1String("matchEndedByAdmin")) {
            m_matchPhase = QStringLiteral("finished");
            m_onlineStage = m_isObserver ? QStringLiteral("observer")
                : m_currentSeatId.isEmpty() ? QStringLiteral("seatSelect") : QStringLiteral("battle");
            emit roomStateChanged();
            emit onlineStateChanged();
            emit eventForward(QStringLiteral("联网推演"),
                              event.value(QStringLiteral("message")).toString(),
                              QStringLiteral("warning"), QString());
        } else if (kind == QLatin1String("matchStarted")) {
            m_matchPhase = QStringLiteral("running");
            emit roomStateChanged();
            emit eventForward(QStringLiteral("联网推演"),
                              event.value(QStringLiteral("message")).toString(),
                              QStringLiteral("info"), QString());
        } else if (kind == QLatin1String("matchReset")) {
            // 重置事件先于完整快照抵达，先解除阵容编辑锁定以避免界面停留在旧阶段。
            clearOnlineRoomDerivedState(true);
            emit eventForward(QStringLiteral("联网推演"),
                              event.value(QStringLiteral("message")).toString(),
                              QStringLiteral("info"), QString());
        } else if (kind == QLatin1String("roomClosed")) {
            clearOnlineRoomDerivedState(false);
            m_networkClient.requestRooms();
            const QString closeMessage = event.value(QStringLiteral("message")).toString().trimmed();
            emit eventForward(QStringLiteral("联网房间"),
                              closeMessage.isEmpty()
                                  ? QStringLiteral("房间已关闭，已返回房间目录；登录身份保持有效")
                                  : closeMessage + QStringLiteral("；已返回房间目录，登录身份保持有效"),
                              QStringLiteral("warning"), QString());
        } else if (kind == QLatin1String("simulationEvent")) {
                    emit eventForward(event.value(QStringLiteral("title")).toString(),
                                      event.value(QStringLiteral("body")).toString(),
                                      event.value(QStringLiteral("level")).toString(),
                                      event.value(QStringLiteral("sourceUnitId")).toString());
        } else if (kind == QLatin1String("targetDestroyed")) {
                    emit targetDestroyedVisual(event.value(QStringLiteral("unitId")).toString(),
                                               event.value(QStringLiteral("x")).toDouble(),
                                               event.value(QStringLiteral("y")).toDouble());
                } else if (kind == QLatin1String("mapMark")) {
                    m_onlineMapMarks.append(event.toVariantMap());
                    while (m_onlineMapMarks.size() > 200) m_onlineMapMarks.removeFirst();
                    emit onlineMapMarksChanged();
                } else {
                    emit eventForward(QStringLiteral("联网推演"),
                                      event.value(QStringLiteral("message")).toString(),
                                      QStringLiteral("info"), QString());
                }
            });
    const auto reportNetworkError = [this](const QString& message) {
        if (m_observerJoinPending) {
            m_observerJoinPending = false;
            clearOnlineRoomDerivedState(false);
        }
        m_remoteLastError = message;
        emit errorForward(message);
    };
    connect(&m_networkClient, &NetworkClient::fatalError, this, reportNetworkError);
    connect(&m_networkClient, &NetworkClient::commandRejected, this, reportNetworkError);
    connect(&m_networkClient, &NetworkClient::authenticationLost, this,
            [this](const QString& message) {
                logoutOnline();
                m_remoteLastError = message;
                emit errorForward(message);
            });
    connect(&m_networkClient, &NetworkClient::commandStatusChanged, this,
            [this](const QString& commandId, const QString& action, const QString& status,
                   const QString& code, const QString& message) {
                m_lastCommandId = commandId;
                m_lastCommandCode = code;
                m_lastCommandAction = action;
                m_lastCommandStatus = status;
                m_lastCommandMessage = message;
                emit commandStatusChanged();
                if (action == QLatin1String("leaveRoom")
                    && (status == QLatin1String("accepted")
                        || status == QLatin1String("rejected")
                        || status == QLatin1String("unknown")
                        || status == QLatin1String("canceled"))) {
                    if (m_leaveRoomPending) {
                        m_leaveRoomPending = false;
                        emit leaveRoomPendingChanged();
                    }
                    if (status == QLatin1String("accepted")) {
                        clearOnlineRoomDerivedState(false);
                        m_networkClient.requestRooms();
                    }
                }
                if (action == QLatin1String("requestIntelHistory")
                    && commandId == m_onlineIntelHistoryRequestId
                    && (status == QLatin1String("accepted")
                        || status == QLatin1String("rejected")
                        || status == QLatin1String("unknown")
                        || status == QLatin1String("canceled"))) {
                    m_onlineIntelHistoryRequestId.clear();
                    m_onlineIntelHistoryAppendPending = false;
                    if (status != QLatin1String("accepted")) {
                        m_onlineIntelHistory.clear();
                        m_onlineIntelHistoryHasMore = false;
                        m_onlineIntelHistoryCursor.clear();
                    }
                    emit onlineIntelHistoryChanged();
                }
                if (action == QLatin1String("shareIntel")
                    || action == QLatin1String("createIntelReport")
                    || action == QLatin1String("requestIntelHistory")) {
                    emit onlineIntelCommandStatus(action, commandId, status, code, message);
                }
            });
}

void SimulationController::onUnitDestroyed(const QString& unitId) {
    if (m_focusedUnitId == unitId) {
        m_focusedUnitId.clear();
        emit focusedUnitIdChanged();
        // For commandpost-red/blue view, re-pick the alive CP so UnitPanel
        // doesn't stay empty until the next setViewMode.
        ensureFocusedConsistent();
    }
}

void SimulationController::setViewMode(const QString& m) {
    if (!viewModeOptions().contains(m)) return;
    if (isNetworked()) {
        QString expected;
        if (isRoomAdmin()) expected = QStringLiteral("editor");
        else if (m_currentSeatSide == QLatin1String("red")
                 || m_currentSeatSide == QLatin1String("blue")) {
            expected = QStringLiteral("commandpost-%1").arg(m_currentSeatSide);
        }
        if (!expected.isEmpty() && m != expected) return;
    }
    if (m == m_viewMode) return;
    const QString previousSide = m_focusedSide;
    m_viewMode = m;
    ensureFocusedConsistent();
    if (previousSide != m_focusedSide) emit focusedSideChanged();
    emit viewModeChanged();
}

void SimulationController::setFocusedSide(const QString& s) {
    if (s != QLatin1String("red") && s != QLatin1String("blue")) return;
    if (isNetworked() && !isRoomAdmin() && !m_currentSeatSide.isEmpty()
        && s != m_currentSeatSide) return;
    if (m_focusedSide == s) return;
    const QString previousSide = m_focusedSide;
    m_focusedSide = s;
    ensureFocusedConsistent();
    if (previousSide != m_focusedSide) emit focusedSideChanged();
}

void SimulationController::setFocusedUnitId(const QString& id) {
    if (m_focusedUnitId == id) return;
    m_focusedUnitId = id;
    emit focusedUnitIdChanged();
}

QString SimulationController::focusedKind() const {
    auto snap = m_engine.unitSnapshot(m_focusedUnitId);
    return snap.value("kind").toString();
}

QJsonObject SimulationController::vmfWorkflow() const {
    if (isNetworked()) return projectVmfWorkflow(m_remoteVmfWorkflow);
    const QString side = m_focusedSide == QLatin1String("blue")
        ? QStringLiteral("blue") : QStringLiteral("red");
    const GuidedStrikeWorkflow* workflow = m_engine.guidedStrikeWorkflow(side);
    return workflow ? projectVmfWorkflow(workflow->snapshot()) : QJsonObject{};
}

void SimulationController::loadDefault() {
    if (isNetworked()) {
        if (canEditScenario())
            m_networkClient.sendScenarioReplace(ScenarioIo::toJson(ScenarioIo::defaultScenario()));
        return;
    }
    m_engine.loadDefaultScenario();
    ensureFocusedConsistent();
}

void SimulationController::saveScenario(const QString& path) {
    m_engine.persistScenario(path);
}

void SimulationController::loadScenario(const QString& path) {
    QString err;
    auto s = ScenarioIo::loadFromFile(path, &err);
    if (!err.isEmpty()) {
        emit errorForward(QStringLiteral("加载场景失败: %1").arg(err));
        return;
    }
    // Refuse to apply a scenario that has no command posts or zero units; this prevents
    // a silently-empty load from wiping the running world without diagnostic.
    int redCp = 0, blueCp = 0;
    for (const auto& u : s.units) {
        if (u.kind == QLatin1String("commandpost")) {
            if (u.side == QLatin1String("red")) ++redCp;
            else if (u.side == QLatin1String("blue")) ++blueCp;
        }
    }
    if (s.units.empty()) {
        emit errorForward(QStringLiteral("场景 '%1' 为空，已忽略").arg(path));
        return;
    }
    if (redCp != 1 || blueCp != 1) {
        emit errorForward(QStringLiteral("场景 '%1' 必须每方恰好 1 个指挥所（红=%2，蓝=%3），已忽略").arg(path).arg(redCp).arg(blueCp));
        return;
    }
    if (isNetworked()) {
        if (canEditScenario())
            m_networkClient.sendScenarioReplace(ScenarioIo::toJson(s));
        return;
    }
    if (m_engine.setScenario(s)) ensureFocusedConsistent();
}

void SimulationController::setRunning(bool r) {
    if (!isNetworked()) {
        m_engine.setRunning(r);
        return;
    }
    Q_UNUSED(r);
    emit errorForward(QStringLiteral("联网推演生命周期由网页管理员控制"));
}

void SimulationController::setSpeed(double s) {
    if (isNetworked()) {
        Q_UNUSED(s);
        emit errorForward(QStringLiteral("联网推演生命周期由网页管理员控制"));
    } else {
        m_engine.setSpeedMul(s);
    }
}

void SimulationController::stepOnce() {
    if (isNetworked()) {
        emit errorForward(QStringLiteral("联网推演生命周期由网页管理员控制"));
    } else {
        m_engine.stepOnce(1.0);
    }
}

void SimulationController::command(const QString& action, const QVariantMap& args) {
    if (isNetworked()) {
        if (m_isObserver) {
            emit errorForward(QStringLiteral("观察模式只读，不能下达命令"));
            return;
        }
        m_networkClient.sendCommand(action, args);
    }
    else m_engine.command(action, args);
    emit commandExecuted(action, args);
}

QString SimulationController::guidedStrikeSide() const {
    if (isNetworked()) return m_currentSeatSide;
    return m_focusedSide == QLatin1String("blue") ? QStringLiteral("blue")
                                                    : QStringLiteral("red");
}

QVariantMap SimulationController::guidedStrikeResult(bool accepted, const QString& code,
                                                     const QString& message,
                                                     const QString& requestId) const {
    QVariantMap result{{QStringLiteral("accepted"), accepted},
                       {QStringLiteral("code"), code},
                       {QStringLiteral("message"), message}};
    if (!requestId.isEmpty()) result.insert(QStringLiteral("requestId"), requestId);
    return result;
}

QVariantMap SimulationController::sendGuidedStrikeVmf(Message message) {
    if (!isNetworked()) {
        return guidedStrikeResult(false, QStringLiteral("LOCAL_ONLY"),
                                  QStringLiteral("本地 VMF 消息应通过工作流提交"));
    }
    if (m_isObserver || m_currentSeatSide.isEmpty() || m_currentSeatId.isEmpty()) {
        return guidedStrikeResult(false, QStringLiteral("SEAT_REQUIRED"),
                                  QStringLiteral("当前会话没有可用战位"));
    }
    if (!m_engine.vmfEnabled()
        || m_engine.scenario().communicationPolicy.format
               != QLatin1String("vmf-design-v1")) {
        return guidedStrikeResult(false, QStringLiteral("VMF_DISABLED"),
                                  QStringLiteral("当前场景未启用 VMF profile"));
    }
    if (message.sender.isEmpty() || message.receiver.isEmpty()) {
        return guidedStrikeResult(false, QStringLiteral("INVALID_ARGUMENT"),
                                  QStringLiteral("VMF 消息缺少发送方或接收方"));
    }
    if (message.traceId.isEmpty()) {
        message.traceId = QStringLiteral("guided-strike-%1")
            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    if (message.correlationId.isEmpty()) {
        message.correlationId = m_remoteVmfWorkflow
            .value(QStringLiteral("correlationId")).toString();
    }
    if (message.correlationId.isEmpty()) message.correlationId = message.traceId;
    message.requiresAck = true;
    message.automaticAck = true;
    message.timestamp = QDateTime::currentDateTimeUtc();

    Message encoded;
    QString error;
    if (!m_engine.prepareVmfMessage(message, &encoded, &error)) {
        return guidedStrikeResult(false, QStringLiteral("INVALID_VMF"), error);
    }
    const QJsonObject payload{
        {QStringLiteral("messageType"), Message::typeName(encoded.type)},
        {QStringLiteral("senderUnitId"), encoded.sender},
        {QStringLiteral("receiverUnitId"), encoded.receiver},
        {QStringLiteral("traceId"), encoded.traceId},
        {QStringLiteral("correlationId"), encoded.correlationId},
        {QStringLiteral("vmfMessage"), encoded.vmfMessage},
        {QStringLiteral("wireBytes"), QString::fromLatin1(encoded.wireBytes.toBase64())},
        {QStringLiteral("wireBitLength"), encoded.wireBitLength},
        {QStringLiteral("requiresAck"), encoded.requiresAck},
        {QStringLiteral("retryCount"), encoded.retryCount},
        {QStringLiteral("fieldCount"), encoded.payload.value(QStringLiteral("vmfFieldCount"))},
        {QStringLiteral("payload"), encoded.payload}};
    const QString requestId = m_networkClient.sendVmfMessage(payload);
    if (requestId.isEmpty()) {
        return guidedStrikeResult(false, QStringLiteral("NETWORK_UNAVAILABLE"),
                                  QStringLiteral("联网会话尚未建立"));
    }
    return guidedStrikeResult(true, QStringLiteral("PENDING"),
                              QStringLiteral("VMF 消息已发送，等待服务器回执"), requestId);
}

QVariantMap SimulationController::reportGuidedStrikeTarget(const QString& reconId,
                                                           const QString& targetId,
                                                           const QVariantMap& report) {
    const QString side = guidedStrikeSide();
    if (side.isEmpty() || reconId.trimmed().isEmpty() || targetId.trimmed().isEmpty()) {
        return guidedStrikeResult(false, QStringLiteral("INVALID_ARGUMENT"),
                                  QStringLiteral("目标报告缺少侦察机或目标"));
    }
    QJsonObject payload = QJsonObject::fromVariantMap(report);
    payload.insert(QStringLiteral("targetId"), targetId.trimmed());
    const QJsonObject targetSnapshot = m_engine.unitSnapshot(targetId.trimmed());
    if (!payload.contains(QStringLiteral("x")) && !payload.contains(QStringLiteral("latitude"))) {
        const QJsonArray position = targetSnapshot.value(QStringLiteral("position")).toArray();
        if (position.size() >= 2) {
            payload.insert(QStringLiteral("x"), position.at(0));
            payload.insert(QStringLiteral("y"), position.at(1));
        }
    }
    if (!payload.contains(QStringLiteral("targetType"))) {
        payload.insert(QStringLiteral("targetType"),
                       targetSnapshot.value(QStringLiteral("kind")).toString(
                           QStringLiteral("unknown")));
    }
    if (!payload.contains(QStringLiteral("targetCount"))) payload.insert(QStringLiteral("targetCount"), 1);
    if (!payload.contains(QStringLiteral("friendFoe"))) payload.insert(QStringLiteral("friendFoe"), QStringLiteral("enemy"));
    if (!payload.contains(QStringLiteral("status"))) {
        payload.insert(QStringLiteral("status"), targetSnapshot.value(QStringLiteral("alive")).toBool(true)
                           ? QStringLiteral("intact") : QStringLiteral("destroyed"));
    }
    if (payload.isEmpty()) {
        return guidedStrikeResult(false, QStringLiteral("INVALID_ARGUMENT"),
                                  QStringLiteral("目标报告字段不完整"));
    }
    if (!isNetworked()) {
        GuidedStrikeWorkflow* workflow = m_engine.guidedStrikeWorkflow(side);
        if (!workflow) return guidedStrikeResult(false, QStringLiteral("VMF_DISABLED"), QStringLiteral("当前场景未启用 VMF"));
        QString error;
        const bool accepted = workflow->reportTarget(reconId.trimmed(), targetId.trimmed(), payload, &error);
        return guidedStrikeResult(accepted, accepted ? QStringLiteral("ACCEPTED") : QStringLiteral("REJECTED"),
                                  accepted ? QStringLiteral("目标报告已进入 VMF 工作流") : error);
    }
    Message message;
    message.type = Message::Type::TargetReport;
    message.sender = reconId.trimmed();
    message.receiver = commandPostIdFor(side);
    message.payload = payload;
    return sendGuidedStrikeVmf(message);
}

QVariantMap SimulationController::dispatchGuidedStrike(const QString& attackerId,
                                                       const QString& targetId,
                                                       const QVariantList& waypoints) {
    const QString side = guidedStrikeSide();
    const QString cpId = commandPostIdFor(side);
    const QJsonArray points = QJsonArray::fromVariantList(waypoints);
    if (side.isEmpty() || attackerId.trimmed().isEmpty() || targetId.trimmed().isEmpty()
        || points.isEmpty()) {
        return guidedStrikeResult(false, QStringLiteral("INVALID_ARGUMENT"),
                                  QStringLiteral("派单需要攻击机、目标和至少一个航点"));
    }
    if (!isNetworked()) {
        GuidedStrikeWorkflow* workflow = m_engine.guidedStrikeWorkflow(side);
        if (!workflow) return guidedStrikeResult(false, QStringLiteral("VMF_DISABLED"), QStringLiteral("当前场景未启用 VMF"));
        QString error;
        const bool accepted = workflow->confirmDispatch(cpId, attackerId.trimmed(), targetId.trimmed(), points, &error);
        return guidedStrikeResult(accepted, accepted ? QStringLiteral("ACCEPTED") : QStringLiteral("REJECTED"),
                                  accepted ? QStringLiteral("打击派单已进入 VMF 工作流") : error);
    }
    Message plan;
    plan.type = Message::Type::StrikePlan;
    plan.sender = cpId;
    plan.receiver = attackerId.trimmed();
    // StrikePlan and AttackOrder are two catalogued edges of one task.  They
    // must share the workflow correlation, while each envelope still gets a
    // distinct trace ID so server-side deduplication treats both deliveries
    // as separate messages.
    QString correlationId = m_remoteVmfWorkflow
                                .value(QStringLiteral("correlationId"))
                                .toString()
                                .trimmed();
    if (correlationId.isEmpty()) {
        correlationId = QStringLiteral("guided-strike-%1")
                            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    plan.correlationId = correlationId;
    plan.payload = QJsonObject{{QStringLiteral("targetId"), targetId.trimmed()},
                               {QStringLiteral("waypoints"), points}};
    QVariantMap result = sendGuidedStrikeVmf(plan);
    if (!result.value(QStringLiteral("accepted")).toBool()) return result;
    Message order;
    order.type = Message::Type::AttackOrder;
    order.sender = cpId;
    order.receiver = attackerId.trimmed();
    order.correlationId = correlationId;
    order.payload = QJsonObject{{QStringLiteral("targetId"), targetId.trimmed()},
                                {QStringLiteral("waypoints"), points}};
    order.payload.insert(QStringLiteral("fireNow"), false);
    const QVariantMap orderResult = sendGuidedStrikeVmf(order);
    if (!orderResult.value(QStringLiteral("accepted")).toBool()) return orderResult;
    result.insert(QStringLiteral("requestId"), orderResult.value(QStringLiteral("requestId")));
    result.insert(QStringLiteral("message"), QStringLiteral("派单与航路已发送，等待服务器工作流事件"));
    return result;
}

QVariantMap SimulationController::commandGuidedStrikeGroundGuidance(
    const QString& guideId, const QString& attackerId, const QString& targetId) {
    const QString side = guidedStrikeSide();
    const QString cpId = commandPostIdFor(side);
    if (side.isEmpty() || guideId.trimmed().isEmpty() || attackerId.trimmed().isEmpty()
        || targetId.trimmed().isEmpty()) {
        return guidedStrikeResult(false, QStringLiteral("INVALID_ARGUMENT"),
                                  QStringLiteral("地面引导命令字段不完整"));
    }
    if (!isNetworked()) {
        GuidedStrikeWorkflow* workflow = m_engine.guidedStrikeWorkflow(side);
        if (!workflow) return guidedStrikeResult(false, QStringLiteral("VMF_DISABLED"), QStringLiteral("当前场景未启用 VMF"));
        QString error;
        const bool accepted = workflow->commandGroundGuidance(cpId, guideId.trimmed(), attackerId.trimmed(), targetId.trimmed(), &error);
        return guidedStrikeResult(accepted, accepted ? QStringLiteral("ACCEPTED") : QStringLiteral("REJECTED"),
                                  accepted ? QStringLiteral("地面引导命令已进入 VMF 工作流") : error);
    }
    Message message;
    message.type = Message::Type::GroundGuideOrder;
    message.sender = cpId;
    message.receiver = guideId.trimmed();
    message.payload = QJsonObject{{QStringLiteral("attackerId"), attackerId.trimmed()},
                                  {QStringLiteral("targetId"), targetId.trimmed()}};
    return sendGuidedStrikeVmf(message);
}

QVariantMap SimulationController::confirmGuidedStrikeAttack(
    const QString& guideId, const QString& attackerId, const QString& targetId,
    const QVariantList& waypoints) {
    const QString side = guidedStrikeSide();
    const QJsonArray points = QJsonArray::fromVariantList(waypoints);
    if (side.isEmpty() || guideId.trimmed().isEmpty() || attackerId.trimmed().isEmpty()
        || targetId.trimmed().isEmpty() || points.isEmpty()) {
        return guidedStrikeResult(false, QStringLiteral("INVALID_ARGUMENT"),
                                  QStringLiteral("地面确认攻击需要引导单元、攻击机、目标和航点"));
    }
    if (!isNetworked()) {
        GuidedStrikeWorkflow* workflow = m_engine.guidedStrikeWorkflow(side);
        if (!workflow) return guidedStrikeResult(false, QStringLiteral("VMF_DISABLED"), QStringLiteral("当前场景未启用 VMF"));
        QString error;
        const bool accepted = workflow->confirmGroundAttack(guideId.trimmed(), attackerId.trimmed(), targetId.trimmed(), points, &error);
        return guidedStrikeResult(accepted, accepted ? QStringLiteral("ACCEPTED") : QStringLiteral("REJECTED"),
                                  accepted ? QStringLiteral("地面确认已进入 VMF 工作流") : error);
    }
    Message message;
    message.type = Message::Type::GroundAttackConfirm;
    message.sender = guideId.trimmed();
    message.receiver = attackerId.trimmed();
    message.payload = QJsonObject{{QStringLiteral("targetId"), targetId.trimmed()},
                                  {QStringLiteral("waypoints"), points},
                                  {QStringLiteral("fireNow"), true}};
    return sendGuidedStrikeVmf(message);
}

QVariantMap SimulationController::withdrawGuidedStrike(const QString& attackerId,
                                                       const QVariantMap& home) {
    const QString side = guidedStrikeSide();
    const QString cpId = commandPostIdFor(side);
    const QJsonObject homeObject = QJsonObject::fromVariantMap(home);
    double homeX = homeObject.value(QStringLiteral("x")).toDouble(
        std::numeric_limits<double>::quiet_NaN());
    double homeY = homeObject.value(QStringLiteral("y")).toDouble(
        std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(homeX) || !std::isfinite(homeY)) {
        const QJsonArray position = m_engine.unitSnapshot(attackerId.trimmed())
                                        .value(QStringLiteral("position")).toArray();
        if (position.size() >= 2) {
            homeX = position.at(0).toDouble();
            homeY = position.at(1).toDouble();
        }
    }
    if (side.isEmpty() || attackerId.trimmed().isEmpty() || !std::isfinite(homeX)
        || !std::isfinite(homeY)) {
        return guidedStrikeResult(false, QStringLiteral("INVALID_ARGUMENT"),
                                  QStringLiteral("撤离需要攻击机和有效返航位置"));
    }
    if (!isNetworked()) {
        GuidedStrikeWorkflow* workflow = m_engine.guidedStrikeWorkflow(side);
        if (!workflow) return guidedStrikeResult(false, QStringLiteral("VMF_DISABLED"), QStringLiteral("当前场景未启用 VMF"));
        QString error;
        const bool accepted = workflow->confirmWithdraw(cpId, attackerId.trimmed(), homeX, homeY, &error);
        return guidedStrikeResult(accepted, accepted ? QStringLiteral("ACCEPTED") : QStringLiteral("REJECTED"),
                                  accepted ? QStringLiteral("撤离命令已进入 VMF 工作流") : error);
    }
    Message message;
    message.type = Message::Type::WithdrawOrder;
    message.sender = cpId;
    message.receiver = attackerId.trimmed();
    message.payload = QJsonObject{{QStringLiteral("homeX"), homeX},
                                  {QStringLiteral("homeY"), homeY},
                                  {QStringLiteral("x"), homeX},
                                  {QStringLiteral("y"), homeY}};
    return sendGuidedStrikeVmf(message);
}

QVariantMap SimulationController::sendVmfTaskCommand(const QVariantMap& command) {
    if (!isNetworked() || m_isObserver) {
        return guidedStrikeResult(false, QStringLiteral("SEAT_REQUIRED"),
                                  QStringLiteral("严格 VMF 任务只能由联网战位提交"));
    }
    if (m_protocolProfile != QLatin1String("vmf-guided-strike-v1")) {
        return guidedStrikeResult(false, QStringLiteral("VMF_TASK_PROFILE_REQUIRED"),
                                  QStringLiteral("当前房间未启用严格 VMF profile"));
    }
    QJsonObject payload = QJsonObject::fromVariantMap(command);
    if (!payload.contains(QStringLiteral("requestId"))) {
        payload.insert(QStringLiteral("requestId"),
                       QStringLiteral("vmf-task-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    }
    if (!payload.contains(QStringLiteral("taskId"))
        || !payload.contains(QStringLiteral("action"))) {
        return guidedStrikeResult(false, QStringLiteral("INVALID_ARGUMENT"),
                                  QStringLiteral("严格 VMF 命令缺少 taskId 或 action"));
    }
    if (!payload.contains(QStringLiteral("messages"))) {
        payload.insert(QStringLiteral("messages"), QJsonArray{});
    }

    const QString action = payload.value(QStringLiteral("action")).toString();
    if (action != QLatin1String("createTask")
        && payload.value(QStringLiteral("messages")).toArray().isEmpty()) {
        QJsonObject task;
        for (const QJsonValue& value : m_remoteVmfTasks.value(QStringLiteral("tasks")).toArray()) {
            if (value.toObject().value(QStringLiteral("taskId"))
                == payload.value(QStringLiteral("taskId"))) {
                task = value.toObject();
                break;
            }
        }
        if (task.isEmpty()) {
            return guidedStrikeResult(false, QStringLiteral("TASK_NOT_FOUND"),
                                      QStringLiteral("严格 VMF 任务状态尚未同步"));
        }
        const auto unitForSeat = [this](const QString& seatId) {
            for (const QVariant& value : m_onlineSeats) {
                const QVariantMap seat = value.toMap();
                if (seat.value(QStringLiteral("seatId")).toString() == seatId) {
                    return seat.value(QStringLiteral("unitId")).toString();
                }
            }
            return QString{};
        };
        QString senderSeat;
        QString receiverSeat;
        Message::Type domainType = Message::Type::CommCheck;
        bool requiresMessage = true;
        if (action == QLatin1String("reportTarget")) {
            senderSeat = task.value(QStringLiteral("reconSeatId")).toString();
            receiverSeat = task.value(QStringLiteral("commanderSeatId")).toString();
            domainType = Message::Type::TargetReport;
        } else if (action == QLatin1String("dispatch")) {
            senderSeat = task.value(QStringLiteral("commanderSeatId")).toString();
            receiverSeat = task.value(QStringLiteral("attackSeatId")).toString();
            domainType = Message::Type::AttackOrder;
        } else if (action == QLatin1String("acceptDispatch")
                   || action == QLatin1String("acceptGuidance")) {
            senderSeat = task.value(QStringLiteral("attackSeatId")).toString();
            receiverSeat = action == QLatin1String("acceptDispatch")
                ? task.value(QStringLiteral("commanderSeatId")).toString()
                : task.value(QStringLiteral("groundSeatId")).toString();
            domainType = Message::Type::RouteAcceptance;
        } else if (action == QLatin1String("orderGroundGuidance")) {
            senderSeat = task.value(QStringLiteral("commanderSeatId")).toString();
            receiverSeat = task.value(QStringLiteral("groundSeatId")).toString();
            domainType = Message::Type::GroundGuideOrder;
        } else if (action == QLatin1String("identityHello")
                   || action == QLatin1String("identityConfirm")) {
            senderSeat = action == QLatin1String("identityHello")
                ? task.value(QStringLiteral("attackSeatId")).toString()
                : task.value(QStringLiteral("groundSeatId")).toString();
            receiverSeat = action == QLatin1String("identityHello")
                ? task.value(QStringLiteral("groundSeatId")).toString()
                : task.value(QStringLiteral("attackSeatId")).toString();
            domainType = Message::Type::IdentityReport;
        } else if (action == QLatin1String("sendGuidancePackage")) {
            senderSeat = task.value(QStringLiteral("groundSeatId")).toString();
            receiverSeat = task.value(QStringLiteral("attackSeatId")).toString();
            domainType = Message::Type::GroundTargetReport;
        } else if (action == QLatin1String("reportAttackReady")) {
            senderSeat = task.value(QStringLiteral("attackSeatId")).toString();
            receiverSeat = task.value(QStringLiteral("groundSeatId")).toString();
            domainType = Message::Type::AttackReadyReport;
        } else if (action == QLatin1String("authorizeAttack")) {
            senderSeat = task.value(QStringLiteral("groundSeatId")).toString();
            receiverSeat = task.value(QStringLiteral("attackSeatId")).toString();
            domainType = Message::Type::AttackAuthorization;
        } else if (action == QLatin1String("reportBattleDamage")) {
            senderSeat = task.value(QStringLiteral("attackSeatId")).toString();
            receiverSeat = task.value(QStringLiteral("groundSeatId")).toString();
            domainType = Message::Type::BattleDamageReport;
        } else if (action == QLatin1String("confirmDamageAssessment")) {
            senderSeat = task.value(QStringLiteral("groundSeatId")).toString();
            receiverSeat = task.value(QStringLiteral("attackSeatId")).toString();
            domainType = Message::Type::DamageAssessmentConfirm;
        } else if (action == QLatin1String("confirmTargetDestroyed")) {
            senderSeat = task.value(QStringLiteral("reconSeatId")).toString();
            receiverSeat = task.value(QStringLiteral("commanderSeatId")).toString();
            domainType = Message::Type::TargetDestroyed;
        } else if (action == QLatin1String("withdraw")) {
            senderSeat = task.value(QStringLiteral("commanderSeatId")).toString();
            receiverSeat = task.value(QStringLiteral("attackSeatId")).toString();
            domainType = Message::Type::WithdrawOrder;
        } else {
            requiresMessage = false;
        }
        if (requiresMessage) {
            Message source;
            source.id = QStringLiteral("strict-%1")
                            .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
            source.traceId = source.id;
            source.correlationId = task.value(QStringLiteral("correlationId")).toString();
            source.type = domainType;
            source.sender = unitForSeat(senderSeat);
            source.receiver = unitForSeat(receiverSeat);
            source.timestamp = QDateTime::currentDateTimeUtc();
            source.payload = QJsonObject{{QStringLiteral("taskId"),
                                           task.value(QStringLiteral("taskId"))},
                                          {QStringLiteral("targetId"),
                                           task.value(QStringLiteral("targetId"))}};
            QJsonArray route = task.value(QStringLiteral("route")).toArray();
            if (domainType == Message::Type::WithdrawOrder || route.isEmpty()) {
                const QString routePointUnitId = domainType == Message::Type::WithdrawOrder
                    ? source.sender : task.value(QStringLiteral("targetId")).toString();
                const QJsonArray targetPosition = m_engine.unitSnapshot(routePointUnitId)
                                                      .value(QStringLiteral("position")).toArray();
                if (targetPosition.size() >= 2) {
                    route = QJsonArray{QJsonObject{{QStringLiteral("x"), targetPosition.at(0)},
                                                   {QStringLiteral("y"), targetPosition.at(1)}}};
                }
            }
            if (!route.isEmpty()) {
                const QJsonObject destination = route.last().toObject();
                source.payload.insert(QStringLiteral("x"), destination.value(QStringLiteral("x")));
                source.payload.insert(QStringLiteral("y"), destination.value(QStringLiteral("y")));
                source.payload.insert(QStringLiteral("waypoints"), route);
            }
            if (domainType == Message::Type::BattleDamageReport
                || domainType == Message::Type::DamageAssessmentConfirm) {
                const QJsonObject target = m_engine.unitSnapshot(
                    task.value(QStringLiteral("targetId")).toString());
                source.payload.insert(QStringLiteral("hp"), target.value(QStringLiteral("hp")));
                source.payload.insert(QStringLiteral("alive"),
                                      target.value(QStringLiteral("alive")));
                source.payload.insert(QStringLiteral("destroyed"),
                                      !target.value(QStringLiteral("alive")).toBool(true));
            }
            Message encoded;
            QString encodeError;
            if (source.sender.isEmpty() || source.receiver.isEmpty()
                || !m_engine.prepareVmfMessage(source, &encoded, &encodeError)) {
                return guidedStrikeResult(false, QStringLiteral("VMF_CODEC_FAILED"),
                                          encodeError.isEmpty()
                                              ? QStringLiteral("严格 VMF 绑定单元不可用")
                                              : encodeError);
            }
            payload.insert(QStringLiteral("messages"), QJsonArray{QJsonObject{
                {QStringLiteral("messageId"), encoded.id},
                {QStringLiteral("traceId"), encoded.traceId},
                {QStringLiteral("timestamp"), encoded.timestamp.toString(Qt::ISODateWithMs)},
                {QStringLiteral("domainType"), Message::typeName(encoded.type)},
                {QStringLiteral("vmfMessage"), encoded.vmfMessage},
                {QStringLiteral("catalogId"),
                 encoded.payload.value(QStringLiteral("vmfCatalogId"))},
                {QStringLiteral("payload"), source.payload},
                {QStringLiteral("wireBytes"),
                 QString::fromLatin1(encoded.wireBytes.toBase64())},
                {QStringLiteral("wireBitLength"), encoded.wireBitLength},
                {QStringLiteral("correlationId"), encoded.correlationId}}});
        }
    }
    const QString requestId = m_networkClient.sendVmfTaskCommand(payload);
    if (requestId.isEmpty()) {
        return guidedStrikeResult(false, QStringLiteral("NETWORK_UNAVAILABLE"),
                                  QStringLiteral("联网会话尚未建立"));
    }
    return guidedStrikeResult(true, QStringLiteral("PENDING"),
                              QStringLiteral("严格 VMF 任务命令已发送，等待服务器回执"), requestId);
}

QVariantMap SimulationController::sendDemoAction(const QVariantMap& command) {
    if (!isNetworked() || m_isObserver || m_currentSeatSide != QLatin1String("red")) {
        return guidedStrikeResult(false, QStringLiteral("SEAT_REQUIRED"),
                                  QStringLiteral("演示动作只能由红方联网战位提交"));
    }
    if (m_protocolProfile != QLatin1String("vmf-demo-v2")) {
        return guidedStrikeResult(false, QStringLiteral("DEMO_PROFILE_REQUIRED"),
                                  QStringLiteral("当前房间未启用演示模式 v2"));
    }
    QJsonObject payload = QJsonObject::fromVariantMap(command);
    payload.insert(QStringLiteral("seat"), m_currentSeatId);
    if (!payload.contains(QStringLiteral("expectedRevision"))) {
        payload.insert(QStringLiteral("expectedRevision"),
                       m_remoteDemoState.value(QStringLiteral("revision")));
    }
    if (!payload.contains(QStringLiteral("phase"))) {
        payload.insert(QStringLiteral("phase"),
                       m_remoteDemoState.value(QStringLiteral("phase")));
    }
    if (!payload.contains(QStringLiteral("inputMode"))) {
        payload.insert(QStringLiteral("inputMode"), QStringLiteral("template"));
    }
    if (!payload.contains(QStringLiteral("payload"))) {
        payload.insert(QStringLiteral("payload"), QJsonObject{});
    }
    if (!payload.contains(QStringLiteral("actionId"))) {
        payload.insert(QStringLiteral("actionId"),
                       QStringLiteral("demo-action-%1")
                           .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    }
    const QString requestId = m_networkClient.sendDemoAction(payload);
    return guidedStrikeResult(!requestId.isEmpty(),
                              requestId.isEmpty() ? QStringLiteral("NETWORK_UNAVAILABLE")
                                                  : QStringLiteral("PENDING"),
                              requestId.isEmpty() ? QStringLiteral("联网会话尚未建立")
                                                  : QStringLiteral("演示动作已发送，等待服务器确认"),
                              requestId);
}

QVariantMap SimulationController::sendDemoControl(const QString& action,
                                                  const QVariantMap& payload) {
    if (!isNetworked() || (m_userRole != QLatin1String("admin")
                           && m_userRole != QLatin1String("room_admin")
                           && m_userRole != QLatin1String("director"))) {
        return guidedStrikeResult(false, QStringLiteral("DIRECTOR_REQUIRED"),
                                  QStringLiteral("演示控制需要管理员或导演权限"));
    }
    QJsonObject command{{QStringLiteral("expectedRevision"),
                         m_remoteDemoState.value(QStringLiteral("revision"))},
                        {QStringLiteral("action"), action},
                        {QStringLiteral("payload"), QJsonObject::fromVariantMap(payload)}};
    const QString requestId = m_networkClient.sendDemoControl(command);
    return guidedStrikeResult(!requestId.isEmpty(),
                              requestId.isEmpty() ? QStringLiteral("NETWORK_UNAVAILABLE")
                                                  : QStringLiteral("PENDING"),
                              requestId.isEmpty() ? QStringLiteral("联网会话尚未建立")
                                                  : QStringLiteral("导演控制已发送，等待服务器确认"),
                              requestId);
}

QJsonObject SimulationController::unitsJson() const {
    return ScenarioIo::toJson(m_engine.scenario());
}

QString SimulationController::upsertUnit(const QVariantMap& data) {
    ScenarioUnit u = scenarioUnitFromVariantMap(data, true);
    if (isNetworked()) {
        if (!canEditScenario()
            || (u.side != QLatin1String("red") && u.side != QLatin1String("blue"))) return {};
        m_networkClient.sendScenarioUpsert(scenarioUnitJson(u));
        return u.id;
    }
    m_engine.addOrUpdateUnit(u);
    invalidateCaches();
    return u.id;
}

bool SimulationController::replaceUnits(const QVariantList& units) {
    Scenario replacement = m_engine.scenario();
    replacement.units.clear();
    replacement.units.reserve(static_cast<size_t>(units.size()));
    for (const auto& value : units) {
        replacement.units.push_back(scenarioUnitFromVariantMap(value.toMap(), false));
    }
    if (isNetworked()) {
        if (!canEditScenario()) return false;
        m_networkClient.sendScenarioReplace(ScenarioIo::toJson(replacement));
        return true;
    }
    if (!m_engine.setScenario(replacement)) return false;
    invalidateCaches();
    ensureFocusedConsistent();
    return true;
}

bool SimulationController::replaceScenario(const QVariantMap& scenario) {
    const Scenario replacement = ScenarioIo::fromJson(QJsonObject::fromVariantMap(scenario));
    if (isNetworked()) {
        if (!canEditScenario()) return false;
        m_networkClient.sendScenarioReplace(ScenarioIo::toJson(replacement));
        return true;
    }
    if (!m_engine.setScenario(replacement)) return false;
    invalidateCaches();
    ensureFocusedConsistent();
    return true;
}

void SimulationController::removeUnit(const QString& id) {
    if (isNetworked()) {
        // The remote scenario is the source of truth during room setup. Its
        // units do not have to exist in the runtime projection yet.
        if (!canEditScenario() || id.trimmed().isEmpty()) return;
        m_networkClient.sendScenarioRemove(id);
        return;
    }
    m_engine.removeUnit(id);
    invalidateCaches();
    if (m_focusedUnitId == id) {
        m_focusedUnitId.clear();
        emit focusedUnitIdChanged();
    }
}

bool SimulationController::applyScenarioReplacement(const Scenario& replacement) {
    if (isNetworked()) {
        if (!canEditScenario()) return false;
        m_networkClient.sendScenarioReplace(ScenarioIo::toJson(replacement));
        return true;
    }
    if (!m_engine.setScenario(replacement)) return false;
    invalidateCaches();
    ensureFocusedConsistent();
    return true;
}

bool SimulationController::removeUnits(const QStringList& ids) {
    if (ids.isEmpty()) return false;
    if (isNetworked() && !canEditScenario()) return false;
    if (isNetworked()) {
        QSet<QString> selected;
        for (const QString& id : ids) {
            const QString normalized = id.trimmed();
            if (!normalized.isEmpty()) selected.insert(normalized);
        }
        if (selected.isEmpty()) return false;
        for (const QString& id : selected) m_networkClient.sendScenarioRemove(id);
        return true;
    }
    const QSet<QString> selected(ids.cbegin(), ids.cend());
    Scenario replacement = m_engine.scenario();
    std::erase_if(replacement.units, [&selected](const ScenarioUnit& unit) {
        return selected.contains(unit.id);
    });
    return applyScenarioReplacement(replacement);
}

bool SimulationController::batchUpdateUnits(const QStringList& ids,
                                            const QVariantMap& changes) {
    if (ids.isEmpty() || changes.isEmpty()) return false;
    if (isNetworked() && !canEditScenario()) return false;
    const QSet<QString> selected(ids.cbegin(), ids.cend());
    Scenario replacement = m_engine.scenario();
    const double offsetX = changes.value(QStringLiteral("offsetX"), 0.0).toDouble();
    const double offsetY = changes.value(QStringLiteral("offsetY"), 0.0).toDouble();
    static const QSet<QString> editableFields{
        QStringLiteral("side"), QStringLiteral("alt"), QStringLiteral("detectRange"),
        QStringLiteral("attackRange"), QStringLiteral("commRange"), QStringLiteral("speed"),
        QStringLiteral("maxHp"), QStringLiteral("armor"), QStringLiteral("repairRate"),
        QStringLiteral("subsystemRepairRate"), QStringLiteral("ammoCapacity"),
        QStringLiteral("initialAmmo"), QStringLiteral("hitProbability"),
        QStringLiteral("optimalRange"), QStringLiteral("minAttackRange"),
        QStringLiteral("cooldownSec"), QStringLiteral("damageMin"),
        QStringLiteral("damageMax"), QStringLiteral("rangeFalloff"),
        QStringLiteral("fuelCapacitySec"), QStringLiteral("initialFuelSec"),
        QStringLiteral("rearmDurationSec")};
    for (ScenarioUnit& unit : replacement.units) {
        if (!selected.contains(unit.id)) continue;
        QVariantMap data = scenarioUnitJson(unit).toVariantMap();
        data[QStringLiteral("x")] = unit.pos.x + offsetX;
        data[QStringLiteral("y")] = unit.pos.y + offsetY;
        for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
            if (editableFields.contains(it.key())) data[it.key()] = it.value();
        }
        unit = scenarioUnitFromVariantMap(data, false);
    }
    return applyScenarioReplacement(replacement);
}

bool SimulationController::transformUnits(const QStringList& ids,
                                          const QString& operation,
                                          double value) {
    if (ids.isEmpty()) return false;
    if (isNetworked() && !canEditScenario()) return false;
    const QSet<QString> selected(ids.cbegin(), ids.cend());
    Scenario replacement = m_engine.scenario();
    std::vector<ScenarioUnit*> units;
    for (ScenarioUnit& unit : replacement.units) {
        if (selected.contains(unit.id)) units.push_back(&unit);
    }
    if (units.empty()) return false;

    auto byX = [](const ScenarioUnit* a, const ScenarioUnit* b) {
        return a->pos.x < b->pos.x || (a->pos.x == b->pos.x && a->id < b->id);
    };
    auto byY = [](const ScenarioUnit* a, const ScenarioUnit* b) {
        return a->pos.y < b->pos.y || (a->pos.y == b->pos.y && a->id < b->id);
    };
    if (operation == QLatin1String("snap")) {
        if (!std::isfinite(value) || value <= 0.0) return false;
        for (ScenarioUnit* unit : units) {
            unit->pos.x = std::round(unit->pos.x / value) * value;
            unit->pos.y = std::round(unit->pos.y / value) * value;
        }
    } else if (operation.startsWith(QLatin1String("align"))) {
        double target = 0.0;
        if (operation == QLatin1String("alignLeft")) {
            target = (*std::min_element(units.begin(), units.end(), byX))->pos.x;
            for (ScenarioUnit* unit : units) unit->pos.x = target;
        } else if (operation == QLatin1String("alignRight")) {
            target = (*std::max_element(units.begin(), units.end(), byX))->pos.x;
            for (ScenarioUnit* unit : units) unit->pos.x = target;
        } else if (operation == QLatin1String("alignTop")) {
            target = (*std::min_element(units.begin(), units.end(), byY))->pos.y;
            for (ScenarioUnit* unit : units) unit->pos.y = target;
        } else if (operation == QLatin1String("alignBottom")) {
            target = (*std::max_element(units.begin(), units.end(), byY))->pos.y;
            for (ScenarioUnit* unit : units) unit->pos.y = target;
        } else if (operation == QLatin1String("alignCenterX")) {
            for (const ScenarioUnit* unit : units) target += unit->pos.x;
            target /= static_cast<double>(units.size());
            for (ScenarioUnit* unit : units) unit->pos.x = target;
        } else if (operation == QLatin1String("alignCenterY")) {
            for (const ScenarioUnit* unit : units) target += unit->pos.y;
            target /= static_cast<double>(units.size());
            for (ScenarioUnit* unit : units) unit->pos.y = target;
        } else return false;
    } else if (operation == QLatin1String("distributeX")
               || operation == QLatin1String("distributeY")) {
        if (units.size() < 3) return false;
        auto comparator = operation == QLatin1String("distributeX") ? byX : byY;
        std::sort(units.begin(), units.end(), comparator);
        const double first = operation == QLatin1String("distributeX")
            ? units.front()->pos.x : units.front()->pos.y;
        const double last = operation == QLatin1String("distributeX")
            ? units.back()->pos.x : units.back()->pos.y;
        const double spacing = (last - first) / static_cast<double>(units.size() - 1);
        for (size_t index = 1; index + 1 < units.size(); ++index) {
            if (operation == QLatin1String("distributeX")) units[index]->pos.x = first + spacing * index;
            else units[index]->pos.y = first + spacing * index;
        }
    } else return false;
    return applyScenarioReplacement(replacement);
}

QVariantList SimulationController::copyUnits(const QStringList& ids) const {
    const QSet<QString> selected(ids.cbegin(), ids.cend());
    QVariantList copied;
    for (const ScenarioUnit& unit : m_engine.scenario().units) {
        if (selected.contains(unit.id)) copied.append(scenarioUnitJson(unit).toVariantMap());
    }
    return copied;
}

QStringList SimulationController::pasteUnits(const QVariantList& copied, double offsetX,
                                             double offsetY,
                                             const QString& sideOverride) {
    if (copied.isEmpty() || !std::isfinite(offsetX) || !std::isfinite(offsetY)) return {};
    if (isNetworked() && !canEditScenario()) return {};
    Scenario replacement = m_engine.scenario();
    QSet<QString> existing;
    for (const ScenarioUnit& unit : replacement.units) existing.insert(unit.id);
    QStringList created;
    std::vector<ScenarioUnit> newUnits;
    const qint64 stamp = QDateTime::currentMSecsSinceEpoch();
    int serial = 0;
    for (const QVariant& value : copied) {
        QVariantMap data = value.toMap();
        QString id;
        do {
            id = QStringLiteral("u_%1_%2").arg(stamp).arg(++serial);
        } while (existing.contains(id));
        data[QStringLiteral("id")] = id;
        data[QStringLiteral("callsign")] = data.value(QStringLiteral("callsign")).toString()
            + QStringLiteral(" 副本");
        data[QStringLiteral("x")] = data.value(QStringLiteral("x")).toDouble() + offsetX;
        data[QStringLiteral("y")] = data.value(QStringLiteral("y")).toDouble() + offsetY;
        if (sideOverride == QLatin1String("red") || sideOverride == QLatin1String("blue")) {
            data[QStringLiteral("side")] = sideOverride;
        }
        ScenarioUnit unit = scenarioUnitFromVariantMap(data, false);
        replacement.units.push_back(unit);
        newUnits.push_back(unit);
        existing.insert(id);
        created.append(id);
    }
    if (!applyScenarioReplacement(replacement)) return {};
    return created;
}

QVariantList SimulationController::scenarioValidationIssues() const {
    QVariantList issues;
    auto add = [&issues](const QString& severity, const QString& code,
                         const QString& message, const QString& unitId = QString()) {
        issues.append(QVariantMap{{QStringLiteral("severity"), severity},
                                  {QStringLiteral("code"), code},
                                  {QStringLiteral("message"), message},
                                  {QStringLiteral("unitId"), unitId}});
    };
    QSet<QString> ids;
    int redCp = 0;
    int blueCp = 0;
    const Scenario& scenario = m_engine.scenario();
    for (const ScenarioUnit& unit : scenario.units) {
        if (ids.contains(unit.id)) add(QStringLiteral("error"), QStringLiteral("duplicate_id"),
                                      QStringLiteral("ID 重复"), unit.id);
        ids.insert(unit.id);
        if (unit.callsign.trimmed().isEmpty()) add(QStringLiteral("warning"), QStringLiteral("empty_callsign"),
                                                   QStringLiteral("呼号为空"), unit.id);
        if (unit.kind == QLatin1String("commandpost")) {
            if (unit.side == QLatin1String("red")) ++redCp;
            else if (unit.side == QLatin1String("blue")) ++blueCp;
        }
        if (unit.pos.x < 0.0 || unit.pos.y < 0.0
            || unit.pos.x > scenario.map.widthMeters || unit.pos.y > scenario.map.heightMeters) {
            add(QStringLiteral("error"), QStringLiteral("out_of_bounds"),
                QStringLiteral("单元超出地图边界"), unit.id);
        }
        for (const SchedulePoint& point : unit.schedule) {
            if (point.x < 0.0 || point.y < 0.0 || point.x > scenario.map.widthMeters
                || point.y > scenario.map.heightMeters) {
                add(QStringLiteral("error"), QStringLiteral("route_out_of_bounds"),
                    QStringLiteral("路径点超出地图边界"), unit.id);
                break;
            }
        }
    }
    if (redCp != 1) add(QStringLiteral("error"), QStringLiteral("red_cp_count"),
                        QStringLiteral("红方必须恰好有 1 个指挥所，当前 %1 个").arg(redCp));
    if (blueCp != 1) add(QStringLiteral("error"), QStringLiteral("blue_cp_count"),
                         QStringLiteral("蓝方必须恰好有 1 个指挥所，当前 %1 个").arg(blueCp));
    return issues;
}

QVariantList SimulationController::unitTemplates() const {
    auto make = [this](const QString& name, const QString& kind, double altitude) {
        ScenarioUnit unit;
        unit.callsign = name;
        unit.kind = kind;
        unit.side = QStringLiteral("red");
        unit.pos.alt = altitude;
        if (kind == QLatin1String("attackuav")) {
            unit.detectRange = 4000; unit.attackRange = 2500; unit.commRange = 15000;
            unit.speed = 200; unit.maxHp = 120; unit.optimalRange = 1800;
            unit.damageMin = 80; unit.damageMax = 120; unit.attackPower = 100;
        } else if (kind == QLatin1String("reconuav")) {
            unit.detectRange = 8000; unit.attackRange = 0; unit.speed = 150;
        } else if (kind == QLatin1String("jammeruav")) {
            unit.detectRange = 6000; unit.attackRange = 0; unit.speed = 120; unit.maxHp = 80;
        } else {
            unit.detectRange = 3000; unit.attackRange = 0; unit.commRange = 10000;
            unit.speed = 18; unit.maxHp = 80;
        }
        QVariantMap result = scenarioUnitJson(unit).toVariantMap();
        result[QStringLiteral("templateName")] = name;
        return result;
    };
    return {make(QStringLiteral("侦察无人机"), QStringLiteral("reconuav"), 3000),
            make(QStringLiteral("攻击无人机"), QStringLiteral("attackuav"), 2000),
            make(QStringLiteral("电子干扰机"), QStringLiteral("jammeruav"), 4000),
            make(QStringLiteral("地面侦察分队"), QStringLiteral("groundscout"), 0)};
}

Q_INVOKABLE void SimulationController::setUnitSchedule(const QString& uid, const QVariantList& schedule) {
    if (isNetworked()) {
        if (m_isObserver) return;
        if (m_matchPhase == QLatin1String("running")) {
            m_networkClient.sendCommand(QStringLiteral("setSchedule"),
                                        QVariantMap{{QStringLiteral("unitId"), uid},
                                                    {QStringLiteral("schedule"), schedule}});
            return;
        }
        if (!canEditScenario()) return;
        for (const auto& unit : m_engine.scenario().units) {
            if (unit.id != uid) continue;
            QVariantMap data = scenarioUnitJson(unit).toVariantMap();
            data[QStringLiteral("schedule")] = schedule;
            m_networkClient.sendScenarioUpsert(QJsonObject::fromVariantMap(data));
            return;
        }
        return;
    }
    m_engine.command(QStringLiteral("setSchedule"),
                     QVariantMap{{QStringLiteral("unitId"), uid},
                                 {QStringLiteral("schedule"), schedule}});
}

bool SimulationController::seekReplay(double targetTime) {
    if (isNetworked()) {
        emit errorForward(QStringLiteral("联网席位不能在本地改写权威回放状态"));
        return false;
    }
    QString error;
    if (!m_engine.seekReplay(targetTime, &error)) {
        emit errorForward(error);
        return false;
    }
    invalidateCaches();
    return true;
}

bool SimulationController::stepReplayEvent(int direction) {
    if (direction == 0 || isNetworked()) return false;
    const QJsonArray events = m_engine.timelineEvents();
    const double current = m_engine.simTime();
    double target = direction > 0 ? m_engine.replayDuration() : 0.0;
    bool found = false;
    if (direction > 0) {
        for (const QJsonValue& value : events) {
            const double time = value.toObject().value(QStringLiteral("simTime")).toDouble();
            if (time > current + 1e-6 && (!found || time < target)) {
                target = time;
                found = true;
            }
        }
    } else {
        for (const QJsonValue& value : events) {
            const double time = value.toObject().value(QStringLiteral("simTime")).toDouble();
            if (time < current - 1e-6 && (!found || time > target)) {
                target = time;
                found = true;
            }
        }
    }
    return seekReplay(target);
}

QJsonObject SimulationController::battleReport() const {
    return isNetworked() ? QJsonObject{} : m_engine.battleReport();
}

QString SimulationController::exportBattleReport(const QString& requestedPath,
                                                  const QString& requestedFormat) {
    if (isNetworked()) {
        emit errorForward(QStringLiteral("联网战报应由权威服务器导出"));
        return {};
    }
    const QString format = requestedFormat.toLower() == QLatin1String("csv")
        ? QStringLiteral("csv") : QStringLiteral("json");
    QString path = requestedPath.trimmed();
    if (path.isEmpty()) {
        const QString directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            + QStringLiteral("/WargameReports");
        QDir().mkpath(directory);
        path = directory + QStringLiteral("/battle-report-%1.%2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")), format);
    }
    if (!path.endsWith(QLatin1Char('.') + format, Qt::CaseInsensitive)) {
        path += QLatin1Char('.') + format;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        emit errorForward(QStringLiteral("无法创建战报目录"));
        return {};
    }

    const QJsonObject report = m_engine.battleReport();
    QByteArray data;
    if (format == QLatin1String("json")) {
        data = QJsonDocument(report).toJson(QJsonDocument::Indented);
    } else {
        auto quote = [](QString value) {
            value.replace(QLatin1Char('"'), QStringLiteral("\"\""));
            return QStringLiteral("\"") + value + QStringLiteral("\"");
        };
        QString csv = QStringLiteral("sequence,simTime,category,level,title,details\n");
        for (const QJsonValue& value : report.value(QStringLiteral("events")).toArray()) {
            const QJsonObject event = value.toObject();
            csv += QStringLiteral("%1,%2,%3,%4,%5,%6\n")
                .arg(event.value(QStringLiteral("sequence")).toInteger())
                .arg(event.value(QStringLiteral("simTime")).toDouble(), 0, 'f', 3)
                .arg(quote(event.value(QStringLiteral("category")).toString()),
                     quote(event.value(QStringLiteral("level")).toString()),
                     quote(event.value(QStringLiteral("title")).toString()),
                     quote(QString::fromUtf8(QJsonDocument(
                         event.value(QStringLiteral("details")).toObject())
                                                .toJson(QJsonDocument::Compact))));
        }
        data = csv.toUtf8();
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        emit errorForward(QStringLiteral("战报写入失败: %1").arg(path));
        return {};
    }
    return path;
}

QJsonArray SimulationController::perceptionForSide(const QString& side) const {
    return m_engine.collectPerceptionSnapshot(side);
}

QJsonArray SimulationController::allUnits() const {
    if (!m_snapshotCacheValid) {
        m_snapshotCache = m_engine.collectAllUnitsSnapshot();
        m_snapshotCacheValid = true;
    }
    return m_snapshotCache;
}

QVariantList SimulationController::units() const {
    if (!m_unitsViewCacheValid) {
        m_unitsViewCache = m_engine.unitsForView();
        m_unitsViewCacheValid = true;
    }
    return m_unitsViewCache;
}

void SimulationController::invalidateCaches() {
    m_cpCache.clear();
    m_snapshotCacheValid = false;
    m_unitsViewCacheValid = false;
}

QJsonObject SimulationController::unitAt(const QString& id) const {
    return m_engine.unitSnapshot(id);
}

QVariantList SimulationController::unitOptions(const QString& kindFilter, const QString& sideFilter) const {
    QVariantList out;
    for (const auto& u : m_engine.scenario().units) {
        if (!kindFilter.isEmpty() && u.kind != kindFilter) continue;
        if (!sideFilter.isEmpty() && u.side != sideFilter) continue;
        auto* pu = m_engine.unit(u.id);
        if (!pu || !pu->alive()) continue;
        QVariantMap m;
        m["id"] = u.id;
        m["callsign"] = u.callsign;
        m["kind"] = u.kind;
        m["side"] = u.side;
        m["movable"] = u.kind != "commandpost";
        out.append(m);
    }
    return out;
}

QStringList SimulationController::viewModeOptions() const {
    return {
        "editor",
        "commandpost-red",
        "commandpost-blue",
        "director"
    };
}

QString SimulationController::commandPostIdFor(const QString& side) const {
    auto it = m_cpCache.find(side);
    if (it != m_cpCache.end()) return it.value();
    for (const auto& u : m_engine.scenario().units) {
        if (u.kind != QLatin1String("commandpost")) continue;
        if (u.side != side) continue;
        auto* pu = m_engine.unit(u.id);
        if (pu && pu->alive()) {
            m_cpCache[side] = u.id;
            return u.id;
        }
    }
    QString fallback = side == QLatin1String("red") ? QStringLiteral("red_cp") : QStringLiteral("blue_cp");
    m_cpCache[side] = fallback;
    return fallback;
}

QVariantList SimulationController::attackableTargets(const QString& attackerId, const QString& enemySide) const {
    QVariantList out;
    auto* atk = m_engine.unit(attackerId);
    if (!atk || !atk->alive()) return out;
    const double atkRange = atk->attackRange();
    const GeoPos atkPos = atk->pos();
    const auto all = m_engine.collectAllUnitsSnapshot();
    for (const auto& v : all) {
        const auto e = v.toObject();
        if (e.value("side").toString() != enemySide) continue;
        if (!e.value("alive").toBool()) continue;
        QJsonArray epos = e.value("position").toArray();
        if (epos.size() < 2) continue;
        const double dx = atkPos.x - epos.at(0).toDouble();
        const double dy = atkPos.y - epos.at(1).toDouble();
        if (std::sqrt(dx*dx + dy*dy) <= atkRange) {
            QVariantMap m;
            m["id"] = e["id"]; m["callsign"] = e["callsign"];
            m["kind"] = e["kind"]; m["side"] = e["side"];
            out.append(m);
        }
    }
    return out;
}

QVariantList SimulationController::detectedEnemyOptions(const QString& attackerId, const QString& friendlySide, const QString& enemySide) const {
    QVariantList out;
    QSet<QString> added;
    const auto allUnits = m_engine.collectAllUnitsSnapshot();
    if (isNetworked()) {
        QHash<QString, QJsonObject> unitsById;
        for (const QJsonValue& value : allUnits) {
            const QJsonObject unit = value.toObject();
            unitsById.insert(unit.value(QStringLiteral("id")).toString(), unit);
        }
        const auto appendTarget = [&out, &added, &unitsById, &enemySide](
                                      const QString& targetId) {
            const QJsonObject unit = unitsById.value(targetId);
            if (targetId.isEmpty() || added.contains(targetId) || unit.isEmpty()
                || !unit.value(QStringLiteral("alive")).toBool()
                || unit.value(QStringLiteral("side")).toString() != enemySide) {
                return;
            }
            QVariantMap option;
            option[QStringLiteral("id")] = targetId;
            option[QStringLiteral("callsign")] = unit.value(QStringLiteral("callsign")).toVariant();
            option[QStringLiteral("kind")] = unit.value(QStringLiteral("kind")).toVariant();
            option[QStringLiteral("side")] = unit.value(QStringLiteral("side")).toVariant();
            out.append(option);
            added.insert(targetId);
        };
        for (const QVariant& value : m_onlineIntelRecords) {
            const QVariantMap contact = value.toMap();
            if (!contact.value(QStringLiteral("actionable")).toBool()
                || contact.value(QStringLiteral("type")).toString()
                    != QLatin1String("sensorContact")) {
                continue;
            }
            const QString targetId = contact.value(QStringLiteral("targetId")).toString();
            appendTarget(targetId);
        }
        for (auto it = unitsById.cbegin(); it != unitsById.cend(); ++it) {
            if (it->value(QStringLiteral("status")).toString() == QStringLiteral("已探测")) {
                appendTarget(it.key());
            }
        }
        return out;
    }
    // gather friendly recon positions
    QVariantList reconList;
    for (const auto& v : allUnits) {
        const auto o = v.toObject();
        if (o.value("side").toString() == friendlySide && o.value("kind").toString() == QLatin1String("reconuav") && o.value("alive").toBool())
            reconList.append(v.toVariant());
    }
    // attacker info
    double atkRange = -1.0;
    QJsonArray atkPosArr;
    if (!attackerId.isEmpty()) {
        auto* atk = m_engine.unit(attackerId);
        if (atk && atk->alive() && atk->position().size() >= 2) {
            atkRange = atk->attackRange();
            atkPosArr = QJsonArray{atk->position().at(0).toDouble(), atk->position().at(1).toDouble()};
        }
    }
    for (const auto& v : allUnits) {
        const auto e = v.toObject();
        if (e.value("side").toString() != enemySide) continue;
        if (!e.value("alive").toBool()) continue;
        if (added.contains(e.value("id").toString())) continue;
        QJsonArray epos = e.value("position").toArray();
        if (epos.size() < 2) continue;
        const double ex = epos.at(0).toDouble(), ey = epos.at(1).toDouble();
        bool found = false;
        for (const auto& rv : reconList) {
            const auto r = rv.toJsonObject();
            const auto rpos = r.value("position").toArray();
            if (rpos.size() < 2) continue;
            const double dx = rpos.at(0).toDouble() - ex;
            const double dy = rpos.at(1).toDouble() - ey;
            if (std::sqrt(dx*dx + dy*dy) <= r.value("detectRange").toDouble(0)) { found = true; break; }
        }
        if (!found && atkRange > 0 && !atkPosArr.isEmpty()) {
            const double adx = atkPosArr.at(0).toDouble() - ex;
            const double ady = atkPosArr.at(1).toDouble() - ey;
            if (std::sqrt(adx*adx + ady*ady) <= atkRange) found = true;
        }
        if (found) {
            QVariantMap m;
            m["id"] = e["id"]; m["callsign"] = e["callsign"];
            m["kind"] = e["kind"]; m["side"] = e["side"];
            out.append(m);
            added.insert(e.value("id").toString());
        }
    }
    return out;
}

bool SimulationController::hasTargetInAttackRange(const QString& unitId, const QString& enemySide) const {
    auto* u = m_engine.unit(unitId);
    if (!u || u->kindStr() != QLatin1String("attackuav") || !u->alive()) return false;
    const double atkRange = u->attackRange();
    const GeoPos myPos = u->pos();
    const auto all = m_engine.collectAllUnitsSnapshot();
    for (const auto& v : all) {
        const auto e = v.toObject();
        if (e.value("side").toString() != enemySide) continue;
        if (!e.value("alive").toBool()) continue;
        QJsonArray epos = e.value("position").toArray();
        if (epos.size() < 2) continue;
        const double dx = myPos.x - epos.at(0).toDouble();
        const double dy = myPos.y - epos.at(1).toDouble();
        if (std::sqrt(dx*dx + dy*dy) <= atkRange) return true;
    }
    return false;
}

bool SimulationController::hasTargetInDetectShared(const QString& unitId, const QString& friendlySide, const QString& enemySide) const {
    auto* u = m_engine.unit(unitId);
    if (!u || u->kindStr() != QLatin1String("groundscout") || !u->alive()) return false;
    const auto allUnits = m_engine.collectAllUnitsSnapshot();
    QVariantList reconList;
    for (const auto& v : allUnits) {
        const auto o = v.toObject();
        if (o.value("side").toString() == friendlySide && o.value("kind").toString() == QLatin1String("reconuav") && o.value("alive").toBool())
            reconList.append(v.toVariant());
    }
    for (const auto& v : allUnits) {
        const auto e = v.toObject();
        if (e.value("side").toString() != enemySide) continue;
        if (!e.value("alive").toBool()) continue;
        QJsonArray epos = e.value("position").toArray();
        if (epos.size() < 2) continue;
        const double ex = epos.at(0).toDouble(), ey = epos.at(1).toDouble();
        for (const auto& rv : reconList) {
            const auto r = rv.toJsonObject();
            const auto rpos = r.value("position").toArray();
            if (rpos.size() < 2) continue;
            const double dx = rpos.at(0).toDouble() - ex;
            const double dy = rpos.at(1).toDouble() - ey;
            if (std::sqrt(dx*dx + dy*dy) <= r.value("detectRange").toDouble(0)) return true;
        }
    }
    return false;
}

QStringList SimulationController::detectedEnemyIds(const QString& friendlySide) const {
    QSet<QString> ids;
    const auto allUnits = m_engine.collectAllUnitsSnapshot();

    // 1) Direct detection: any friendly unit's detectRange covers the enemy
    for (const auto& v : allUnits) {
        const auto fu = v.toObject();
        if (fu.value("side").toString() != friendlySide) continue;
        if (!fu.value("alive").toBool()) continue;
        QJsonArray fpos = fu.value("position").toArray();
        if (fpos.size() < 2) continue;
        const double fx = fpos.at(0).toDouble(), fy = fpos.at(1).toDouble();
        const double fdr = fu.value("detectRange").toDouble(0);
        for (const auto& v2 : allUnits) {
            const auto eu = v2.toObject();
            if (eu.value("side").toString() == friendlySide) continue;
            if (!eu.value("alive").toBool()) continue;
            QJsonArray epos = eu.value("position").toArray();
            if (epos.size() < 2) continue;
            const double dx = fx - epos.at(0).toDouble();
            const double dy = fy - epos.at(1).toDouble();
            if (std::sqrt(dx*dx + dy*dy) <= fdr)
                ids.insert(eu.value("id").toString());
        }
    }

    // 2) Shared knowledge: any friendly unit stored SharedDetect info
    for (const auto& v : allUnits) {
        const auto fu = v.toObject();
        if (fu.value("side").toString() != friendlySide) continue;
        const auto sk = fu.value("sharedKnowledge").toObject();
        for (auto it = sk.begin(); it != sk.end(); ++it) {
            const QString key = it.key();
            if (key.startsWith(QLatin1String("shared:detect:"))) {
                const auto info = it.value().toObject();
                const QString tid = info.value("targetId").toString();
                if (!tid.isEmpty()) ids.insert(tid);
            }
        }
    }

    return {ids.begin(), ids.end()};
}

QString SimulationController::pickDefaultUnit(const QString& kind, const QString& side) const {
    for (const auto& u : m_engine.scenario().units) {
        if (u.side != side || u.kind != kind) continue;
        auto* runtimeUnit = m_engine.unit(u.id);
        if (runtimeUnit && runtimeUnit->alive()) return u.id;
    }
    return QString();
}

void SimulationController::ensureFocusedConsistent() {
    if (m_viewMode == "commandpost-red") m_focusedSide = "red";
    if (m_viewMode == "commandpost-blue") m_focusedSide = "blue";

    if (m_viewMode == "commandpost-red" || m_viewMode == "commandpost-blue") {
        // Remote snapshots arrive throughout a simulation. Preserve a valid
        // user selection; the command post is only a fallback for missing,
        // destroyed, or opposite-side focused units.
        const UnitBase* focused = m_engine.unit(m_focusedUnitId);
        if (focused && focused->alive() && focused->sideStr() == m_focusedSide)
            return;
        QString id = pickDefaultUnit("commandpost", m_focusedSide);
        if (m_focusedUnitId != id) {
            m_focusedUnitId = id;
            emit focusedUnitIdChanged();
        }
        return;
    }
}

void SimulationController::saveSetting(const QString& key, const QVariant& value) {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(dir)) {
        emit errorForward(QStringLiteral("保存设置失败: %1").arg(key));
        return;
    }
    const QString path = dir + "/settings.json";
    QJsonObject obj;
    QFile existing(path);
    if (existing.open(QIODevice::ReadOnly)) {
        obj = QJsonDocument::fromJson(existing.readAll()).object();
    }
    if (key == QLatin1String("network/password")) obj.remove(key);
    else obj[key] = QJsonValue::fromVariant(value);
    const QByteArray data = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) || output.write(data) != data.size()
        || !output.commit()) {
        emit errorForward(QStringLiteral("保存设置失败: %1").arg(key));
        return;
    }
    if (key == QLatin1String("network/password")) {
        emit errorForward(QStringLiteral("密码仅能存储在系统密钥链中"));
        return;
    }
    emit settingChanged(key);
    if (key.startsWith(QStringLiteral("shortcuts/"))) emit shortcutsChanged();
}

void SimulationController::saveRememberedPassword(const QString& server, const QString& username,
                                                  const QString& password, bool remember) {
    if (server.trimmed().isEmpty() || username.trimmed().isEmpty()) return;
    QKeychain::Job* job = nullptr;
    if (remember) {
        auto* write = new QKeychain::WritePasswordJob(QString::fromLatin1(kPasswordService), this);
        write->setTextData(password);
        job = write;
    } else {
        job = new QKeychain::DeletePasswordJob(QString::fromLatin1(kPasswordService), this);
    }
    job->setInsecureFallback(false);
    job->setKey(rememberedPasswordKey(server, username));
    connect(job, &QKeychain::Job::finished, this, [this, remember](QKeychain::Job* completed) {
        if (completed->error() == QKeychain::NoError
            || (!remember && completed->error() == QKeychain::EntryNotFound)) return;
        emit errorForward(QStringLiteral("无法更新系统密钥链中的登录密码: %1")
                              .arg(completed->errorString()));
    });
    job->start();
}

void SimulationController::loadRememberedPassword(const QString& server, const QString& username) {
    if (server.trimmed().isEmpty() || username.trimmed().isEmpty()) {
        emit rememberedPasswordLoaded(QString());
        return;
    }
    auto* job = new QKeychain::ReadPasswordJob(QString::fromLatin1(kPasswordService), this);
    job->setInsecureFallback(false);
    job->setKey(rememberedPasswordKey(server, username));
    connect(job, &QKeychain::Job::finished, this, [this](QKeychain::Job* completed) {
        auto* read = qobject_cast<QKeychain::ReadPasswordJob*>(completed);
        if (completed->error() == QKeychain::NoError && read) {
            emit rememberedPasswordLoaded(read->textData());
            return;
        }
        if (completed->error() != QKeychain::EntryNotFound) {
            emit errorForward(QStringLiteral("无法读取系统密钥链中的登录密码: %1")
                                  .arg(completed->errorString()));
        }
        emit rememberedPasswordLoaded(QString());
    });
    job->start();
}

QVariant SimulationController::loadSetting(const QString& key, const QVariant& defaultValue) const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString path = dir + "/settings.json";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return defaultValue;
    QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    if (!obj.contains(key)) return defaultValue;
    return obj[key].toVariant();
}

void SimulationController::useLocalMode() {
    const bool wasNetworked = isNetworked();
    const bool wasLeaveRoomPending = m_leaveRoomPending;
    m_networkClient.close();
    if (wasNetworked && !m_savedLocalScenario.units.empty()) m_engine.setScenario(m_savedLocalScenario);
    m_sessionMode = QStringLiteral("local");
    m_networkState = QStringLiteral("disconnected");
    m_networkStatus = QStringLiteral("本地模式");
    m_username.clear();
    m_displayName.clear();
    m_userRole.clear();
    m_serverAddress.clear();
    m_remoteMessages.clear();
    const bool hadVmfWorkflow = !m_remoteVmfWorkflow.isEmpty();
    m_remoteVmfWorkflow = {};
    m_remoteVmfTasks = {};
    m_remoteVmfTrace = {};
    m_remoteDemoState = {};
    m_protocolProfile = QStringLiteral("native");
    m_operationMode = QStringLiteral("standard");
    m_participantSide.clear();
    m_fixedTargetSide.clear();
    m_serverScenarioEditable = false;
    m_vmfAutomation = {};
    m_remoteProjectiles.clear();
    m_chatMessages.clear();
    m_remoteScenarioRevision = -1;
    m_lastCommandId.clear();
    m_lastCommandCode.clear();
    m_lastCommandAction.clear();
    m_lastCommandStatus.clear();
    m_lastCommandMessage.clear();
    m_onlineRooms.clear();
    m_onlineSeats.clear();
    m_onlineMapMarks.clear();
    m_onlineIntelRecords.clear();
    m_onlineIntelRevision = 0;
    m_onlineIntelShareTargets.clear();
    resetOnlineIntelHistory();
    const bool hadObserverTrajectories = !m_observerTrajectories.isEmpty();
    m_observerTrajectories = {};
    m_pendingSeatTransfers.clear();
    m_communicationState = QStringLiteral("disconnected");
    m_roomName.clear();
    m_roomDescription.clear();
    m_scenarioId = QStringLiteral("default");
    m_onlineSeatLimits = {};
    m_onlineSeatParameters = {};
    m_currentRoomId.clear();
    m_currentSeatId.clear();
    m_currentSeatType.clear();
    m_currentSeatSide.clear();
    m_onlineStage = QStringLiteral("login");
    m_isObserver = false;
    m_observerJoinPending = false;
    m_seatReady = false;
    m_leaveRoomPending = false;
    emit sessionChanged();
    emit networkStatusChanged();
    emit messagesForward();
    if (hadVmfWorkflow) emit vmfWorkflowChanged();
    emit vmfTasksChanged();
    emit vmfTraceChanged();
    emit demoStateChanged();
    emit projectilesForward();
    emit chatMessagesChanged();
    emit commandStatusChanged();
    emit onlineRoomsChanged();
    emit onlineSeatsChanged();
    emit onlineMapMarksChanged();
    if (hadObserverTrajectories) emit observerTrajectoriesChanged();
    emit pendingSeatTransfersChanged();
    emit onlineStateChanged();
    if (wasLeaveRoomPending) emit leaveRoomPendingChanged();
    if (wasNetworked) emit networkedChanged();
    ensureFocusedConsistent();
}

void SimulationController::loginOnline(const QString& server, const QString& username,
                                       const QString& password) {
    if (!isNetworked()) m_savedLocalScenario = m_engine.scenario();
    m_networkClient.login(server, username, password);
}

void SimulationController::diagnoseServer(const QString& server) {
    m_networkClient.diagnoseServer(server);
}

void SimulationController::rememberServerAddress(const QString& server) {
    QString normalized = server.trimmed();
    if (normalized.isEmpty()) return;
    if (!normalized.contains(QStringLiteral("://"))) normalized.prepend(QStringLiteral("http://"));
    QUrl url(normalized);
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme().toLower() != QLatin1String("http")
            && url.scheme().toLower() != QLatin1String("https"))) return;
    normalized = url.toString(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment);
    while (normalized.endsWith(QLatin1Char('/'))) normalized.chop(1);
    if (normalized.isEmpty()) return;

    m_serverHistory.removeAll(normalized);
    m_serverHistory.prepend(normalized);
    while (m_serverHistory.size() > 6) m_serverHistory.removeLast();
    saveSetting(QStringLiteral("network/server"), normalized);
    saveSetting(QStringLiteral("network/serverHistory"), QVariant::fromValue(m_serverHistory));
    emit serverHistoryChanged();
}

void SimulationController::logoutOnline() {
    const bool wasNetworked = isNetworked();
    useLocalMode();
    m_sessionMode = QStringLiteral("unselected");
    m_networkStatus = QStringLiteral("请选择运行模式");
    emit sessionChanged();
    emit roomStateChanged();
    emit networkStatusChanged();
    if (!wasNetworked) emit networkedChanged();
}

void SimulationController::setReady(bool ready) {
    setSeatReady(ready);
}

void SimulationController::endMatch() {
    if (isNetworked()) {
        emit errorForward(QStringLiteral("联网推演由网页管理员停止或重置"));
    }
}

void SimulationController::sendChat(const QString& text, const QStringList& recipientSeatIds) {
    if (isNetworked() && !m_isObserver && !text.trimmed().isEmpty()) {
        m_networkClient.sendChat(text.trimmed(), recipientSeatIds);
    }
}

void SimulationController::sendUnitOrder(const QString& unitId, const QString& text) {
    const QVariantMap args{{QStringLiteral("unitId"), unitId},
                           {QStringLiteral("text"), text.trimmed()}};
    if (isNetworked()) {
        if (m_isObserver) return;
        m_networkClient.sendUnitOrder(unitId, text);
    }
    else m_engine.command(QStringLiteral("unitOrder"), args);
    emit commandExecuted(QStringLiteral("unitOrder"), args);
}

void SimulationController::requestOnlineRooms() {
    if (!isNetworked()) return;
    if (!m_isObserver && !m_observerJoinPending) m_onlineStage = QStringLiteral("roomSelect");
    m_networkClient.requestRooms();
    emit onlineStateChanged();
}

void SimulationController::joinOnlineRoom(const QString& roomId) {
    if (!isNetworked() || m_isObserver || m_observerJoinPending || roomId.trimmed().isEmpty()) return;
    m_networkClient.joinRoom(roomId.trimmed());
}

void SimulationController::observeOnlineRoom(const QString& roomId) {
    const QString normalizedRoomId = roomId.trimmed();
    if (!isNetworked() || m_isObserver || m_observerJoinPending || normalizedRoomId.isEmpty()) return;
    m_observerJoinPending = true;
    m_isObserver = true;
    m_currentRoomId = normalizedRoomId;
    m_currentSeatId.clear();
    m_currentSeatType.clear();
    m_currentSeatSide.clear();
    m_onlineStage = QStringLiteral("observer");
    emit onlineStateChanged();
    m_networkClient.observeRoom(normalizedRoomId);
}

void SimulationController::claimOnlineSeat(const QString& seatId) {
    if (!isNetworked() || m_isObserver || m_observerJoinPending || seatId.trimmed().isEmpty()) return;
    // The server is authoritative. Do not show an occupied seat as selected
    // before the seatState response confirms the claim.
    m_networkClient.claimSeat(seatId.trimmed());
}

void SimulationController::approveSeatTransfer(qint64 userId, qint64 requestedRevision) {
    if (!isNetworked() || m_isObserver || m_currentSeatType != QLatin1String("commander")
        || userId <= 0 || requestedRevision <= 0) return;
    const QVariantMap args{{QStringLiteral("userId"), userId},
                           {QStringLiteral("requestedRevision"), requestedRevision}};
    m_networkClient.approveSeatTransfer(m_currentSeatId, userId, requestedRevision);
    emit commandExecuted(QStringLiteral("approveSeatTransfer"), args);
}

void SimulationController::rejectSeatTransfer(qint64 userId, qint64 requestedRevision) {
    if (!isNetworked() || m_isObserver || m_currentSeatType != QLatin1String("commander")
        || userId <= 0 || requestedRevision <= 0) return;
    const QVariantMap args{{QStringLiteral("userId"), userId},
                           {QStringLiteral("requestedRevision"), requestedRevision}};
    m_networkClient.rejectSeatTransfer(m_currentSeatId, userId, requestedRevision);
    emit commandExecuted(QStringLiteral("rejectSeatTransfer"), args);
}

void SimulationController::leaveOnlineRoom() {
    if (!isNetworked() || m_leaveRoomPending) return;
    m_leaveRoomPending = true;
    emit leaveRoomPendingChanged();
    m_networkClient.leaveRoom();
}

void SimulationController::setSeatReady(bool ready) {
    if (!isNetworked() || m_isObserver || m_currentSeatId.isEmpty()) return;
    m_networkClient.sendSeatReady(ready);
}

void SimulationController::updateOnlineRoomConfig(const QVariantMap& config) {
    if (!isNetworked() || !isRoomAdmin() || m_currentRoomId.isEmpty()
        || m_matchPhase != QLatin1String("preparing")) {
        emit errorForward(QStringLiteral("只有已进入准备阶段房间的房间管理员可以保存配置"));
        return;
    }
    QVariantMap args = config;
    // Seat capacity is derived by the server from the complete GIS scenario.
    // Drop the legacy field so room metadata cannot become a second source.
    args.remove(QStringLiteral("seatLimits"));
    if (!args.contains(QStringLiteral("expectedConfigVersion"))) {
        args.insert(QStringLiteral("expectedConfigVersion"), m_configVersion);
    }
    m_networkClient.sendCommand(QStringLiteral("updateRoomConfig"), args);
    emit commandExecuted(QStringLiteral("updateRoomConfig"), args);
}

void SimulationController::deployOnlineUnit(const QString& unitId, const QVariantMap& position) {
    if (isNetworked() && !m_isObserver) m_networkClient.sendDeployment(unitId, position);
}

void SimulationController::requestOnlineRedeploy() {
    if (isNetworked() && !m_isObserver) m_networkClient.requestRedeploy();
}

void SimulationController::redeployOnlineUnit(const QString& seatId) {
    const QString targetSeatId = seatId.trimmed();
    if (!isNetworked() || m_isObserver || targetSeatId.isEmpty()) return;
    m_networkClient.redeploy(targetSeatId);
    emit commandExecuted(QStringLiteral("redeploy"),
                         QVariantMap{{QStringLiteral("seatId"), targetSeatId}});
}

void SimulationController::setOnlineUnitName(const QString& unitName) {
    if (isNetworked() && !m_isObserver && !unitName.trimmed().isEmpty()) {
        m_networkClient.sendUnitName(unitName.trimmed());
    }
}

QString SimulationController::shareOnlineIntel(const QString& intelId,
                                               const QStringList& recipientSeatIds,
                                               const QString& note) {
    if (isNetworked() && !m_isObserver) {
        return m_networkClient.shareIntel(intelId, recipientSeatIds, note);
    }
    return {};
}

QString SimulationController::createOnlineIntelReport(const QVariantMap& position,
                                                      const QString& type,
                                                      const QString& title,
                                                      const QString& note) {
    if (isNetworked() && !m_isObserver) {
        return m_networkClient.createIntelReport(position, type.trimmed(), title, note);
    }
    return {};
}

QString SimulationController::requestOnlineIntelHistory(const QVariantMap& query) {
    if (isNetworked() && !m_isObserver && m_onlineIntelHistoryRequestId.isEmpty()) {
        m_onlineIntelHistoryAppendPending = !query.value(QStringLiteral("cursor"))
                                                 .toString().trimmed().isEmpty();
        const QString requestId = m_networkClient.requestIntelHistory(query);
        if (!requestId.isEmpty()) {
            m_onlineIntelHistoryRequestId = requestId;
            emit onlineIntelHistoryChanged();
        } else {
            m_onlineIntelHistoryAppendPending = false;
        }
        return requestId;
    }
    return {};
}

void SimulationController::resetOnlineIntelHistory() {
    m_networkClient.cancelIntelHistoryRequests();
    m_onlineIntelHistoryRequestId.clear();
    m_onlineIntelHistoryAppendPending = false;
    m_onlineIntelHistory.clear();
    m_onlineIntelHistoryHasMore = false;
    m_onlineIntelHistoryCursor.clear();
    emit onlineIntelHistoryChanged();
    emit onlineIntelHistoryReset();
}

void SimulationController::markOnlineMap(const QVariantMap& position, const QString& label,
                                         const QStringList& recipientSeatIds) {
    if (isNetworked() && !m_isObserver) m_networkClient.sendMapMark(position, label, recipientSeatIds);
}

void SimulationController::setObserverTrajectories(const QStringList& unitIds) {
    if (!isNetworked() || !m_isObserver) return;
    QSet<QString> unique;
    QStringList normalized;
    for (const QString& value : unitIds) {
        const QString unitId = value.trimmed();
        if (unitId.isEmpty() || unique.contains(unitId)) continue;
        unique.insert(unitId);
        normalized.append(unitId);
    }
    if (normalized.size() > Protocol::MaxObserverTrajectoryUnits) {
        emit errorForward(QStringLiteral("最多选择 8 个单位轨迹"));
        return;
    }
    m_networkClient.setObserverTrajectories(normalized);
}

QString SimulationController::connectToPeer(const QString& host, int port) {
    const QString message = QStringLiteral("请通过联网登录界面连接账号服务器: http://%1:%2")
                                .arg(host).arg(port);
    m_networkStatus = message;
    emit networkStatusChanged();
    return message;
}

void SimulationController::disconnectFromPeer() {
    useLocalMode();
}

void SimulationController::applyRoleView() {
    if (!isNetworked()) {
        ensureFocusedConsistent();
        return;
    }
    QString target;
    if (isRoomAdmin()) target = QStringLiteral("editor");
    else if (m_currentSeatSide == QLatin1String("red")
             || m_currentSeatSide == QLatin1String("blue")) {
        target = QStringLiteral("commandpost-%1").arg(m_currentSeatSide);
    }
    if (!target.isEmpty()) setViewMode(target);
    ensureFocusedConsistent();
}

void SimulationController::clearOnlineRoomDerivedState(bool preserveRoomId) {
    const bool wasObserver = m_isObserver;
    if (!preserveRoomId) m_currentRoomId.clear();
    m_currentSeatId.clear();
    m_currentSeatType.clear();
    m_currentSeatSide.clear();
    m_seatReady = false;
    m_matchPhase = QStringLiteral("preparing");
    m_redReady = false;
    m_blueReady = false;
    m_roomMode = QStringLiteral("pvp");
    m_aiDifficulty = QStringLiteral("normal");
    m_aiEffectiveEngine = QStringLiteral("rules");
    m_configVersion = 1;
    m_roomName.clear();
    m_roomDescription.clear();
    m_scenarioId = QStringLiteral("default");
    m_protocolProfile = QStringLiteral("native");
    m_operationMode = QStringLiteral("standard");
    m_participantSide.clear();
    m_fixedTargetSide.clear();
    m_serverScenarioEditable = false;
    m_vmfAutomation = {};
    m_remoteVmfTasks = {};
    m_remoteVmfTrace = {};
    m_remoteDemoState = {};
    m_onlineSeatLimits = {};
    m_onlineSeatParameters = {};
    m_remoteReadyForSim = false;
    m_remoteCpIssues.clear();
    m_communicationState = QStringLiteral("disconnected");
    m_onlineSeats.clear();
    m_onlineMapMarks.clear();
    m_onlineIntelRecords.clear();
    m_onlineIntelRevision = 0;
    m_onlineIntelShareTargets.clear();
    resetOnlineIntelHistory();
    const bool hadObserverTrajectories = !m_observerTrajectories.isEmpty();
    m_observerTrajectories = {};
    m_pendingSeatTransfers.clear();
    m_remoteMessages.clear();
    const bool hadProjectiles = !m_remoteProjectiles.isEmpty();
    m_remoteProjectiles.clear();
    m_chatMessages.clear();
    m_remoteScenarioRevision = -1;
    m_lastCommandId.clear();
    m_lastCommandCode.clear();
    m_lastCommandAction.clear();
    m_lastCommandStatus.clear();
    m_lastCommandMessage.clear();
    m_isObserver = preserveRoomId && wasObserver;
    m_observerJoinPending = false;
    m_onlineStage = m_isObserver ? QStringLiteral("observer")
        : preserveRoomId && !m_currentRoomId.isEmpty()
            ? (isRoomAdmin() ? QStringLiteral("roomAdmin")
                             : QStringLiteral("seatSelect"))
            : QStringLiteral("roomSelect");

    Scenario emptyProjection = m_engine.scenario();
    emptyProjection.units.clear();
    m_engine.setRemoteScenario(emptyProjection);
    invalidateCaches();
    ensureFocusedConsistent();
    emit onlineSeatsChanged();
    emit onlineMapMarksChanged();
    if (hadObserverTrajectories) emit observerTrajectoriesChanged();
    emit pendingSeatTransfersChanged();
    emit messagesForward();
    emit vmfTasksChanged();
    emit vmfTraceChanged();
    if (hadProjectiles) emit projectilesForward();
    emit chatMessagesChanged();
    emit commandStatusChanged();
    emit readyForSimForward();
    emit roomStateChanged();
    emit onlineStateChanged();
    emit sessionChanged();
    emit mapInfoForward();
    emit onlineIntelChanged();
}

QJsonObject SimulationController::scenarioUnitJson(const ScenarioUnit& unit) const {
    Scenario wrapper;
    wrapper.units.push_back(unit);
    const QJsonArray units = ScenarioIo::toJson(wrapper).value(QStringLiteral("units")).toArray();
    return units.isEmpty() ? QJsonObject{} : units.at(0).toObject();
}

void SimulationController::applyRemoteSnapshot(const QJsonObject& payload) {
    applyRemoteState(payload, {}, false);
}

void SimulationController::applyRemoteState(const QJsonObject& payload,
                                            const QStringList& changedUnitIds,
                                            bool partialRuntime) {
    if (!isNetworked()) return;
    Protocol::SnapshotProjection projection;
    const Protocol::ValidationResult validation = Protocol::projectSnapshot(payload, &projection);
    if (!validation.valid) {
        m_remoteLastError = validation.message;
        emit errorForward(validation.message);
        return;
    }
    const bool previousObserver = m_isObserver;
    const QString previousRoomId = m_currentRoomId;
    const QString previousSeatId = m_currentSeatId;
    const QString previousSeatType = m_currentSeatType;
    const QString previousSeatSide = m_currentSeatSide;
    const QString previousOnlineStage = m_onlineStage;
    const bool previousSeatReady = m_seatReady;
    const bool previousReadyForSim = m_remoteReadyForSim;
    const QString previousCpIssues = m_remoteCpIssues;
    const QString previousMatchPhase = m_matchPhase;
    const bool previousRedReady = m_redReady;
    const bool previousBlueReady = m_blueReady;
    const QString previousRoomMode = m_roomMode;
    const QString previousAiDifficulty = m_aiDifficulty;
    const QString previousAiEffectiveEngine = m_aiEffectiveEngine;
    const qint64 previousConfigVersion = m_configVersion;
    const QString previousRoomName = m_roomName;
    const QString previousRoomDescription = m_roomDescription;
    const QString previousScenarioId = m_scenarioId;
    const QJsonObject previousSeatLimits = m_onlineSeatLimits;
    const QJsonObject previousSeatParameters = m_onlineSeatParameters;
    const QString previousCommunicationState = m_communicationState;
    const QVariantList previousMessages = m_remoteMessages;
    const QJsonObject previousVmfWorkflow = m_remoteVmfWorkflow;
    const QJsonObject previousVmfTasks = m_remoteVmfTasks;
    const QJsonObject previousDemoState = m_remoteDemoState;
    const QString previousProtocolProfile = m_protocolProfile;
    const QString previousOperationMode = m_operationMode;
    const QString previousParticipantSide = m_participantSide;
    const QString previousFixedTargetSide = m_fixedTargetSide;
    const bool previousScenarioEditable = m_serverScenarioEditable;
    const QJsonObject previousVmfAutomation = m_vmfAutomation;
    const QVariantList previousMapMarks = m_onlineMapMarks;
    const QVariantList previousIntelRecords = m_onlineIntelRecords;
    const qint64 previousIntelRevision = m_onlineIntelRevision;
    const QStringList previousIntelShareTargets = m_onlineIntelShareTargets;
    const QVariantList previousSeats = m_onlineSeats;
    const Protocol::RoomLifecycleProjection& room = projection.lifecycle;
    m_isObserver = room.observer;
    if (room.observer) m_observerJoinPending = false;
    // roomState is shared by all clients and therefore contains the server's
    // room id even for a client that has just left. Do not resurrect a room
    // selection while the controller is already showing the room directory.
    if (!room.roomId.isEmpty()
        && (m_onlineStage != QLatin1String("roomSelect") || room.observer)) {
        m_currentRoomId = room.roomId;
    }
    const bool roomSelectionConfirmed = !m_currentRoomId.isEmpty()
        && m_currentRoomId == room.roomId && m_onlineStage != QLatin1String("roomSelect");
    if (room.observer) {
        m_onlineSeats.clear();
        m_currentSeatId.clear();
        m_currentSeatType.clear();
        m_currentSeatSide.clear();
        m_seatReady = false;
        m_onlineStage = QStringLiteral("observer");
    } else if (roomSelectionConfirmed
        && payload.value(QStringLiteral("roomState")).toObject().contains(QStringLiteral("seats"))) {
        m_onlineSeats = Protocol::seatVariants(room.seats);
    } else if (!roomSelectionConfirmed) {
        m_onlineSeats.clear();
    }
    m_matchPhase = room.phase;
    m_redReady = room.redReady;
    m_blueReady = room.blueReady;
    m_roomMode = room.roomMode;
    m_aiDifficulty = room.aiDifficulty;
    m_aiEffectiveEngine = room.aiEngine;
    m_configVersion = room.configVersion;
    m_roomName = room.roomName;
    m_roomDescription = room.roomDescription;
    m_scenarioId = room.scenarioId;
    m_protocolProfile = room.protocolProfile;
    m_operationMode = room.operationMode;
    m_participantSide = room.participantSide;
    m_fixedTargetSide = room.fixedTargetSide;
    m_serverScenarioEditable = room.scenarioEditable;
    m_vmfAutomation = room.vmfAutomation;
    m_remoteVmfTasks = room.vmfTasks;
    m_remoteDemoState = room.demoState;
    m_onlineSeatLimits = room.seatLimits;
    m_onlineSeatParameters = room.seatParameters;
    const QString projectedCommunication = payload.value(QStringLiteral("roomState"))
                                               .toObject()
                                               .value(QStringLiteral("communicationState"))
                                               .toString();
    if (projectedCommunication == QLatin1String("twoWay")) {
        m_communicationState = QStringLiteral("bilateral");
    } else if (projectedCommunication == QLatin1String("bilateral")
               || projectedCommunication == QLatin1String("receiveOnly")
               || projectedCommunication == QLatin1String("disconnected")) {
        m_communicationState = projectedCommunication;
    } else {
        m_communicationState = QStringLiteral("disconnected");
    }
    const QJsonObject incomingRoomState = payload.value(QStringLiteral("roomState")).toObject();
    QJsonObject incomingVmfWorkflow = incomingRoomState.value(QStringLiteral("vmfWorkflow")).toObject();
    if (incomingVmfWorkflow.isEmpty()) {
        const QJsonObject workflows = incomingRoomState.value(QStringLiteral("vmfWorkflows")).toObject();
        incomingVmfWorkflow = workflows.value(
            m_focusedSide == QLatin1String("blue") ? QStringLiteral("blue")
                                                    : QStringLiteral("red")).toObject();
    }
    m_remoteVmfWorkflow = incomingVmfWorkflow;
    const QVariantList roomSeats = Protocol::seatVariants(room.seats);
    const bool roomAdminSession = !room.observer && isRoomAdmin() && roomSelectionConfirmed;
    bool currentSeatPresent = room.observer;
    if (!roomAdminSession && roomSelectionConfirmed && !m_currentSeatId.isEmpty()) {
        for (const QVariant& value : roomSeats) {
            const QVariantMap seat = value.toMap();
            if (seat.value(QStringLiteral("seatId")).toString() == m_currentSeatId
                && seat.value(QStringLiteral("occupied")).toBool()) {
                currentSeatPresent = true;
                m_seatReady = seat.value(QStringLiteral("ready")).toBool();
                break;
            }
        }
    }
    if (!roomAdminSession && !currentSeatPresent) {
        m_currentSeatId.clear();
        m_currentSeatType.clear();
        m_currentSeatSide.clear();
        m_seatReady = false;
        m_onlineStage = m_currentRoomId.isEmpty()
            ? QStringLiteral("roomSelect") : QStringLiteral("seatSelect");
    }
    const bool activePhase = m_matchPhase == QLatin1String("running")
        || m_matchPhase == QLatin1String("paused")
        || m_matchPhase == QLatin1String("finished");
    if (room.observer) {
        m_onlineStage = QStringLiteral("observer");
    } else if (roomAdminSession) {
        m_onlineStage = QStringLiteral("roomAdmin");
    } else if (roomSelectionConfirmed && currentSeatPresent && activePhase) {
        m_onlineStage = QStringLiteral("battle");
    } else if (roomSelectionConfirmed && currentSeatPresent) {
        m_onlineStage = QStringLiteral("deployment");
    } else if (roomSelectionConfirmed) {
        m_onlineStage = QStringLiteral("seatSelect");
    } else {
        m_onlineStage = QStringLiteral("roomSelect");
    }
    m_remoteReadyForSim = room.readyForSim;
    m_remoteCpIssues = room.cpIssues;
    const qint64 revision = room.scenarioRevision;

    const QJsonObject scenarioObject = payload.value(QStringLiteral("scenario")).toObject();
    QStringList incomingIds;
    if (roomSelectionConfirmed) {
        const QJsonArray scenarioUnits = scenarioObject.value(QStringLiteral("units")).toArray();
        incomingIds.reserve(scenarioUnits.size());
        for (const QJsonValue& value : scenarioUnits) {
            const QString id = value.toObject().value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) incomingIds.append(id);
        }
    }
    QStringList currentIds = m_engine.unitIds();
    std::sort(incomingIds.begin(), incomingIds.end());
    std::sort(currentIds.begin(), currentIds.end());
    if (revision != m_remoteScenarioRevision || incomingIds != currentIds) {
        Scenario incomingScenario = ScenarioIo::fromJson(scenarioObject);
        if (!roomSelectionConfirmed) incomingScenario.units.clear();
        if (!m_engine.setRemoteScenario(incomingScenario)) {
            m_remoteLastError = m_engine.lastError();
            emit errorForward(m_remoteLastError);
            return;
        }
        m_remoteScenarioRevision = revision;
    }

    m_remoteMessages = roomSelectionConfirmed
        ? payload.value(QStringLiteral("messages")).toArray().toVariantList() : QVariantList{};
    m_onlineMapMarks = roomSelectionConfirmed
        ? payload.value(QStringLiteral("mapMarks")).toArray().toVariantList() : QVariantList{};
    const QJsonObject intelObject = (!room.observer && roomSelectionConfirmed)
        ? payload.value(QStringLiteral("intelState")).toObject() : QJsonObject{};
    Protocol::IntelState intelState;
    QVariantList nextIntelRecords = m_onlineIntelRecords;
    qint64 nextIntelRevision = m_onlineIntelRevision;
    QStringList nextIntelShareTargets = m_onlineIntelShareTargets;
    if (!intelObject.isEmpty() && Protocol::fromJson(intelObject, &intelState).valid) {
        nextIntelRecords.clear();
        for (const auto& record : intelState.records) {
            nextIntelRecords.append(record.toJson().toVariantMap());
        }
        nextIntelRevision = intelState.revision;
        nextIntelShareTargets = intelState.shareTargets;
    } else if (room.observer || !roomSelectionConfirmed) {
        nextIntelRecords.clear();
        nextIntelRevision = 0;
        nextIntelShareTargets.clear();
    }
    const bool intelChanged = nextIntelRecords != previousIntelRecords
        || nextIntelRevision != previousIntelRevision
        || nextIntelShareTargets != previousIntelShareTargets;
    if (intelChanged) {
        m_onlineIntelRecords = std::move(nextIntelRecords);
        m_onlineIntelRevision = nextIntelRevision;
        m_onlineIntelShareTargets = std::move(nextIntelShareTargets);
        emit onlineIntelChanged();
    }
    const QJsonObject incomingTrajectories = room.observer
        ? payload.value(QStringLiteral("observerTrajectories")).toObject()
        : QJsonObject{};
    if (incomingTrajectories != m_observerTrajectories) {
        m_observerTrajectories = incomingTrajectories;
        emit observerTrajectoriesChanged();
    }
    const QJsonArray allRuntimeUnits = roomSelectionConfirmed
        ? payload.value(QStringLiteral("units")).toArray() : QJsonArray{};
    QJsonArray runtimeUnits = allRuntimeUnits;
    if (partialRuntime) {
        const QSet<QString> changed(changedUnitIds.cbegin(), changedUnitIds.cend());
        runtimeUnits = {};
        for (const QJsonValue& value : allRuntimeUnits) {
            if (changed.contains(value.toObject().value(QStringLiteral("id")).toString())) {
                runtimeUnits.append(value);
            }
        }
    }
    const QVariantList incomingProjectiles = roomSelectionConfirmed
        ? payload.value(QStringLiteral("projectiles")).toArray().toVariantList()
        : QVariantList{};
    if (incomingProjectiles != m_remoteProjectiles) {
        m_remoteProjectiles = incomingProjectiles;
        emit projectilesForward();
    }
    m_engine.applyRemoteRuntimeState(runtimeUnits,
                                     room.simTime, room.running, room.speed,
                                     partialRuntime);
    invalidateCaches();
    applyRoleView();
    ensureFocusedConsistent();
    if (m_remoteMessages != previousMessages) emit messagesForward();
    if (m_remoteVmfWorkflow != previousVmfWorkflow) emit vmfWorkflowChanged();
    if (m_remoteVmfTasks != previousVmfTasks) emit vmfTasksChanged();
    if (m_remoteDemoState != previousDemoState) emit demoStateChanged();
    if (m_onlineMapMarks != previousMapMarks) emit onlineMapMarksChanged();
    if (m_onlineSeats != previousSeats) {
        emit onlineSeatsChanged();
        // canEditScenario is also a server-confirmed permission gate: a room
        // admin may edit the initial scene only while no seat is occupied.
        emit roomStateChanged();
    }
    if (m_remoteReadyForSim != previousReadyForSim
        || m_remoteCpIssues != previousCpIssues) {
        emit readyForSimForward();
    }
    if (m_matchPhase != previousMatchPhase || m_redReady != previousRedReady
        || m_blueReady != previousBlueReady || m_roomMode != previousRoomMode
        || m_aiDifficulty != previousAiDifficulty
        || m_aiEffectiveEngine != previousAiEffectiveEngine
        || m_configVersion != previousConfigVersion
        || m_roomName != previousRoomName
        || m_roomDescription != previousRoomDescription
        || m_scenarioId != previousScenarioId
        || m_onlineSeatLimits != previousSeatLimits
        || m_onlineSeatParameters != previousSeatParameters
        || m_communicationState != previousCommunicationState
        || m_protocolProfile != previousProtocolProfile
        || m_operationMode != previousOperationMode
        || m_participantSide != previousParticipantSide
        || m_fixedTargetSide != previousFixedTargetSide
        || m_serverScenarioEditable != previousScenarioEditable
        || m_vmfAutomation != previousVmfAutomation) {
        emit roomStateChanged();
    }
    if (m_isObserver != previousObserver || m_currentRoomId != previousRoomId
        || m_currentSeatId != previousSeatId || m_currentSeatType != previousSeatType
        || m_currentSeatSide != previousSeatSide
        || m_onlineStage != previousOnlineStage || m_seatReady != previousSeatReady) {
        emit onlineStateChanged();
    }
}

QJsonObject SimulationController::allSettings() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QString path = dir + "/settings.json";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

} // namespace gbr
