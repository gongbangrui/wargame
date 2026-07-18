#include "ServerConfig.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>
#include <cmath>
#include <limits>

namespace gbr {

bool ServerConfig::loadFile(const QString& path, ServerConfig* result, QString* error) {
    if (error) error->clear();
    if (!result) {
        if (error) *error = QStringLiteral("配置输出对象不能为空");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开服务器配置: %1").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("服务器配置 JSON 无效: %1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject object = document.object();
    static const QSet<QString> allowedFields{
        QStringLiteral("listenAddress"), QStringLiteral("port"),
        QStringLiteral("healthAddress"), QStringLiteral("healthPort"),
        QStringLiteral("adminAddress"), QStringLiteral("adminPort"),
        QStringLiteral("databasePath"), QStringLiteral("scenarioPath"),
        QStringLiteral("roomId"), QStringLiteral("snapshotIntervalMs"),
        QStringLiteral("checkpointIntervalMs"), QStringLiteral("maxConnections"),
        QStringLiteral("maxPacketBytes"), QStringLiteral("maxSendQueueBytes"),
        QStringLiteral("commandRatePerSecond"), QStringLiteral("commandBurst"),
        QStringLiteral("allowPublicListen"), QStringLiteral("tokens"),
        QStringLiteral("accounts"), QStringLiteral("admin"),
    };
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (!allowedFields.contains(it.key())) {
            if (error) *error = QStringLiteral("服务器配置包含未知字段: %1").arg(it.key());
            return false;
        }
    }
    auto validString = [&object](const QString& key) {
        return !object.contains(key) || object.value(key).isString();
    };
    auto validInteger = [&object](const QString& key) {
        if (!object.contains(key) || !object.value(key).isDouble()) return !object.contains(key);
        const double value = object.value(key).toDouble();
        return std::isfinite(value) && std::floor(value) == value
            && value >= 0.0 && value <= std::numeric_limits<int>::max();
    };
    static const QStringList stringFields{
        QStringLiteral("listenAddress"), QStringLiteral("healthAddress"), QStringLiteral("adminAddress"),
        QStringLiteral("databasePath"), QStringLiteral("scenarioPath"),
        QStringLiteral("roomId"),
    };
    static const QStringList integerFields{
        QStringLiteral("port"), QStringLiteral("healthPort"), QStringLiteral("adminPort"),
        QStringLiteral("snapshotIntervalMs"), QStringLiteral("checkpointIntervalMs"),
        QStringLiteral("maxConnections"), QStringLiteral("maxPacketBytes"),
        QStringLiteral("maxSendQueueBytes"), QStringLiteral("commandRatePerSecond"),
        QStringLiteral("commandBurst"),
    };
    for (const QString& key : stringFields) {
        if (!validString(key)) {
            if (error) *error = QStringLiteral("服务器配置字段类型无效: %1").arg(key);
            return false;
        }
    }
    for (const QString& key : integerFields) {
        if (!validInteger(key)) {
            if (error) *error = QStringLiteral("服务器配置整数字段无效: %1").arg(key);
            return false;
        }
    }
    if ((object.contains(QStringLiteral("allowPublicListen"))
         && !object.value(QStringLiteral("allowPublicListen")).isBool())
        || !object.value(QStringLiteral("tokens")).isArray()
        || (object.contains(QStringLiteral("accounts")) && !object.value(QStringLiteral("accounts")).isArray())
        || (object.contains(QStringLiteral("admin")) && !object.value(QStringLiteral("admin")).isObject())) {
        if (error) *error = QStringLiteral("allowPublicListen、tokens、accounts 或 admin 字段类型无效");
        return false;
    }
    const int port = object.value(QStringLiteral("port")).toInt(8080);
    const int healthPort = object.value(QStringLiteral("healthPort")).toInt(9090);
    const int adminPort = object.value(QStringLiteral("adminPort")).toInt(9091);
    if (port < 1 || port > 65535 || healthPort < 1 || healthPort > 65535
        || adminPort < 1 || adminPort > 65535) {
        if (error) *error = QStringLiteral("服务端口必须在 1 至 65535 之间");
        return false;
    }
    ServerConfig config;
    config.listenAddress = QHostAddress(object.value(QStringLiteral("listenAddress")).toString(QStringLiteral("127.0.0.1")));
    config.port = static_cast<quint16>(port);
    config.healthAddress = QHostAddress(object.value(QStringLiteral("healthAddress")).toString(QStringLiteral("127.0.0.1")));
    config.healthPort = static_cast<quint16>(healthPort);
    config.adminAddress = QHostAddress(object.value(QStringLiteral("adminAddress")).toString(QStringLiteral("127.0.0.1")));
    config.adminPort = static_cast<quint16>(adminPort);
    config.databasePath = object.value(QStringLiteral("databasePath")).toString(config.databasePath);
    config.scenarioPath = object.value(QStringLiteral("scenarioPath")).toString();
    config.roomId = object.value(QStringLiteral("roomId")).toString(config.roomId);
    config.snapshotIntervalMs = object.value(QStringLiteral("snapshotIntervalMs")).toInt(config.snapshotIntervalMs);
    config.checkpointIntervalMs = object.value(QStringLiteral("checkpointIntervalMs")).toInt(config.checkpointIntervalMs);
    config.maxConnections = object.value(QStringLiteral("maxConnections")).toInt(config.maxConnections);
    config.maxPacketBytes = object.value(QStringLiteral("maxPacketBytes")).toInt(config.maxPacketBytes);
    config.maxSendQueueBytes = object.value(QStringLiteral("maxSendQueueBytes")).toInt(config.maxSendQueueBytes);
    config.commandRatePerSecond = object.value(QStringLiteral("commandRatePerSecond")).toInt(config.commandRatePerSecond);
    config.commandBurst = object.value(QStringLiteral("commandBurst")).toInt(config.commandBurst);
    config.allowPublicListen = object.value(QStringLiteral("allowPublicListen")).toBool(false);
    config.tokens = object.value(QStringLiteral("tokens")).toArray();
    config.accounts = object.value(QStringLiteral("accounts")).toArray();
    config.admin = object.value(QStringLiteral("admin")).toObject();
    config.tokenPepper = qgetenv("WARGAME_TOKEN_PEPPER");
    *result = config;
    return result->validate(error);
}

bool ServerConfig::validate(QString* error) const {
    if (error) error->clear();
    auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (listenAddress.isNull() || healthAddress.isNull()) return fail(QStringLiteral("监听地址无效"));
    if (port == 0 || healthPort == 0 || adminPort == 0
        || port == healthPort || port == adminPort || healthPort == adminPort) {
        return fail(QStringLiteral("服务端口无效或重复"));
    }
    if (!allowPublicListen && listenAddress != QHostAddress::LocalHost
        && listenAddress != QHostAddress::LocalHostIPv6) {
        return fail(QStringLiteral("默认只允许本机监听；公网/容器监听需显式设置 allowPublicListen=true"));
    }
    if (adminAddress.isNull()) return fail(QStringLiteral("管理平台地址无效"));
    if (!allowPublicListen && adminAddress != QHostAddress::LocalHost
        && adminAddress != QHostAddress::LocalHostIPv6) {
        return fail(QStringLiteral("管理平台地址无效"));
    }
    if (databasePath.trimmed().isEmpty() || roomId.trimmed().isEmpty()) return fail(QStringLiteral("databasePath 和 roomId 不能为空"));
    if (snapshotIntervalMs < 50 || snapshotIntervalMs > 5000
        || checkpointIntervalMs < 1000 || checkpointIntervalMs > 3600000) {
        return fail(QStringLiteral("快照或检查点间隔超出允许范围"));
    }
    if (maxConnections < 1 || maxConnections > 1024
        || maxPacketBytes < 1024 || maxPacketBytes > 8 * 1024 * 1024
        || maxSendQueueBytes < 64 * 1024 || maxSendQueueBytes > 64 * 1024 * 1024) {
        return fail(QStringLiteral("连接数或数据大小限制无效"));
    }
    if (commandRatePerSecond < 1 || commandRatePerSecond > 1000
        || commandBurst < commandRatePerSecond || commandBurst > 5000) {
        return fail(QStringLiteral("命令限流参数无效"));
    }
    if (tokenPepper.size() < 32) return fail(QStringLiteral("环境变量 WARGAME_TOKEN_PEPPER 至少需要 32 字节"));
    if (tokens.isEmpty()) return fail(QStringLiteral("配置必须至少包含一个 tokenHash"));
    return true;
}

} // namespace gbr
