#include "SimulationRoom.h"

#include "PersistenceStore.h"
#include "ServerConfig.h"
#include "VisibleStateProjector.h"
#include "core/Scenario.h"
#include "core/SnapshotCodec.h"
#include "core/UnitBase.h"

#include <QJsonDocument>
#include <QDateTime>
#include <QSet>
#include <cmath>
#include <vector>

namespace gbr {

namespace {

constexpr qsizetype kMaxRoomUnits = 500;

bool isDomainAction(const QString& action) {
    static const QSet<QString> actions{
        QString::fromLatin1(CommandAction::AssignTarget),
        QString::fromLatin1(CommandAction::SetFlightPlan),
        QString::fromLatin1(CommandAction::EngageTarget),
        QString::fromLatin1(CommandAction::MoveTo),
        QString::fromLatin1(CommandAction::Withdraw),
        QString::fromLatin1(CommandAction::SetSpeed),
        QString::fromLatin1(CommandAction::Pursue),
        QString::fromLatin1(CommandAction::GuideAttack),
        QString::fromLatin1(CommandAction::SetSchedule),
        QString::fromLatin1(CommandAction::Halt),
    };
    return actions.contains(action);
}

} // namespace

SimulationRoom::SimulationRoom(const ServerConfig& config, PersistenceStore* persistence,
                               QObject* parent)
    : QObject(parent), m_config(config), m_persistence(persistence) {
    m_checkpointTimer.setInterval(config.checkpointIntervalMs);
    connect(&m_checkpointTimer, &QTimer::timeout, this, [this] {
        QString error;
        if (!checkpointNow(&error)) emit checkpointFailed(error);
    });
}

QString SimulationRoom::roomId() const {
    return m_config.roomId;
}

qint64 SimulationRoom::serverTick() const {
    return static_cast<qint64>(std::llround(m_engine.simTime() / 0.05));
}

bool SimulationRoom::isPreparationPhase() const {
    return !m_engine.running() && serverTick() == 0;
}

void SimulationRoom::resetReadiness() {
    m_redReady = false;
    m_blueReady = false;
}

bool SimulationRoom::initialize(QString* error) {
    if (error) error->clear();
    qint64 restoredRevision = 1;
    qint64 restoredTick = 0;
    QString checkpointError;
    const QJsonObject checkpoint = m_persistence
        ? m_persistence->loadLatestCheckpoint(roomId(), &restoredRevision, &restoredTick,
                                              &checkpointError)
        : QJsonObject{};
    if (!checkpointError.isEmpty()) {
        if (error) *error = checkpointError;
        return false;
    }
    if (!checkpoint.isEmpty()) {
        const QJsonObject runtime = checkpoint.value(QStringLiteral("runtime")).toObject();
        if (checkpoint.value(QStringLiteral("checkpointVersion")).toInt(-1) != 1
            || !checkpoint.value(QStringLiteral("scenario")).isObject()
            || !checkpoint.value(QStringLiteral("runtime")).isObject()
            || runtime.value(QStringLiteral("scenarioRevision")).toInteger(-1)
                != restoredRevision
            || runtime.value(QStringLiteral("serverTick")).toInteger(-1) != restoredTick) {
            if (error) *error = QStringLiteral("最新检查点结构无效");
            return false;
        }
        const Scenario scenario = ScenarioIo::fromJson(checkpoint.value(QStringLiteral("scenario")).toObject());
        if (static_cast<qsizetype>(scenario.units.size()) > kMaxRoomUnits) {
            if (error) *error = QStringLiteral("检查点场景超过每房间 500 个单元限制");
            return false;
        }
        if (!m_engine.setScenario(scenario)
            || !m_engine.restoreRuntimeState(runtime)) {
            if (error) *error = m_engine.lastError();
            return false;
        }
        m_scenarioRevision = std::max<qint64>(1, restoredRevision);
    } else if (!m_config.scenarioPath.isEmpty()) {
        QString scenarioError;
        const Scenario scenario = ScenarioIo::loadFromFile(m_config.scenarioPath, &scenarioError);
        if (static_cast<qsizetype>(scenario.units.size()) > kMaxRoomUnits) {
            if (error) *error = QStringLiteral("初始场景超过每房间 500 个单元限制");
            return false;
        }
        if (!scenarioError.isEmpty() || !m_engine.setScenario(scenario)) {
            if (error) *error = scenarioError.isEmpty() ? m_engine.lastError() : scenarioError;
            return false;
        }
    } else {
        m_engine.loadDefaultScenario();
    }
    m_checkpointTimer.start();
    return true;
}

CommandResult SimulationRoom::rejected(const char* code, const QString& message) const {
    return CommandResult::rejected(code, message, serverTick());
}

CommandResult SimulationRoom::accepted(const QString& message) const {
    return CommandResult::success(serverTick(), message);
}

bool SimulationRoom::canControlUnit(const SessionIdentity& identity, const QString& unitId) const {
    const UnitBase* unit = m_engine.unit(unitId);
    if (!unit) return false;
    if (identity.role == QLatin1String(SessionRole::Director)) return true;
    return !identity.side.isEmpty() && unit->sideStr() == identity.side;
}

CommandResult SimulationRoom::authorizeDomainCommand(const SessionIdentity& identity,
                                                     const QString& action,
                                                     const QVariantMap& args) const {
    if (identity.role == QLatin1String(SessionRole::Observer)
        || identity.role == QLatin1String(SessionRole::Editor)) {
        return rejected(CommandCode::PermissionDenied, QStringLiteral("当前角色不能下达战术命令"));
    }
    const QString unitId = args.value(QStringLiteral("unitId")).toString();
    const QString attackerId = args.value(QStringLiteral("attackerId")).toString();
    const QString guideId = args.value(QStringLiteral("guideId")).toString();
    if (!unitId.isEmpty() && !canControlUnit(identity, unitId)) {
        return rejected(CommandCode::PermissionDenied, QStringLiteral("不能控制其他阵营单元"));
    }
    if (!attackerId.isEmpty() && !canControlUnit(identity, attackerId)) {
        return rejected(CommandCode::PermissionDenied, QStringLiteral("不能控制其他阵营攻击单元"));
    }
    if (!guideId.isEmpty() && !canControlUnit(identity, guideId)) {
        return rejected(CommandCode::PermissionDenied, QStringLiteral("不能控制其他阵营引导单元"));
    }
    if (identity.role == QLatin1String(SessionRole::Red)
        || identity.role == QLatin1String(SessionRole::Blue)) {
        const QString targetId = args.value(QStringLiteral("targetId")).toString();
        if (!targetId.isEmpty()
            && !VisibleStateProjector::visibleEnemyIds(m_engine, identity.side).contains(targetId)) {
            return rejected(CommandCode::PermissionDenied, QStringLiteral("目标不在当前合法视野中"));
        }
    }
    Q_UNUSED(action);
    return accepted(QStringLiteral("权限检查通过"));
}

CommandResult SimulationRoom::specialCommand(const SessionIdentity& identity,
                                             const QString& action,
                                             const QVariantMap& args) {
    if (action == QLatin1String("setRunning")) {
        if (identity.role != QLatin1String(SessionRole::Director)) {
            return rejected(CommandCode::PermissionDenied, QStringLiteral("只有导演可以开始或暂停推演"));
        }
        if (!args.contains(QStringLiteral("running"))) {
            return rejected(CommandCode::InvalidArgument, QStringLiteral("缺少 running 参数"));
        }
        const bool running = args.value(QStringLiteral("running")).toBool();
        if (running) {
            if (!m_engine.readyForSim()) {
                return rejected(CommandCode::SimulationStateInvalid, m_engine.cpIssues());
            }
            if (isPreparationPhase() && !bothSidesReady()) {
                return rejected(CommandCode::SimulationStateInvalid,
                                QStringLiteral("红蓝双方均点击就绪后才能开始推演"));
            }
        }
        m_engine.setRunning(running);
        return accepted(running ? QStringLiteral("推演已开始") : QStringLiteral("推演已暂停"));
    }
    if (action == QLatin1String("setSimulationSpeed")) {
        if (identity.role != QLatin1String(SessionRole::Director)) {
            return rejected(CommandCode::PermissionDenied, QStringLiteral("只有导演可以修改推演速度"));
        }
        bool speedOk = false;
        const double speed = args.value(QStringLiteral("speed")).toDouble(&speedOk);
        if (!speedOk || !std::isfinite(speed) || speed < 0.0 || speed > 8.0) {
            return rejected(CommandCode::InvalidArgument, QStringLiteral("推演速度必须在 0 至 8 之间"));
        }
        m_engine.setSpeedMul(speed);
        return accepted(QStringLiteral("推演速度已更新"));
    }
    if (action == QLatin1String("stepOnce")) {
        if (identity.role != QLatin1String(SessionRole::Director)) {
            return rejected(CommandCode::PermissionDenied, QStringLiteral("只有导演可以单步推演"));
        }
        if (m_engine.running()) return rejected(CommandCode::SimulationStateInvalid, QStringLiteral("运行中不能单步"));
        m_engine.stepOnce(0.05);
        return accepted(QStringLiteral("已推进一个 tick"));
    }
    if (action == QLatin1String("resetSimulation")) {
        if (identity.role != QLatin1String(SessionRole::Director)) {
            return rejected(CommandCode::PermissionDenied, QStringLiteral("只有导演可以结束并复位推演"));
        }
        const Scenario initial = m_engine.scenario();
        if (!m_engine.setScenario(initial)) {
            return rejected(CommandCode::SimulationStateInvalid, m_engine.lastError());
        }
        ++m_scenarioRevision;
        resetReadiness();
        return accepted(QStringLiteral("推演已结束，所有单位已恢复初始状态"));
    }
    if (action == QLatin1String("replaceScenario")) {
        const QVariant scenarioValue = args.value(QStringLiteral("scenario"));
        if (!scenarioValue.isValid()) return rejected(CommandCode::InvalidArgument, QStringLiteral("缺少 scenario 参数"));
        const QJsonObject scenarioObject = QJsonObject::fromVariantMap(scenarioValue.toMap());
        if (!scenarioObject.value(QStringLiteral("units")).isArray()
            || scenarioObject.value(QStringLiteral("units")).toArray().size() > kMaxRoomUnits) {
            return rejected(CommandCode::InvalidArgument,
                            QStringLiteral("场景单元必须是数组且不能超过 500 个"));
        }
        if (identity.role == QLatin1String(SessionRole::Red)
            || identity.role == QLatin1String(SessionRole::Blue)) {
            if (!isPreparationPhase()) {
                return rejected(CommandCode::SimulationStateInvalid,
                                QStringLiteral("只能在推演开始前编辑己方阵容"));
            }
            const Scenario submitted = ScenarioIo::fromJson(scenarioObject);
            for (const auto& unit : submitted.units) {
                if (unit.side != identity.side) {
                    return rejected(CommandCode::PermissionDenied,
                                    QStringLiteral("只能编辑己方单元"));
                }
            }
            Scenario merged = m_engine.scenario();
            std::vector<ScenarioUnit> units;
            units.reserve(merged.units.size() + submitted.units.size());
            for (const auto& unit : merged.units) {
                if (unit.side != identity.side) units.push_back(unit);
            }
            for (const auto& unit : submitted.units) units.push_back(unit);
            merged.units = std::move(units);
            if (!m_engine.setScenario(merged)) {
                return rejected(CommandCode::InvalidArgument, m_engine.lastError());
            }
            ++m_scenarioRevision;
            resetReadiness();
            return accepted(QStringLiteral("己方阵容已更新，双方需要重新就绪"));
        }
        if (identity.role != QLatin1String(SessionRole::Editor)
            && identity.role != QLatin1String(SessionRole::Director)) {
            return rejected(CommandCode::PermissionDenied, QStringLiteral("当前角色不能编辑场景"));
        }
        if (m_engine.running()) return rejected(CommandCode::SimulationStateInvalid, QStringLiteral("必须暂停推演后才能编辑场景"));
        const Scenario scenario = ScenarioIo::fromJson(scenarioObject);
        if (!m_engine.setScenario(scenario)) return rejected(CommandCode::InvalidArgument, m_engine.lastError());
        ++m_scenarioRevision;
        resetReadiness();
        return accepted(QStringLiteral("场景已替换"));
    }
    if (action == QLatin1String("setSideReady")) {
        if (identity.role != QLatin1String(SessionRole::Red)
            && identity.role != QLatin1String(SessionRole::Blue)) {
            return rejected(CommandCode::PermissionDenied, QStringLiteral("只有红蓝双方可以设置就绪状态"));
        }
        if (!isPreparationPhase()) {
            return rejected(CommandCode::SimulationStateInvalid,
                            QStringLiteral("推演开始后不能修改就绪状态"));
        }
        if (!args.value(QStringLiteral("ready")).isValid()) {
            return rejected(CommandCode::InvalidArgument, QStringLiteral("缺少 ready 参数"));
        }
        const bool ready = args.value(QStringLiteral("ready")).toBool();
        if (ready && !m_engine.readyForSim()) {
            return rejected(CommandCode::SimulationStateInvalid, m_engine.cpIssues());
        }
        if (identity.side == QLatin1String("red")) m_redReady = ready;
        else m_blueReady = ready;
        return accepted(ready ? QStringLiteral("已就绪") : QStringLiteral("已取消就绪"));
    }
    if (action == QLatin1String("sendChat")) {
        if (identity.role != QLatin1String(SessionRole::Red)
            && identity.role != QLatin1String(SessionRole::Blue)
            && identity.role != QLatin1String(SessionRole::Director)) {
            return rejected(CommandCode::PermissionDenied, QStringLiteral("当前角色不能发送聊天消息"));
        }
        const QString text = args.value(QStringLiteral("text")).toString().trimmed();
        if (text.isEmpty() || text.size() > 500) {
            return rejected(CommandCode::InvalidArgument, QStringLiteral("聊天内容必须为 1 至 500 个字符"));
        }
        m_chatMessages.append(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("chat-%1").arg(m_nextChatMessageId++)},
            {QStringLiteral("role"), identity.role},
            {QStringLiteral("side"), identity.side},
            {QStringLiteral("userId"), identity.userId},
            {QStringLiteral("text"), text},
            {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        });
        while (m_chatMessages.size() > 100) m_chatMessages.removeAt(0);
        return accepted(QStringLiteral("聊天消息已发送"));
    }
    return rejected(CommandCode::UnknownAction, QStringLiteral("未知命令: %1").arg(action));
}

