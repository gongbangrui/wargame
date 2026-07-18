#include "client/RemoteSessionAdapter.h"
#include "core/CommandResult.h"
#include "core/Scenario.h"
#include "protocol/Protocol.h"
#include "server/AuthPolicy.h"
#include "server/PersistenceStore.h"
#include "server/ServerConfig.h"
#include "server/ServerMetrics.h"
#include "server/SessionGateway.h"
#include "server/SimulationRoom.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QWebSocket>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace gbr;

namespace {

const QString kRedToken = QStringLiteral("soak-red-token-0123456789-0123456789-abcd");
const QByteArray kPepper = QByteArrayLiteral("soak-pepper-0123456789abcdef-0123456789abcdef");

struct Options {
    int durationSeconds = 8 * 60 * 60;
    int clients = 32;
    int units = 500;
    double maxTickP99Ms = 25.0;
    bool reconnectProbe = false;
    QString reportPath;
};

SessionIdentity redIdentity() {
    return {QStringLiteral("soak-red"), QStringLiteral("red"), QStringLiteral("red"),
            QStringLiteral("main"), QDateTime::currentDateTimeUtc().addDays(1)};
}

QJsonObject tokenConfig() {
    const SessionIdentity identity = redIdentity();
    return {{QStringLiteral("tokenHash"),
             QString::fromLatin1(AuthPolicy::hashToken(kRedToken, kPepper))},
            {QStringLiteral("userId"), identity.userId},
            {QStringLiteral("role"), identity.role},
            {QStringLiteral("side"), identity.side},
            {QStringLiteral("roomId"), identity.roomId},
            {QStringLiteral("expiresAt"), identity.expiresAt.toString(Qt::ISODate)}};
}

Scenario makeScenario(int unitCount) {
    Scenario scenario;
    scenario.map.name = QStringLiteral("soak-500");
    scenario.map.widthMeters = 40000.0;
    scenario.map.heightMeters = 30000.0;

    auto add = [&scenario](const QString& id, const QString& kind, const QString& side,
                           const GeoPos& pos, double speed) {
        ScenarioUnit unit;
        unit.id = id;
        unit.callsign = id;
        unit.kind = kind;
        unit.side = side;
        unit.pos = pos;
        unit.detectRange = kind == QLatin1String("commandpost") ? 5000.0 : 3500.0;
        unit.attackRange = 0.0;
        unit.commRange = 20000.0;
        unit.speed = speed;
        unit.maxHp = kind == QLatin1String("commandpost") ? 200.0 : 100.0;
        unit.attackPower = 0.0;
        if (speed > 0.0) {
            const double x2 = std::fmod(pos.x + 1800.0, scenario.map.widthMeters - 300.0) + 150.0;
            const double y2 = std::fmod(pos.y + 900.0, scenario.map.heightMeters - 300.0) + 150.0;
            unit.schedule = {{0.0, pos.x, pos.y}, {45.0, x2, y2}, {90.0, pos.x, pos.y}};
        }
        scenario.units.push_back(std::move(unit));
    };

    add(QStringLiteral("red_cp"), QStringLiteral("commandpost"), QStringLiteral("red"),
        {1000.0, 15000.0, 50.0}, 0.0);
    add(QStringLiteral("blue_cp"), QStringLiteral("commandpost"), QStringLiteral("blue"),
        {39000.0, 15000.0, 50.0}, 0.0);
    for (int index = 2; index < unitCount; ++index) {
        const bool red = (index % 2) == 0;
        const int grid = index - 2;
        const double x = 500.0 + (grid % 25) * 1550.0;
        const double y = 500.0 + ((grid / 25) % 19) * 1500.0;
        add(QStringLiteral("%1_r_%2").arg(red ? QStringLiteral("red") : QStringLiteral("blue"))
                .arg(index, 3, 10, QLatin1Char('0')),
            QStringLiteral("reconuav"), red ? QStringLiteral("red") : QStringLiteral("blue"),
            {x, y, 1500.0}, 65.0);
    }
    return scenario;
}

double percentile(QVector<double> values, double ratio) {
    if (values.isEmpty()) return 0.0;
    std::sort(values.begin(), values.end());
    const qsizetype index = std::clamp<qsizetype>(
        static_cast<qsizetype>(std::ceil(values.size() * ratio)) - 1,
        0, values.size() - 1);
    return values.at(index);
}

qint64 residentBytes() {
    QFile status(QStringLiteral("/proc/self/status"));
    if (!status.open(QIODevice::ReadOnly)) return 0;
    const QRegularExpression expression(QStringLiteral("^VmRSS:\\s+(\\d+)\\s+kB$"));
    while (!status.atEnd()) {
        const QString line = QString::fromUtf8(status.readLine()).trimmed();
        const auto match = expression.match(line);
        if (match.hasMatch()) return match.captured(1).toLongLong() * 1024;
    }
    return 0;
}

QString newId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

class SoakRunner final : public QObject {
public:
    SoakRunner(Options options, QObject* parent = nullptr)
        : QObject(parent), m_options(std::move(options)) {
        m_sampleTimer.setInterval(1000);
        connect(&m_sampleTimer, &QTimer::timeout, this, &SoakRunner::sample);
        m_commandTimer.setInterval(1000);
        connect(&m_commandTimer, &QTimer::timeout, this, &SoakRunner::sendCommands);
        m_finishTimer.setSingleShot(true);
        connect(&m_finishTimer, &QTimer::timeout, this, &SoakRunner::finish);
    }

