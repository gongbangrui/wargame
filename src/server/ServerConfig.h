#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace gbr {

struct ServerConfig {
    QHostAddress listenAddress = QHostAddress::LocalHost;
    quint16 port = 8080;
    QHostAddress healthAddress = QHostAddress::LocalHost;
    quint16 healthPort = 9090;
    QHostAddress adminAddress = QHostAddress::LocalHost;
    quint16 adminPort = 9091;
    QString databasePath = QStringLiteral("data/wargame.sqlite3");
    QString scenarioPath;
    QString roomId = QStringLiteral("main");
    int snapshotIntervalMs = 100;
    int checkpointIntervalMs = 10000;
    int maxConnections = 32;
    int maxPacketBytes = 256 * 1024;
    int maxSendQueueBytes = 1024 * 1024;
    int commandRatePerSecond = 20;
    int commandBurst = 40;
    bool allowPublicListen = false;
    QJsonArray tokens;
    QJsonArray accounts;
    QJsonObject admin;
    QByteArray tokenPepper;

    static bool loadFile(const QString& path, ServerConfig* result, QString* error = nullptr);
    bool validate(QString* error = nullptr) const;
};

} // namespace gbr