CommandResult SimulationRoom::execute(const SessionIdentity& identity, const QString& action,
                                      const QVariantMap& args, qint64 clientRevision) {
    if (!identity.isValid() || identity.roomId != roomId()) {
        return rejected(CommandCode::NotAuthenticated, QStringLiteral("会话身份无效或房间不匹配"));
    }
    if (clientRevision != m_scenarioRevision) {
        return rejected(CommandCode::RevisionMismatch,
                        QStringLiteral("场景版本不一致，需要重新同步"));
    }
    if (!isDomainAction(action)) return specialCommand(identity, action, args);
    const CommandResult authorization = authorizeDomainCommand(identity, action, args);
    if (!authorization.accepted) return authorization;
    return m_engine.command(action, args);
}

QJsonObject SimulationRoom::projectedState(const SessionIdentity& identity,
                                           qint64 lastSequence) const {
    QJsonObject state = VisibleStateProjector::project(m_engine, identity, m_scenarioRevision,
                                                        serverTick(), lastSequence);
    state.insert(QStringLiteral("lobby"), QJsonObject{
        {QStringLiteral("redReady"), m_redReady},
        {QStringLiteral("blueReady"), m_blueReady},
        {QStringLiteral("bothReady"), bothSidesReady()},
        {QStringLiteral("preparation"), isPreparationPhase()},
    });
    if (identity.role == QLatin1String(SessionRole::Red)
        || identity.role == QLatin1String(SessionRole::Blue)) {
        QJsonObject ownScenario = ScenarioIo::toJson(m_engine.scenario());
        QJsonArray ownUnits;
        for (const auto& value : ownScenario.value(QStringLiteral("units")).toArray()) {
            if (value.toObject().value(QStringLiteral("side")).toString() == identity.side) {
                ownUnits.append(value);
            }
        }
        ownScenario.insert(QStringLiteral("units"), ownUnits);
        state.insert(QStringLiteral("scenario"), ownScenario);
    }
    if (identity.role == QLatin1String(SessionRole::Red)
        || identity.role == QLatin1String(SessionRole::Blue)
        || identity.role == QLatin1String(SessionRole::Director)) {
        state.insert(QStringLiteral("chatMessages"), m_chatMessages);
    } else {
        state.insert(QStringLiteral("chatMessages"), QJsonArray{});
    }
    return state;
}

bool SimulationRoom::checkpointNow(QString* error) {
    if (!m_persistence || !m_persistence->isOpen()) {
        if (error) *error = QStringLiteral("持久化存储未打开");
        return false;
    }
    const QJsonObject checkpoint{
        {QStringLiteral("checkpointVersion"), 1},
        {QStringLiteral("scenario"), ScenarioIo::toJson(m_engine.scenario())},
        {QStringLiteral("runtime"), SnapshotCodec::encodeRuntime(m_engine, serverTick(), m_scenarioRevision)},
    };
    return m_persistence->saveCheckpoint(roomId(), m_scenarioRevision,
                                         serverTick(), checkpoint, error);
}

} // namespace gbr