    bool start(QString* error) {
        auto contextualize = [error](const QString& stage) {
            if (!error) return;
            *error = stage + QStringLiteral(": ")
                + (error->isEmpty() ? QStringLiteral("未提供详细错误") : *error);
        };
        if (!m_directory.isValid()) {
            if (error) *error = QStringLiteral("无法创建压测临时目录");
            return false;
        }
        QTcpServer portProbe;
        if (!portProbe.listen(QHostAddress::LocalHost, 0)) {
            if (error) *error = QStringLiteral("压测端口探测失败: %1").arg(portProbe.errorString());
            return false;
        }
        m_config.listenAddress = QHostAddress::LocalHost;
        m_config.port = portProbe.serverPort();
        m_config.healthPort = static_cast<quint16>(m_config.port + 1);
        portProbe.close();
        m_config.roomId = QStringLiteral("main");
        m_config.databasePath = m_directory.filePath(QStringLiteral("soak.sqlite3"));
        m_config.maxConnections = m_options.clients + (m_options.reconnectProbe ? 1 : 0);
        m_config.snapshotIntervalMs = 100;
        m_config.checkpointIntervalMs = 10000;
        m_config.tokenPepper = kPepper;
        m_config.tokens = QJsonArray{tokenConfig()};
        if (!m_auth.load(m_config.tokens, kPepper, error)) {
            contextualize(QStringLiteral("压测认证配置失败"));
            return false;
        }
        if (!m_persistence.open(m_config.databasePath, error)) {
            contextualize(QStringLiteral("压测数据库打开失败"));
            return false;
        }

        m_room = std::make_unique<SimulationRoom>(m_config, &m_persistence, this);
        if (!m_room->initialize(error)) {
            contextualize(QStringLiteral("压测房间初始化失败"));
            return false;
        }
        if (!m_room->engine()->setScenario(makeScenario(m_options.units))) {
            if (error) *error = QStringLiteral("压测场景加载失败: %1")
                                    .arg(m_room->engine()->lastError());
            return false;
        }
        m_metrics.startedAtMs = QDateTime::currentMSecsSinceEpoch();
        connect(m_room->engine(), &SimulationEngine::tickCompleted, this,
                [this](double durationMs) { m_tickDurations.append(durationMs); });
        connect(m_room.get(), &SimulationRoom::checkpointFailed, this,
                [this](const QString&) { ++m_checkpointSignals; });

        m_gateway = std::make_unique<SessionGateway>(m_config, &m_auth, &m_persistence,
                                                      m_room.get(), &m_metrics, this);
        if (!m_gateway->listen(error)) {
            contextualize(QStringLiteral("压测 WebSocket 监听失败"));
            return false;
        }

        for (int index = 0; index < m_options.clients; ++index) createClient(index);
        if (m_options.reconnectProbe) {
            m_probe = std::make_unique<RemoteSessionAdapter>(this);
            connect(m_probe.get(), &RemoteSessionAdapter::connectedChanged, this, [this] {
                if (m_probe->connected()) ++m_probeConnects;
                else ++m_probeDisconnects;
            });
            m_probe->connectToServer(serverUrl(), kRedToken);
        }
        m_elapsed.start();
        m_sampleTimer.start();
        m_finishTimer.start(m_options.durationSeconds * 1000);
        return true;
    }

private:
    QUrl serverUrl() const {
        return QUrl(QStringLiteral("ws://127.0.0.1:%1/ws").arg(m_gateway->serverPort()));
    }

