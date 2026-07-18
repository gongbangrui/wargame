#include "HealthServer.h"

#include "ServerMetrics.h"

#include <QJsonDocument>
#include <QTcpSocket>

namespace gbr {

HealthServer::HealthServer(ServerMetrics* metrics, QObject* parent)
    : QObject(parent), m_metrics(metrics) {
    connect(&m_server, &QTcpServer::newConnection, this, &HealthServer::handleConnection);
}

bool HealthServer::listen(const QHostAddress& address, quint16 port, QString* error) {
    if (m_server.listen(address, port)) return true;
    if (error) *error = m_server.errorString();
    return false;
}

void HealthServer::handleConnection() {
    while (QTcpSocket* socket = m_server.nextPendingConnection()) {
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { handleRequest(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void HealthServer::handleRequest(QTcpSocket* socket) {
    if (socket->bytesAvailable() > 8192) {
        socket->disconnectFromHost();
        return;
    }
    const QByteArray request = socket->readAll();
    if (!request.contains("\r\n\r\n")) return;
    const QByteArray firstLine = request.left(request.indexOf("\r\n"));
    QByteArray body;
    QByteArray contentType;
    int status = 200;
    if (firstLine.startsWith("GET /healthz ")) {
        body = QJsonDocument(m_metrics->health(QStringLiteral("0.1.0"), m_ready))
                   .toJson(QJsonDocument::Compact);
        contentType = "application/json";
        if (!m_ready) status = 503;
    } else if (firstLine.startsWith("GET /metrics ")) {
        body = m_metrics->prometheus();
        contentType = "text/plain; version=0.0.4";
    } else {
        status = 404;
        body = "not found\n";
        contentType = "text/plain";
    }
    const QByteArray statusText = status == 200 ? "OK" : (status == 503 ? "Service Unavailable" : "Not Found");
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

} // namespace gbr
