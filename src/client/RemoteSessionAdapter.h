#pragma once

#include "ClientStateStore.h"
#include "ISessionAdapter.h"

#include <QMap>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

namespace gbr {

class RemoteSessionAdapter : public ISessionAdapter {
    Q_OBJECT
public:
    explicit RemoteSessionAdapter(QObject* parent = nullptr);

    void connectToServer(const QUrl& url, const QString& token);
    void connectToServerWithPassword(const QUrl& url, const QString& username,
                                     const QString& password);
    void disconnectFromServer();

    bool isRemote() const override { return true; }
    bool connected() const override { return m_authenticated; }
    QString connectionState() const override { return m_connectionState; }
    QString role() const override { return m_store.role(); }
    QString side() const override { return m_store.side(); }
    double simTime() const override { return m_store.simTime(); }
    bool running() const override { return m_store.running(); }
    bool readyForSim() const override { return m_store.readyForSim(); }
    QString cpIssues() const override { return m_store.cpIssues(); }
    QString lastError() const override { return m_store.lastError(); }
    QVariantList units() const override { return m_store.unitsForView(); }
    QVariantList messages() const override { return m_store.messages(); }
    QVariantMap lobby() const override { return m_store.lobby(); }
    QVariantList chatMessages() const override { return m_store.chatMessages(); }
    QJsonObject mapInfo() const override { return m_store.mapInfo(); }
    QJsonArray allUnits() const override { return m_store.allUnits(); }
    QJsonObject unitAt(const QString& id) const override { return m_store.unitAt(id); }
    QJsonObject scenarioJson() const override { return m_store.scenarioJson(); }
    QVariantMap command(const QString& action, const QVariantMap& args) override;
    void setRunning(bool running) override;
    void setSpeed(double speed) override;
    void stepOnce() override;

private:
    void openSocket();
    void onConnected();
    void onDisconnected();
    void onTextMessage(const QString& text);
    void scheduleReconnect();
    void send(const QString& type, const QJsonObject& payload);
    void requestResync();
    void resendPendingCommands();
    void setConnectionState(const QString& state);
    QString newId() const;

    ClientStateStore m_store;
    QWebSocket m_socket;
    QTimer m_reconnectTimer;
    QUrl m_url;
    QString m_token;
    QString m_username;
    QString m_password;
    QString m_clientId;
    QMap<QString, QJsonObject> m_pendingCommands;
    qint64 m_lastEnvelopeSequence = 0;
    int m_reconnectAttempt = 0;
    bool m_authenticated = false;
    bool m_manualDisconnect = false;
    bool m_reconnectAllowed = true;
    QString m_connectionState = QStringLiteral("disconnected");
};

} // namespace gbr
