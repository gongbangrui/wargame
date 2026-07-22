#include "RoomPersistence.h"

#include "protocol/Protocol.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <utility>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace gbr {

namespace {

constexpr int kCheckpointSchemaVersion = 1;
constexpr qint64 kMaxEventLogBytes = 16 * 1024 * 1024;
constexpr qint64 kMaxCheckpointBytes = 64 * 1024 * 1024;

bool ensureParentDirectory(const QString& path, QString* error) {
    const QString directory = QFileInfo(path).absolutePath();
    if (QDir().mkpath(directory)) return true;
    if (error) *error = QStringLiteral("无法创建持久化目录: %1").arg(directory);
    return false;
}

QJsonObject checkpointToJson(const RoomCheckpoint& checkpoint) {
    return {{QStringLiteral("checkpointSchemaVersion"), kCheckpointSchemaVersion},
            {QStringLiteral("protocolVersion"), Protocol::Version},
            {QStringLiteral("savedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("scenario"), ScenarioIo::toJson(checkpoint.scenario)},
            {QStringLiteral("runInitialScenario"), ScenarioIo::toJson(checkpoint.runInitialScenario)},
            {QStringLiteral("runtimeUnits"), checkpoint.runtimeUnits},
            {QStringLiteral("commandHistory"), checkpoint.commandHistory},
            {QStringLiteral("roomState"),
             QJsonObject{{QStringLiteral("phase"), checkpoint.phase},
                         {QStringLiteral("redReady"), checkpoint.redReady},
                         {QStringLiteral("blueReady"), checkpoint.blueReady},
                         {QStringLiteral("running"), checkpoint.running},
                         {QStringLiteral("simTime"), checkpoint.simTime},
                         {QStringLiteral("speed"), checkpoint.speed},
                         {QStringLiteral("scenarioRevision"),
                          static_cast<qint64>(checkpoint.scenarioRevision)},
                         {QStringLiteral("stateRevision"),
                          static_cast<qint64>(checkpoint.stateRevision)},
                         {QStringLiteral("eventSequence"),
                          static_cast<qint64>(checkpoint.eventSequence)}}}};
}

bool checkpointFromJson(const QJsonObject& object, RoomCheckpoint* checkpoint,
                        QString* error) {
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (object.value(QStringLiteral("checkpointSchemaVersion")).toInt()
        != kCheckpointSchemaVersion) {
        return fail(QStringLiteral("检查点结构版本不兼容"));
    }
    if (object.value(QStringLiteral("protocolVersion")).toInt() != Protocol::Version) {
        return fail(QStringLiteral("检查点协议版本不兼容"));
    }
    if (!object.value(QStringLiteral("scenario")).isObject()
        || !object.value(QStringLiteral("runInitialScenario")).isObject()
        || !object.value(QStringLiteral("runtimeUnits")).isArray()
        || !object.value(QStringLiteral("commandHistory")).isArray()
        || !object.value(QStringLiteral("roomState")).isObject()) {
        return fail(QStringLiteral("检查点必需字段类型无效"));
    }
    const QJsonObject room = object.value(QStringLiteral("roomState")).toObject();
    const QString phase = room.value(QStringLiteral("phase")).toString();
    if (phase != QLatin1String("preparing") && phase != QLatin1String("running")
        && phase != QLatin1String("finished")) {
        return fail(QStringLiteral("检查点推演阶段无效"));
    }
    for (const QString& field : {QStringLiteral("redReady"), QStringLiteral("blueReady"),
                                 QStringLiteral("running")}) {
        if (!room.value(field).isBool()) {
            return fail(QStringLiteral("检查点布尔状态无效: %1").arg(field));
        }
    }
    const double simTime = room.value(QStringLiteral("simTime")).toDouble(-1.0);
    const double speed = room.value(QStringLiteral("speed")).toDouble(-1.0);
    const qint64 scenarioRevision = room.value(QStringLiteral("scenarioRevision")).toInteger();
    const qint64 stateRevision = room.value(QStringLiteral("stateRevision")).toInteger();
    const qint64 eventSequence = room.value(QStringLiteral("eventSequence")).toInteger();
    if (!std::isfinite(simTime) || simTime < 0.0 || !std::isfinite(speed)
        || speed < 0.0 || speed > 8.0 || scenarioRevision <= 0
        || stateRevision <= 0 || eventSequence < 0) {
        return fail(QStringLiteral("检查点房间状态无效"));
    }
    const Scenario scenario = ScenarioIo::fromJson(object.value(QStringLiteral("scenario")).toObject());
    if (scenario.units.empty()) return fail(QStringLiteral("检查点场景为空"));

    const Scenario runInitialScenario = ScenarioIo::fromJson(
        object.value(QStringLiteral("runInitialScenario")).toObject());
    if (phase != QLatin1String("preparing") && runInitialScenario.units.empty()) {
        return fail(QStringLiteral("检查点缺少开局场景"));
    }

    checkpoint->scenario = scenario;
    checkpoint->runInitialScenario = runInitialScenario;
    checkpoint->runtimeUnits = object.value(QStringLiteral("runtimeUnits")).toArray();
    checkpoint->commandHistory = object.value(QStringLiteral("commandHistory")).toArray();
    checkpoint->phase = phase;
    checkpoint->redReady = room.value(QStringLiteral("redReady")).toBool();
    checkpoint->blueReady = room.value(QStringLiteral("blueReady")).toBool();
    checkpoint->running = room.value(QStringLiteral("running")).toBool();
    checkpoint->simTime = simTime;
    checkpoint->speed = speed;
    checkpoint->scenarioRevision = static_cast<quint64>(scenarioRevision);
    checkpoint->stateRevision = static_cast<quint64>(stateRevision);
    checkpoint->eventSequence = static_cast<quint64>(eventSequence);
    return true;
}

} // namespace

RoomPersistence::RoomPersistence(QString checkpointPath, QString eventLogPath)
    : m_checkpointPath(std::move(checkpointPath)),
      m_eventLogPath(std::move(eventLogPath)) {}

bool RoomPersistence::saveCheckpoint(const RoomCheckpoint& checkpoint, QString* error) const {
    if (error) error->clear();
    if (!ensureParentDirectory(m_checkpointPath, error)) return false;
    const QByteArray data = QJsonDocument(checkpointToJson(checkpoint))
                                .toJson(QJsonDocument::Indented);
    if (data.size() > kMaxCheckpointBytes) {
        if (error) *error = QStringLiteral("检查点数据超过 64 MiB 限制");
        return false;
    }
    QSaveFile file(m_checkpointPath);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("无法写入检查点: %1").arg(m_checkpointPath);
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        if (error) *error = QStringLiteral("检查点原子写入失败: %1").arg(m_checkpointPath);
        return false;
    }
    return true;
}

bool RoomPersistence::loadCheckpointFile(const QString& path, RoomCheckpoint* checkpoint,
                                         QString* error) const {
    QFile file(path);
    if (file.exists() && file.size() > kMaxCheckpointBytes) {
        if (error) *error = QStringLiteral("检查点文件超过 64 MiB 限制");
        return false;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取检查点: %1").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        if (error) *error = QStringLiteral("检查点 JSON 无效（偏移 %1）: %2")
                                .arg(parseError.offset).arg(parseError.errorString());
        return false;
    }
    return checkpointFromJson(document.object(), checkpoint, error);
}

bool RoomPersistence::loadCheckpoint(RoomCheckpoint* checkpoint, QString* error) const {
    if (error) error->clear();
    if (!checkpoint) {
        if (error) *error = QStringLiteral("检查点输出参数为空");
        return false;
    }
    return loadCheckpointFile(m_checkpointPath, checkpoint, error);
}

bool RoomPersistence::appendEvent(quint64 sequence, const QString& kind,
                                  const QJsonObject& payload, QString* error) const {
    if (error) error->clear();
    if (sequence == 0 || kind.isEmpty()) {
        if (error) *error = QStringLiteral("持久化事件序号或类型无效");
        return false;
    }
    if (!ensureParentDirectory(m_eventLogPath, error)) return false;
    QFileInfo info(m_eventLogPath);
    if (info.exists() && info.size() >= kMaxEventLogBytes) {
        const QString rotated = m_eventLogPath + QStringLiteral(".1");
        QFile::remove(rotated);
        if (!QFile::rename(m_eventLogPath, rotated)) {
            if (error) *error = QStringLiteral("无法轮换事件日志");
            return false;
        }
    }
    QFile file(m_eventLogPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) *error = QStringLiteral("无法写入事件日志: %1").arg(m_eventLogPath);
        return false;
    }
    const QJsonObject event{{QStringLiteral("eventSchemaVersion"), 1},
                            {QStringLiteral("sequence"), static_cast<qint64>(sequence)},
                            {QStringLiteral("kind"), kind},
                            {QStringLiteral("recordedAt"),
                             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                            {QStringLiteral("payload"), payload}};
    const QByteArray line = QJsonDocument(event).toJson(QJsonDocument::Compact) + '\n';
    if (file.write(line) != line.size() || !file.flush()) {
        if (error) *error = QStringLiteral("事件日志写入失败: %1").arg(m_eventLogPath);
        return false;
    }
#ifdef Q_OS_UNIX
    if (::fsync(file.handle()) != 0) {
        if (error) *error = QStringLiteral("事件日志同步到磁盘失败: %1").arg(m_eventLogPath);
        return false;
    }
#endif
    return true;
}

QJsonArray RoomPersistence::eventsAfter(quint64 sequence, QString* error) const {
    if (error) error->clear();
    QJsonArray events;
    quint64 lastSequence = sequence;
    const QStringList paths{m_eventLogPath + QStringLiteral(".1"), m_eventLogPath};
    for (const QString& path : paths) {
        QFile file(path);
        if (!file.exists()) continue;
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法读取事件日志: %1").arg(path);
            return {};
        }
        qint64 lineNumber = 0;
        while (!file.atEnd()) {
            ++lineNumber;
            const QByteArray line = file.readLine().trimmed();
            if (line.isEmpty()) continue;
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
            const QJsonObject event = document.object();
            const qint64 eventSequence = event.value(QStringLiteral("sequence")).toInteger();
            if (!document.isObject()
                || event.value(QStringLiteral("eventSchemaVersion")).toInt() != 1
                || eventSequence <= 0 || event.value(QStringLiteral("kind")).toString().isEmpty()
                || !event.value(QStringLiteral("payload")).isObject()) {
                if (error) {
                    *error = QStringLiteral("事件日志 %1 第 %2 行无效")
                                 .arg(path).arg(lineNumber);
                }
                return {};
            }
            if (static_cast<quint64>(eventSequence) <= sequence) continue;
            if (static_cast<quint64>(eventSequence) != lastSequence + 1) {
                if (error) *error = QStringLiteral("事件日志序号不连续");
                return {};
            }
            events.append(event);
            lastSequence = static_cast<quint64>(eventSequence);
        }
    }
    return events;
}

} // namespace gbr
