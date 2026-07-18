#include "AuthPolicy.h"
#include "AdminServer.h"
#include "HealthServer.h"
#include "PersistenceStore.h"
#include "RoomManager.h"
#include "ServerConfig.h"
#include "ServerMetrics.h"
#include "SessionGateway.h"
#include "SimulationRoom.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTextStream>
#include <QTcpSocket>
#include <QUrl>

#include <csignal>

namespace {

void installTerminationHandler() {
    std::signal(SIGINT, [](int) { QCoreApplication::quit(); });
    std::signal(SIGTERM, [](int) { QCoreApplication::quit(); });
}

int fail(const QString& message) {
    QTextStream(stderr) << message << Qt::endl;
    return 1;
}

bool healthProbe(const QUrl& url, QString* error) {
    if (!url.isValid() || url.scheme() != QLatin1String("http") || url.host().isEmpty()) {
        if (error) *error = QStringLiteral("健康检查 URL 必须是有效的 http:// 地址");
        return false;
    }
    QTcpSocket socket;
    socket.connectToHost(url.host(), static_cast<quint16>(url.port(80)));
    if (!socket.waitForConnected(2000)) {
        if (error) *error = socket.errorString();
        return false;
    }
    const QByteArray path = url.path(QUrl::FullyEncoded).isEmpty()
        ? QByteArrayLiteral("/") : url.path(QUrl::FullyEncoded).toUtf8();
    socket.write("GET " + path + " HTTP/1.1\r\nHost: " + url.host().toUtf8()
                 + "\r\nConnection: close\r\n\r\n");
    if (!socket.waitForBytesWritten(1000) || !socket.waitForReadyRead(2000)) {
        if (error) *error = socket.errorString();
        return false;
    }
    QByteArray response;
    do {
        response += socket.readAll();
    } while (socket.waitForReadyRead(100));
    if (!response.startsWith("HTTP/1.1 200") || !response.contains("\"status\":\"ok\"")) {
        if (error) *error = QStringLiteral("健康检查返回非健康状态");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("wargame_server"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("兵器推演权威无头服务器"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("config"), QStringLiteral("服务器 JSON 配置路径"),
                      QStringLiteral("path"), QStringLiteral("config/server.dev.json")});
    parser.addOption({QStringLiteral("check-config"), QStringLiteral("校验配置后退出")});
    parser.addOption({QStringLiteral("hash-token-stdin"), QStringLiteral("从标准输入读取 token 并计算 tokenHash")});
    parser.addOption({QStringLiteral("hash-password-stdin"), QStringLiteral("从标准输入读取密码并输出 salt/passwordHash")});
    parser.addOption({QStringLiteral("health-probe"), QStringLiteral("检查健康接口后退出"),
                      QStringLiteral("url")});
    parser.addOption({QStringLiteral("backup"), QStringLiteral("创建 SQLite 一致性备份后退出"),
                      QStringLiteral("path")});
    parser.process(application);

    const QByteArray pepper = qgetenv("WARGAME_TOKEN_PEPPER");
    if (parser.isSet(QStringLiteral("health-probe"))) {
        QString error;
        if (!healthProbe(QUrl(parser.value(QStringLiteral("health-probe"))), &error)) {
            return fail(error);
        }
        return 0;
    }
    if (parser.isSet(QStringLiteral("hash-token-stdin"))) {
        if (pepper.size() < 32) return fail(QStringLiteral("WARGAME_TOKEN_PEPPER 至少需要 32 字节"));
        const QString token = QTextStream(stdin).readLine().trimmed();
        if (token.size() < 32) return fail(QStringLiteral("token 至少需要 32 个字符"));
        QTextStream(stdout) << gbr::AuthPolicy::hashToken(token, pepper) << Qt::endl;
        return 0;
    }
    if (parser.isSet(QStringLiteral("hash-password-stdin"))) {
        const QString password = QTextStream(stdin).readLine();
        if (password.size() < 8 || password.size() > 512) return fail(QStringLiteral("密码长度必须在 8 至 512 个字符之间"));
        const auto account = gbr::AuthPolicy::makePasswordAccount(
            QStringLiteral("admin"), password,
            {QStringLiteral("admin"), QStringLiteral("admin"), {}, QStringLiteral("main"), QDateTime::currentDateTimeUtc().addYears(10)});
        QTextStream(stdout) << QJsonDocument(QJsonObject{
            {QStringLiteral("salt"), QString::fromLatin1(account.salt.toHex())},
            {QStringLiteral("passwordHash"), QString::fromLatin1(account.passwordHash)},
        }).toJson(QJsonDocument::Compact) << Qt::endl;
        return 0;
    }

    gbr::ServerConfig config;
    QString error;
    if (!gbr::ServerConfig::loadFile(parser.value(QStringLiteral("config")), &config, &error)) {
        return fail(QStringLiteral("配置无效: %1").arg(error));
    }
    gbr::AuthPolicy auth;
    if (!auth.load(config.tokens, config.tokenPepper, &error)) {
        return fail(QStringLiteral("认证配置无效: %1").arg(error));
    }
    if (parser.isSet(QStringLiteral("check-config"))) {
        QTextStream(stdout) << QStringLiteral("配置有效") << Qt::endl;
        return 0;
    }

    gbr::PersistenceStore persistence;
    if (!persistence.open(config.databasePath, &error)) {
        return fail(QStringLiteral("数据库打开失败: %1").arg(error));
    }
    if (!persistence.syncTokenRecords(auth.records(), &error)) {
        return fail(QStringLiteral("token 身份同步失败: %1").arg(error));
    }
    QVector<gbr::PasswordAccount> accounts;
    for (const auto& value : config.accounts) {
        const QJsonObject object = value.toObject();
        const gbr::SessionIdentity identity{
            object.value(QStringLiteral("userId")).toString(),
            object.value(QStringLiteral("role")).toString(),
            object.value(QStringLiteral("side")).toString(),
            object.value(QStringLiteral("roomId")).toString(),
            QDateTime::fromString(object.value(QStringLiteral("expiresAt")).toString(), Qt::ISODate)};
        gbr::PasswordAccount account;
        account.username = object.value(QStringLiteral("username")).toString();
        account.salt = QByteArray::fromHex(object.value(QStringLiteral("salt")).toString().toLatin1());
        account.passwordHash = object.value(QStringLiteral("passwordHash")).toString().toLatin1();
        account.identity = identity;
        account.disabled = object.value(QStringLiteral("disabled")).toBool(false);
        accounts.push_back(account);
    }
    const QVector<gbr::PasswordAccount> storedAccounts = persistence.passwordAccounts(&error);
    if (!error.isEmpty()) return fail(QStringLiteral("账号读取失败: %1").arg(error));
    if (storedAccounts.isEmpty() && !accounts.isEmpty()) {
        if (!persistence.syncPasswordAccounts(accounts, &error)) return fail(QStringLiteral("账号初始化失败: %1").arg(error));
    } else {
        accounts = storedAccounts;
    }
    if (!auth.setPasswordAccounts(accounts, &error)) return fail(QStringLiteral("账号配置无效: %1").arg(error));
    if (config.admin.contains(QStringLiteral("username"))) {
        gbr::PasswordAccount admin;
        admin.username = config.admin.value(QStringLiteral("username")).toString();
        admin.salt = QByteArray::fromHex(config.admin.value(QStringLiteral("salt")).toString().toLatin1());
        admin.passwordHash = config.admin.value(QStringLiteral("passwordHash")).toString().toLatin1();
        admin.identity = {QStringLiteral("admin"), QStringLiteral("admin"), {}, config.roomId,
                          QDateTime::currentDateTimeUtc().addYears(10)};
        auth.setAdminAccount(admin);
    }
    if (parser.isSet(QStringLiteral("backup"))) {
        if (!persistence.backupTo(parser.value(QStringLiteral("backup")), &error)) {
            return fail(QStringLiteral("备份失败: %1").arg(error));
        }
        QTextStream(stdout) << QStringLiteral("备份完成") << Qt::endl;
        return 0;
    }

    gbr::RoomManager rooms(config, &persistence);
    if (!rooms.initialize(&error)) return fail(QStringLiteral("房间初始化失败: %1").arg(error));

    gbr::ServerMetrics metrics;
    metrics.startedAtMs = QDateTime::currentMSecsSinceEpoch();
    QObject::connect(rooms.room()->engine(), &gbr::SimulationEngine::tickCompleted,
                     &application, [&metrics](double durationMs) {
                         metrics.lastTickDurationMs = durationMs;
                     });
    QObject::connect(rooms.room(), &gbr::SimulationRoom::checkpointFailed,
                     &application, [&metrics](const QString& message) {
                         ++metrics.checkpointFailures;
                         qWarning().noquote() << QJsonDocument(QJsonObject{
                             {QStringLiteral("level"), QStringLiteral("error")},
                             {QStringLiteral("event"), QStringLiteral("checkpoint_failed")},
                             {QStringLiteral("message"), message},
                         }).toJson(QJsonDocument::Compact);
                     });

    gbr::HealthServer health(&metrics);
    if (!health.listen(config.healthAddress, config.healthPort, &error)) {
        return fail(QStringLiteral("健康检查端口监听失败: %1").arg(error));
    }
    gbr::SessionGateway gateway(config, &auth, &persistence, rooms.room(), &metrics);
    if (!gateway.listen(&error)) return fail(QStringLiteral("WebSocket 监听失败: %1").arg(error));

    gbr::AdminServer admin(&auth, &persistence);
    if (!admin.listen(config.adminAddress, config.adminPort, &error)) {
        return fail(QStringLiteral("管理平台监听失败: %1").arg(error));
    }

    installTerminationHandler();
    health.setReady(true);
    QTextStream(stdout) << QStringLiteral("wargame_server 已监听 %1:%2，健康端口 %3:%4")
                              .arg(config.listenAddress.toString()).arg(config.port)
                              .arg(config.healthAddress.toString()).arg(config.healthPort)
                        << Qt::endl;
    const int exitCode = application.exec();
    health.setReady(false);
    admin.close();
    gateway.close();
    if (!rooms.room()->checkpointNow(&error)) {
        QTextStream(stderr) << QStringLiteral("退出检查点失败: %1").arg(error) << Qt::endl;
    }
    return exitCode;
}
