#include "OllamaProvider.h"

// allow: SIZE_OK - one async adapter state machine keeps reply cancellation and parsing together.

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkRequest>
#include <QUrl>

namespace gbr {

namespace {

QString deploymentValue() {
    const QString value = qEnvironmentVariable("AI_PROVIDER", "auto").trimmed().toLower();
    return value.isEmpty() ? QStringLiteral("auto") : value;
}

int boundedEnvironmentInt(const char* name, int fallback, int minimum, int maximum) {
    bool ok = false;
    const int value = qEnvironmentVariable(name).toInt(&ok);
    return ok ? qBound(minimum, value, maximum) : fallback;
}

OllamaResult failure(const QString& kind, const OllamaConfig& config,
                    const QString& requestId = {}) {
    OllamaResult result;
    result.failureClass = kind;
    result.requestId = requestId;
    result.configuredModel = config.model;
    return result;
}

}

OllamaProvider::OllamaProvider(const OllamaConfig& config,
                               QNetworkAccessManager* network,
                               QObject* parent)
    : QObject(parent), m_config(config), m_network(network),
      m_connectionTimeout(new QTimer(this)), m_totalTimeout(new QTimer(this)) {
    m_connectionTimeout->setSingleShot(true);
    m_totalTimeout->setSingleShot(true);
    connect(m_connectionTimeout, &QTimer::timeout, this, [this]() {
        if (!m_reply) return;
        m_abortFailure = QStringLiteral("timeout");
        m_reply->abort();
    });
    connect(m_totalTimeout, &QTimer::timeout, this, [this]() {
        if (!m_reply) return;
        m_abortFailure = QStringLiteral("timeout");
        m_reply->abort();
    });
}

OllamaProvider::~OllamaProvider() {
    (void)cancel();
}

void OllamaProvider::reconfigure(const OllamaConfig& config) {
    (void)cancel();
    m_config = config;
    m_probeGeneration = 0;
    m_probeValid = false;
    m_probeFailureClass.clear();
    m_resolvedModel.clear();
}

OllamaConfig OllamaProvider::fromEnvironment(QString* error) {
    if (error) error->clear();
    OllamaConfig config;
    config.deployment = deploymentValue();
    if (config.deployment != QLatin1String("rules")
        && config.deployment != QLatin1String("auto")
        && config.deployment != QLatin1String("ollama")) {
        if (error) *error = QStringLiteral("AI_PROVIDER 必须是 rules、auto 或 ollama");
        config.deployment = QStringLiteral("rules");
        return config;
    }
    const QString base = qEnvironmentVariable("OLLAMA_BASE_URL", config.baseUrl).trimmed();
    QString normalized;
    if (!validateBaseUrl(base, &normalized, error)) {
        config.deployment = QStringLiteral("rules");
        return config;
    }
    config.baseUrl = normalized;
    config.model = qEnvironmentVariable("OLLAMA_MODEL", config.model).trimmed();
    if (config.model.isEmpty() || config.model.size() > 128) {
        if (error) *error = QStringLiteral("OLLAMA_MODEL 无效");
        config.deployment = QStringLiteral("rules");
        return config;
    }
    config.connectionTimeoutMs = boundedEnvironmentInt(
        "OLLAMA_CONNECT_TIMEOUT_MS", DefaultOllamaConnectionTimeoutMs, 100,
        DefaultOllamaConnectionTimeoutMs);
    config.totalTimeoutMs = boundedEnvironmentInt(
        "OLLAMA_TIMEOUT_MS", DefaultOllamaRequestTimeoutMs,
        MinimumOllamaRequestTimeoutMs, MaximumOllamaRequestTimeoutMs);
    config.maxResponseBytes = boundedEnvironmentInt("OLLAMA_MAX_RESPONSE_BYTES", 64 * 1024,
                                                     1024, 64 * 1024);
    return config;
}

bool OllamaProvider::validateBaseUrl(const QString& value, QString* normalized, QString* error) {
    const QUrl url(value.trimmed());
    if (!url.isValid() || url.isRelative()
        || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        || url.host().isEmpty() || !url.userInfo().isEmpty()
        || !url.query().isEmpty() || !url.fragment().isEmpty()) {
        if (error) *error = QStringLiteral("OLLAMA_BASE_URL 必须是无用户信息、查询参数和片段的 http/https URL");
        return false;
    }
    QUrl canonical = url;
    canonical.setPath(QDir::cleanPath(url.path().isEmpty() ? QStringLiteral("/") : url.path()));
    QString result = canonical.toString(QUrl::FullyEncoded);
    while (result.endsWith(QLatin1Char('/'))) result.chop(1);
    if (normalized) *normalized = result;
    return true;
}

QNetworkRequest OllamaProvider::requestFor(const QString& path) const {
    QNetworkRequest request(QUrl(m_config.baseUrl + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(m_config.totalTimeoutMs);
    return request;
}

QJsonArray OllamaProvider::messagesFor(const OllamaPlanRequest& request) {
    return OllamaPlanningContract::messagesFor(request);
}

QJsonObject OllamaProvider::schemaFor(const OllamaPlanRequest& request) {
    return OllamaPlanningContract::schemaFor(request);
}

void OllamaProvider::monitorReply(QNetworkReply* reply) {
    m_abortFailure.clear();
    m_connectionTimeout->start(m_config.connectionTimeoutMs);
    // A plan may need a model-list probe before the chat request. Both legs
    // consume the same configured planning budget rather than each receiving
    // a full timeout window.
    m_totalTimeout->start(qMax(1, remainingPlanTimeoutMs()));
    const auto connected = [this, reply]() {
        if (m_reply == reply) m_connectionTimeout->stop();
    };
    connect(reply, &QNetworkReply::requestSent, this, connected);
    connect(reply, &QNetworkReply::metaDataChanged, this, connected);
    connect(reply, &QObject::destroyed, this, [this, reply]() {
        if (m_reply != reply) return;
        m_reply = nullptr;
        stopTimers();
        m_body.clear();
        m_abortFailure.clear();
    });
}

void OllamaProvider::stopTimers() {
    m_connectionTimeout->stop();
    m_totalTimeout->stop();
}

int OllamaProvider::remainingPlanTimeoutMs() const {
    if (!m_planAttemptActive || m_planAttemptStartedAt <= 0) {
        return m_config.totalTimeoutMs;
    }
    const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - m_planAttemptStartedAt;
    if (elapsed >= m_config.totalTimeoutMs) return 0;
    return m_config.totalTimeoutMs - static_cast<int>(elapsed);
}

bool OllamaProvider::appendBody(QNetworkReply* reply) {
    const QByteArray chunk = reply->readAll();
    if (m_body.size() + chunk.size() > m_config.maxResponseBytes) {
        m_abortFailure = QStringLiteral("response_too_large");
        reply->abort();
        return false;
    }
    m_body.append(chunk);
    return true;
}

OllamaResult OllamaProvider::attemptResult(const QString& failureClass) const {
    OllamaResult result;
    result.failureClass = failureClass;
    result.requestId = m_activePlanRequest.requestId;
    result.configuredModel = m_config.model;
    result.resolvedModel = m_resolvedModel;
    result.messages = m_activeMessages;
    result.latencyMs = m_planAttemptStartedAt > 0
        ? QDateTime::currentMSecsSinceEpoch() - m_planAttemptStartedAt : 0;
    if (!m_body.isEmpty()) result.rawResponse = QString::fromUtf8(m_body);
    return result;
}

void OllamaProvider::finishAttempt(OllamaResult* result) {
    if (!result) return;
    if (m_planAttemptActive) {
        if (result->requestId.isEmpty()) result->requestId = m_activePlanRequest.requestId;
        if (result->configuredModel.isEmpty()) result->configuredModel = m_config.model;
        if (result->resolvedModel.isEmpty()) result->resolvedModel = m_resolvedModel;
        if (result->messages.isEmpty()) result->messages = m_activeMessages;
        if (result->latencyMs <= 0 && m_planAttemptStartedAt > 0) {
            result->latencyMs = QDateTime::currentMSecsSinceEpoch() - m_planAttemptStartedAt;
        }
        m_planAttemptActive = false;
        m_planAttemptStartedAt = 0;
        m_activePlanRequest = {};
        m_activeMessages = {};
    }
}

void OllamaProvider::probe(quint64 matchGeneration, std::function<void(OllamaResult)> callback) {
    if (m_config.deployment == QLatin1String("rules")) {
        callback(failure(QStringLiteral("rules_disabled"), m_config));
        return;
    }
    if (m_reply) {
        callback(failure(QStringLiteral("busy"), m_config));
        return;
    }
    m_probeGeneration = matchGeneration;
    m_body.clear();
    m_resolvedModel.clear();
    m_probeFailureClass.clear();
    m_reply = m_network ? m_network->get(requestFor(QStringLiteral("/api/tags"))) : nullptr;
    if (!m_reply) {
        m_probeValid = false;
        m_probeFailureClass = QStringLiteral("unavailable");
        callback(failure(m_probeFailureClass, m_config));
        return;
    }
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply* const requestReply = m_reply;
    monitorReply(requestReply);
    connect(requestReply, &QNetworkReply::readyRead, this, [this, requestReply]() {
        if (m_reply == requestReply) appendBody(requestReply);
    });
    connect(requestReply, &QNetworkReply::finished, this,
            [this, requestReply, callback, started]() mutable {
        if (m_reply != requestReply) return;
        QNetworkReply* reply = requestReply;
        m_reply = nullptr;
        stopTimers();
        OllamaResult result;
        result.configuredModel = m_config.model;
        result.latencyMs = m_planAttemptActive && m_planAttemptStartedAt > 0
            ? QDateTime::currentMSecsSinceEpoch() - m_planAttemptStartedAt
            : QDateTime::currentMSecsSinceEpoch() - started;
        if (!m_body.isEmpty()) result.rawResponse = QString::fromUtf8(m_body);
        if (!m_abortFailure.isEmpty()) {
            m_probeValid = false;
            m_probeFailureClass = m_abortFailure;
            result.failureClass = m_probeFailureClass;
        } else if (reply->error() != QNetworkReply::NoError) {
            m_probeValid = false;
            m_probeFailureClass = reply->error() == QNetworkReply::OperationCanceledError
                ? QStringLiteral("timeout") : QStringLiteral("unavailable");
            result.failureClass = m_probeFailureClass;
        } else if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
            m_probeValid = false;
            m_probeFailureClass = QStringLiteral("http_error");
            result.failureClass = m_probeFailureClass;
        } else {
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(m_body, &parseError);
            const QJsonValue modelsValue = document.object().value(QStringLiteral("models"));
            if (parseError.error != QJsonParseError::NoError) {
                m_probeValid = false;
                m_probeFailureClass = QStringLiteral("invalid_json");
                result.failureClass = m_probeFailureClass;
            } else if (!document.isObject() || !modelsValue.isArray()) {
                m_probeValid = false;
                m_probeFailureClass = QStringLiteral("schema_invalid");
                result.failureClass = m_probeFailureClass;
            } else {
                const OllamaModelSelection selection = OllamaPlanningContract::selectModel(
                    m_config.model, modelsValue.toArray());
                m_probeValid = true;
                m_resolvedModel = selection.resolvedModel;
                m_probeFailureClass = selection.failureClass;
                result.ok = selection.failureClass.isEmpty();
                result.failureClass = selection.failureClass;
                result.resolvedModel = m_resolvedModel;
            }
        }
        reply->deleteLater();
        callback(result);
    });
}

void OllamaProvider::requestPlan(const QJsonObject& projection, const QString& requestId,
                                 quint64 matchGeneration, quint64 sourceStateRevision,
                                 quint64 planningGeneration,
                                 std::function<void(OllamaResult)> callback) {
    OllamaPlanRequest request;
    request.projection = projection;
    request.requestId = requestId;
    request.matchGeneration = matchGeneration;
    request.sourceStateRevision = sourceStateRevision;
    request.planningGeneration = planningGeneration;
    request.validUntil = 0.0;
    request.mobileSeats = {};
    requestPlan(request, std::move(callback));
}

void OllamaProvider::requestPlan(const OllamaPlanRequest& request,
                                 std::function<void(OllamaResult)> callback) {
    if (m_config.deployment == QLatin1String("rules")) {
        callback(failure(QStringLiteral("rules_disabled"), m_config, request.requestId));
        return;
    }
    if (m_reply) {
        callback(failure(QStringLiteral("busy"), m_config, request.requestId));
        return;
    }
    m_activePlanRequest = request;
    m_activeMessages = messagesFor(request);
    m_planAttemptActive = true;
    m_planAttemptStartedAt = QDateTime::currentMSecsSinceEpoch();
    const auto finish = [this, callback](OllamaResult result) {
        finishAttempt(&result);
        callback(std::move(result));
    };
    if (!m_probeValid || m_probeGeneration != request.matchGeneration) {
        probe(request.matchGeneration, [this, request, finish](OllamaResult result) {
            if (!result.ok) {
                result.requestId = request.requestId;
                finish(std::move(result));
                return;
            }
            startPlanRequest(request, finish);
        });
        return;
    }
    if (m_resolvedModel.isEmpty()) {
        finish(attemptResult(m_probeFailureClass.isEmpty()
                                 ? QStringLiteral("model_missing") : m_probeFailureClass));
        return;
    }
    startPlanRequest(request, finish);
}

void OllamaProvider::startPlanRequest(const OllamaPlanRequest& request,
                                      std::function<void(OllamaResult)> callback) {
    if (m_planAttemptActive && remainingPlanTimeoutMs() <= 0) {
        callback(attemptResult(QStringLiteral("timeout")));
        return;
    }
    m_body.clear();
    const QString resolvedModel = m_resolvedModel;
    const QJsonObject payload{{QStringLiteral("model"), resolvedModel},
                              {QStringLiteral("stream"), false},
                              {QStringLiteral("think"), false},
                              {QStringLiteral("format"), schemaFor(request)},
                              {QStringLiteral("messages"), m_activeMessages}};
    m_reply = m_network ? m_network->post(requestFor(QStringLiteral("/api/chat")),
                                          QJsonDocument(payload).toJson(QJsonDocument::Compact)) : nullptr;
    if (!m_reply) {
        callback(attemptResult(QStringLiteral("unavailable")));
        return;
    }
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    QNetworkReply* const requestReply = m_reply;
    monitorReply(requestReply);
    connect(requestReply, &QNetworkReply::readyRead, this, [this, requestReply]() {
        if (m_reply == requestReply) appendBody(requestReply);
    });
    connect(requestReply, &QNetworkReply::finished, this,
            [this, requestReply, callback, request, resolvedModel, started]() mutable {
        if (m_reply != requestReply) return;
        QNetworkReply* reply = requestReply;
        m_reply = nullptr;
        stopTimers();
        OllamaResult result = attemptResult(QStringLiteral("invalid_json"));
        result.requestId = request.requestId;
        result.configuredModel = m_config.model;
        result.resolvedModel = resolvedModel;
        result.latencyMs = m_planAttemptStartedAt > 0
            ? QDateTime::currentMSecsSinceEpoch() - m_planAttemptStartedAt
            : QDateTime::currentMSecsSinceEpoch() - started;
        if (!m_body.isEmpty()) result.rawResponse = QString::fromUtf8(m_body);
        if (!m_abortFailure.isEmpty()) {
            result.failureClass = m_abortFailure;
        } else if (reply->error() == QNetworkReply::NoError
                   && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200) {
            QJsonParseError responseError;
            const QJsonDocument response = QJsonDocument::fromJson(m_body, &responseError);
            const QJsonValue message = response.object().value(QStringLiteral("message"));
            const QJsonValue content = message.toObject().value(QStringLiteral("content"));
            QJsonParseError planError;
            const QJsonDocument planDocument = QJsonDocument::fromJson(
                content.toString().toUtf8(), &planError);
            if (planDocument.isObject()) result.parsedPlan = planDocument.object();
            AiPlanV1 plan;
            QString parseError;
            if (responseError.error != QJsonParseError::NoError
                || (content.isString() && planError.error != QJsonParseError::NoError)) {
                result.failureClass = QStringLiteral("invalid_json");
            } else if (!response.isObject() || !message.isObject() || !content.isString()
                       || !planDocument.isObject()
                       || !AiPlanV1::fromJson(planDocument.object(), &plan, &parseError)) {
                result.failureClass = QStringLiteral("schema_invalid");
            } else {
                const QString semanticFailure = OllamaPlanningContract::validatePlan(
                    plan, request);
                if (!semanticFailure.isEmpty()) {
                    result.failureClass = semanticFailure;
                } else {
                    result.ok = true;
                    result.failureClass.clear();
                    result.plan = plan;
                }
            }
        } else if (reply->error() == QNetworkReply::OperationCanceledError) {
            result.failureClass = QStringLiteral("timeout");
        } else {
            result.failureClass = QStringLiteral("http_error");
        }
        reply->deleteLater();
        callback(std::move(result));
    });
}

std::optional<OllamaResult> OllamaProvider::cancel() {
    std::optional<OllamaResult> cancelled;
    if (m_planAttemptActive) cancelled = attemptResult(QStringLiteral("cancelled"));
    stopTimers();
    if (m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    if (cancelled.has_value()) finishAttempt(&cancelled.value());
    m_body.clear();
    m_abortFailure.clear();
    return cancelled;
}

}
