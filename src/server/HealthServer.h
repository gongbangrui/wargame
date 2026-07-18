#pragma once

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>

namespace gbr {

struct ServerMetrics;

class HealthServer : public QObject {
    Q_OBJECT
public:
    explicit HealthServer(ServerMetrics* metrics, QObject* parent = nullptr);
    bool listen(const QHostAddress& address, quint16 port, QString* error = nullptr);
    void setReady(bool ready) { m_ready = ready; }

private:
    void handleConnection();
    void handleRequest(QTcpSocket* socket);

    QTcpServer m_server;
    ServerMetrics* m_metrics = nullptr;
    bool m_ready = false;
};

} // namespace gbr
