#pragma once

#include "AiPlan.h"
#include "OllamaPlanningContract.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <optional>

namespace gbr {

struct OllamaConfig {
    QString deployment = QStringLiteral("auto");
    QString baseUrl = QStringLiteral("http:") + QStringLiteral("//127.0.0.1:11434");
    QString model = QStringLiteral("auto");
    int connectionTimeoutMs = 1500;
    int totalTimeoutMs = 15000;
    int maxResponseBytes = 64 * 1024;
};

struct OllamaResult {
    bool ok = false;
    QString failureClass;
    QString requestId;
    qint64 latencyMs = 0;
    AiPlanV1 plan;
    QString configuredModel;
    QString resolvedModel;
    QJsonArray messages;
    QJsonValue rawResponse = QJsonValue::Null;
    QJsonValue parsedPlan = QJsonValue::Null;
};

class OllamaProvider final : public QObject {
    Q_OBJECT
public:
    explicit OllamaProvider(const OllamaConfig& config,
                            QNetworkAccessManager* network,
                            QObject* parent = nullptr);
    ~OllamaProvider() override;

    static OllamaConfig fromEnvironment(QString* error = nullptr);
    static bool validateBaseUrl(const QString& value, QString* normalized = nullptr,
                                QString* error = nullptr);

    void reconfigure(const OllamaConfig& config);
    const OllamaConfig& config() const { return m_config; }
    QString deployment() const { return m_config.deployment; }
    QString baseUrl() const { return m_config.baseUrl; }
    QString model() const { return m_config.model; }
    QString configuredModel() const { return m_config.model; }
    QString resolvedModel() const { return m_resolvedModel; }
    bool inFlight() const { return m_reply != nullptr; }

    static QJsonArray messagesFor(const OllamaPlanRequest& request);
    static QJsonObject schemaFor(const OllamaPlanRequest& request);

    void probe(quint64 matchGeneration, std::function<void(OllamaResult)> callback);
    void requestPlan(const QJsonObject& projection, const QString& requestId,
                     quint64 matchGeneration, quint64 sourceStateRevision,
                     quint64 planningGeneration,
                     std::function<void(OllamaResult)> callback);
    void requestPlan(const OllamaPlanRequest& request,
                     std::function<void(OllamaResult)> callback);
    std::optional<OllamaResult> cancel();

private:
    void startPlanRequest(const OllamaPlanRequest& request,
                          std::function<void(OllamaResult)> callback);
    QNetworkRequest requestFor(const QString& path) const;
    void monitorReply(QNetworkReply* reply);
    void stopTimers();
    bool appendBody(QNetworkReply* reply);
    OllamaResult attemptResult(const QString& failureClass = QString()) const;
    void finishAttempt(OllamaResult* result);

    OllamaConfig m_config;
    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_reply = nullptr;
    QTimer* m_connectionTimeout = nullptr;
    QTimer* m_totalTimeout = nullptr;
    QByteArray m_body;
    QString m_abortFailure;
    quint64 m_probeGeneration = 0;
    bool m_probeValid = false;
    QString m_probeFailureClass;
    QString m_resolvedModel;
    bool m_planAttemptActive = false;
    qint64 m_planAttemptStartedAt = 0;
    OllamaPlanRequest m_activePlanRequest;
    QJsonArray m_activeMessages;
};

}