    void createClient(int index) {
        auto client = std::make_unique<QWebSocket>();
        QWebSocket* socket = client.get();
        connect(socket, &QWebSocket::connected, this, [socket, index] {
            const QJsonObject hello = ProtocolCodec::envelope(
                QString::fromLatin1(ProtocolType::Hello),
                {{QStringLiteral("token"), kRedToken}}, newId(), {},
                QStringLiteral("soak-client-%1").arg(index));
            socket->sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(hello)));
        });
        connect(socket, &QWebSocket::textMessageReceived, this,
                [this, socket, index](const QString& message) { onClientMessage(socket, index, message); });
        connect(socket, &QWebSocket::disconnected, this, [this] {
            if (m_startedSimulation && !m_finishing) ++m_unexpectedDisconnects;
        });
        socket->open(serverUrl());
        m_clients.push_back(std::move(client));
    }

    void onClientMessage(QWebSocket* socket, int, const QString& message) {
        const ProtocolParseResult parsed = ProtocolCodec::parse(
            message.toUtf8(), ProtocolLimits::MaxSnapshotBytes);
        if (!parsed.accepted) {
            ++m_clientProtocolErrors;
            return;
        }
        const QJsonObject envelope = parsed.envelope;
        const QString type = envelope.value(QStringLiteral("type")).toString();
        const QJsonObject payload = envelope.value(QStringLiteral("payload")).toObject();
        if (type == QLatin1String(ProtocolType::Ping)) {
            socket->sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(
                ProtocolCodec::envelope(QString::fromLatin1(ProtocolType::Pong), {}, newId(), {},
                                        envelope.value(QStringLiteral("clientId")).toString()))));
        } else if (type == QLatin1String(ProtocolType::CommandResult)) {
            const QString commandId = payload.value(QStringLiteral("commandId")).toString();
            const auto it = m_commandSentAt.find(commandId);
            if (it != m_commandSentAt.end()) {
                m_commandLatencies.append(m_elapsed.elapsed() - it.value());
                m_commandSentAt.erase(it);
            }
        } else if (type == QLatin1String(ProtocolType::Error)) {
            ++m_serverErrors;
            const QString code = payload.value(QStringLiteral("code")).toString();
            ++m_serverErrorCodes[code.isEmpty() ? QStringLiteral("UNKNOWN") : code];
        }
    }

    void sample() {
        m_maxRssBytes = std::max(m_maxRssBytes, residentBytes());
        if (!m_startedSimulation && m_metrics.authenticatedConnections == expectedConnections()) {
            m_startedSimulation = true;
            m_room->engine()->setRunning(true);
            m_commandTimer.start();
        }
        if (!m_startedSimulation && m_elapsed.elapsed() > 60000) {
            m_startTimedOut = true;
            finish();
        }
    }

    int expectedConnections() const {
        return m_options.clients + (m_options.reconnectProbe ? 1 : 0);
    }

    void sendCommands() {
        if (!m_startedSimulation) return;
        for (int index = 0; index < m_clients.size(); ++index) {
            const QString commandId = QStringLiteral("soak-%1-%2").arg(index).arg(newId());
            m_commandSentAt.insert(commandId, m_elapsed.elapsed());
            const QJsonObject command = ProtocolCodec::envelope(
                QString::fromLatin1(ProtocolType::Command),
                {{QStringLiteral("commandId"), commandId},
                 {QStringLiteral("action"), QString::fromLatin1(CommandAction::SetSpeed)},
                 {QStringLiteral("args"), QJsonObject{{QStringLiteral("unitId"), QStringLiteral("red_r_002")},
                                                       {QStringLiteral("speed"), 65.0 + (index % 5)}}}},
                newId(), {}, QStringLiteral("soak-client-%1").arg(index), -1, 1);
            m_clients.at(index)->sendTextMessage(QString::fromUtf8(ProtocolCodec::encode(command)));
        }
    }

    void finish() {
        if (m_finishing) return;
        m_finishing = true;
        m_sampleTimer.stop();
        m_commandTimer.stop();
        m_finishTimer.stop();
        if (m_room) m_room->engine()->setRunning(false);
        if (m_probe) m_probe->disconnectFromServer();
        for (const auto& client : m_clients) client->close();
        if (m_gateway) m_gateway->close();
        QString checkpointError;
        const bool checkpointOk = m_room && m_room->checkpointNow(&checkpointError);
        const double tickP99 = percentile(m_tickDurations, 0.99);
        const double commandP95 = percentile(m_commandLatencies, 0.95);
        const bool passed = m_startedSimulation && !m_startTimedOut
            && m_unexpectedDisconnects == 0 && m_checkpointSignals == 0
            && checkpointOk && tickP99 <= m_options.maxTickP99Ms;
        QJsonObject serverErrorCodes;
        for (auto it = m_serverErrorCodes.constBegin(); it != m_serverErrorCodes.constEnd(); ++it) {
            serverErrorCodes.insert(it.key(), it.value());
        }
        QJsonObject report{
            {QStringLiteral("passed"), passed},
            {QStringLiteral("durationSeconds"), m_elapsed.elapsed() / 1000.0},
            {QStringLiteral("clients"), m_options.clients},
            {QStringLiteral("units"), m_options.units},
            {QStringLiteral("tickSamples"), m_tickDurations.size()},
            {QStringLiteral("tickP99Ms"), tickP99},
            {QStringLiteral("commandSamples"), m_commandLatencies.size()},
            {QStringLiteral("commandP95Ms"), commandP95},
            {QStringLiteral("maxRssBytes"), static_cast<double>(m_maxRssBytes)},
            {QStringLiteral("unexpectedDisconnects"), m_unexpectedDisconnects},
            {QStringLiteral("clientProtocolErrors"), m_clientProtocolErrors},
            {QStringLiteral("serverErrors"), m_serverErrors},
            {QStringLiteral("serverErrorCodes"), serverErrorCodes},
            {QStringLiteral("probeConnects"), m_probeConnects},
            {QStringLiteral("probeDisconnects"), m_probeDisconnects},
            {QStringLiteral("checkpointSignals"), m_checkpointSignals},
            {QStringLiteral("checkpointOk"), checkpointOk},
            {QStringLiteral("checkpointError"), checkpointError},
            {QStringLiteral("metrics"), QJsonObject{
                {QStringLiteral("commandsAccepted"), static_cast<double>(m_metrics.commandsAccepted)},
                {QStringLiteral("commandsRejected"), static_cast<double>(m_metrics.commandsRejected)},
                {QStringLiteral("rateLimited"), static_cast<double>(m_metrics.rateLimited)},
                {QStringLiteral("resyncRequests"), static_cast<double>(m_metrics.resyncRequests)},
                {QStringLiteral("deltasSent"), static_cast<double>(m_metrics.deltasSent)},
                {QStringLiteral("bytesSent"), static_cast<double>(m_metrics.bytesSent)},
            }},
        };
        const QByteArray json = QJsonDocument(report).toJson(QJsonDocument::Indented);
        if (m_options.reportPath.isEmpty()) {
            QTextStream(stdout) << json;
        } else {
            QSaveFile output(m_options.reportPath);
            if (!output.open(QIODevice::WriteOnly) || output.write(json) != json.size() || !output.commit()) {
                QTextStream(stderr) << QStringLiteral("无法写入压测报告: %1\n").arg(m_options.reportPath);
                QCoreApplication::exit(2);
                return;
            }
        }
        QCoreApplication::exit(passed ? 0 : 1);
    }

    Options m_options;
    QTemporaryDir m_directory;
    ServerConfig m_config;
    AuthPolicy m_auth;
    PersistenceStore m_persistence;
    std::unique_ptr<SimulationRoom> m_room;
    ServerMetrics m_metrics;
    std::unique_ptr<SessionGateway> m_gateway;
    std::vector<std::unique_ptr<QWebSocket>> m_clients;
    std::unique_ptr<RemoteSessionAdapter> m_probe;
    QHash<QString, qint64> m_commandSentAt;
    QVector<double> m_tickDurations;
    QVector<double> m_commandLatencies;
    QElapsedTimer m_elapsed;
    QTimer m_sampleTimer;
    QTimer m_commandTimer;
    QTimer m_finishTimer;
    qint64 m_maxRssBytes = 0;
    int m_unexpectedDisconnects = 0;
    int m_clientProtocolErrors = 0;
    int m_serverErrors = 0;
    QHash<QString, int> m_serverErrorCodes;
    int m_checkpointSignals = 0;
    int m_probeConnects = 0;
    int m_probeDisconnects = 0;
    bool m_startedSimulation = false;
    bool m_startTimedOut = false;
    bool m_finishing = false;
};

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("兵器推演本机联网长稳压测"));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("duration-seconds"), QStringLiteral("运行时长（秒）"),
                      QStringLiteral("seconds"), QStringLiteral("28800")});
    parser.addOption({QStringLiteral("clients"), QStringLiteral("客户端数量"),
                      QStringLiteral("count"), QStringLiteral("32")});
    parser.addOption({QStringLiteral("units"), QStringLiteral("场景单元数量"),
                      QStringLiteral("count"), QStringLiteral("500")});
    parser.addOption({QStringLiteral("max-tick-p99-ms"), QStringLiteral("允许的 tick p99 上限"),
                      QStringLiteral("ms"), QStringLiteral("25")});
    parser.addOption({QStringLiteral("reconnect-probe"), QStringLiteral("额外启动一个自动重连客户端")});
    parser.addOption({QStringLiteral("report"), QStringLiteral("JSON 报告路径"), QStringLiteral("path")});
    parser.process(application);

    bool ok = false;
    Options options;
    options.durationSeconds = parser.value(QStringLiteral("duration-seconds")).toInt(&ok);
    if (!ok || options.durationSeconds < 5 || options.durationSeconds > 24 * 60 * 60) return 2;
    options.clients = parser.value(QStringLiteral("clients")).toInt(&ok);
    if (!ok || options.clients < 1 || options.clients > 32) return 2;
    options.units = parser.value(QStringLiteral("units")).toInt(&ok);
    if (!ok || options.units < 4 || options.units > 500) return 2;
    options.maxTickP99Ms = parser.value(QStringLiteral("max-tick-p99-ms")).toDouble(&ok);
    if (!ok || !std::isfinite(options.maxTickP99Ms) || options.maxTickP99Ms <= 0.0) return 2;
    options.reconnectProbe = parser.isSet(QStringLiteral("reconnect-probe"));
    options.reportPath = parser.value(QStringLiteral("report"));

    SoakRunner runner(options);
    QString error;
    if (!runner.start(&error)) {
        QTextStream(stderr) << error << Qt::endl;
        return 2;
    }
    return application.exec();
}
