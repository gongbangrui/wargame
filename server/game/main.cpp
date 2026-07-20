#include "GameServer.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

#include <csignal>

namespace {
volatile std::sig_atomic_t stopRequested = 0;

void requestStop(int) {
    stopRequested = 1;
}
}

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("wargame_server"));
    const quint16 port = qEnvironmentVariableIntValue("GAME_PORT") > 0
        ? static_cast<quint16>(qEnvironmentVariableIntValue("GAME_PORT"))
        : 8090;
    gbr::GameServer server;
    if (!server.listen(port)) {
        qCritical() << "无法监听 WebSocket 端口" << port;
        return 2;
    }
    qInfo() << "兵器推演联网服务器已监听" << port;
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    QTimer shutdownPoll;
    shutdownPoll.setInterval(100);
    QObject::connect(&shutdownPoll, &QTimer::timeout, &application, [&application]() {
        if (stopRequested != 0) application.quit();
    });
    shutdownPoll.start();
    return QCoreApplication::exec();
}
