#pragma once

#include "AuthPolicy.h"

#include <QHash>
#include <QObject>
#include <QTcpServer>

namespace gbr {

class PersistenceStore;

class AdminServer : public QObject {
    Q_OBJECT
public:
    AdminServer(AuthPolicy* auth, PersistenceStore* persistence, QObject* parent = nullptr);
    bool listen(const QHostAddress& address, quint16 port, QString* error = nullptr);
    void close();

private:
    struct AdminSession { qint64 expiresAt = 0; QByteArray csrf; };
    void onConnection();
    void handle(QTcpSocket* socket);
    QByteArray page() const;
    QByteArray jsonResponse(int status, const QJsonObject& body,
                            const QByteArray& cookie = {}) const;
    QByteArray response(int status, const QByteArray& type, const QByteArray& body,
                        const QByteArray& extraHeaders = {}) const;
    bool authorized(QTcpSocket* socket, const QByteArray& request, QByteArray* csrf = nullptr);
    QString header(const QByteArray& request, const QByteArray& name) const;
    QJsonObject requestJson(const QByteArray& request) const;
    QByteArray cookie(const QByteArray& request, const QByteArray& name) const;
    void cleanupSessions();

    AuthPolicy* m_auth = nullptr;
    PersistenceStore* m_persistence = nullptr;
    QTcpServer m_server;
    QHash<QByteArray, AdminSession> m_sessions;
};

} // namespace gbr
