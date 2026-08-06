#include <gtest/gtest.h>

#include "OllamaProvider.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

using namespace gbr;

namespace {

class DynamicOllamaServer final : public QObject {
public:
    DynamicOllamaServer() {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket* socket = m_server.nextPendingConnection()) {
                m_buffers.insert(socket, {});
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    QByteArray& bytes = m_buffers[socket];
                    bytes += socket->readAll();
                    const qsizetype headerEnd = bytes.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    qsizetype contentLength = 0;
                    for (const QByteArray& line : bytes.first(headerEnd).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                        }
                    }
                    if (bytes.size() < headerEnd + 4 + contentLength) return;
                    const QList<QByteArray> requestLine = bytes.first(bytes.indexOf("\r\n")).split(' ');
                    m_paths.append(requestLine.value(1));
                    m_bodies.append(bytes.mid(headerEnd + 4, contentLength));
                    m_buffers.remove(socket);
                    const QByteArray body = m_responses.takeFirst();
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                                  + QByteArray::number(body.size())
                                  + "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    void enqueue(const QJsonObject& response) {
        m_responses.append(QJsonDocument(response).toJson(QJsonDocument::Compact));
    }
    const QList<QByteArray>& paths() const { return m_paths; }
    const QList<QByteArray>& bodies() const { return m_bodies; }

private:
    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QList<QByteArray> m_responses;
    QList<QByteArray> m_paths;
    QList<QByteArray> m_bodies;
};

OllamaPlanRequest planningRequest() {
    OllamaPlanRequest request;
    request.projection = QJsonObject{{QStringLiteral("stateRevision"), 11},
                                     {QStringLiteral("units"), QJsonArray{
                                         QJsonObject{{QStringLiteral("id"), QStringLiteral("blue_r1")}},
                                         QJsonObject{{QStringLiteral("id"), QStringLiteral("red_a1")}}}}};
    request.requestId = QStringLiteral("ai-plan:7:3");
    request.matchGeneration = 7;
    request.sourceStateRevision = 11;
    request.planningGeneration = 3;
    request.validUntil = 42.5;
    request.mapWidth = 20000.0;
    request.mapHeight = 15000.0;
    request.mobileSeats = {
        OllamaSeatConstraint{QStringLiteral("blue_attack_1"), QStringLiteral("blue_a1"),
                             QStringLiteral("attackuav"), 100.0, 200.0,
                             {QStringLiteral("red_a1"), QStringLiteral("red_cp")}},
        OllamaSeatConstraint{QStringLiteral("blue_recon_1"), QStringLiteral("blue_r1"),
                             QStringLiteral("reconuav"), 300.0, 400.0,
                             {QStringLiteral("red_a1")}}};
    return request;
}

QJsonObject validPlan() {
    return QJsonObject{{QStringLiteral("schemaVersion"), 1},
                       {QStringLiteral("requestId"), QStringLiteral("ai-plan:7:3")},
                       {QStringLiteral("matchGeneration"), 7},
                       {QStringLiteral("sourceStateRevision"), 11},
                       {QStringLiteral("planningGeneration"), 3},
                       {QStringLiteral("objectives"), QJsonArray{
                           QJsonObject{{QStringLiteral("action"), QStringLiteral("attack")},
                                       {QStringLiteral("priority"), 100},
                                       {QStringLiteral("seatId"), QStringLiteral("blue_attack_1")},
                                       {QStringLiteral("targetId"), QStringLiteral("red_a1")},
                                       {QStringLiteral("validUntil"), 42.5}},
                           QJsonObject{{QStringLiteral("action"), QStringLiteral("search")},
                                       {QStringLiteral("priority"), 90},
                                       {QStringLiteral("seatId"), QStringLiteral("blue_recon_1")},
                                       {QStringLiteral("region"), QJsonObject{
                                            {QStringLiteral("x"), 1000.0},
                                            {QStringLiteral("y"), 1000.0}}},
                                       {QStringLiteral("validUntil"), 42.5}}}}};
}

QJsonObject chatResponse() {
    const QString content = QString::fromUtf8(
        QJsonDocument(validPlan()).toJson(QJsonDocument::Compact));
    return QJsonObject{{QStringLiteral("message"),
                        QJsonObject{{QStringLiteral("content"), content}}}};
}

OllamaResult awaitRequest(OllamaProvider& provider, const OllamaPlanRequest& request) {
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    OllamaResult result;
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    provider.requestPlan(request, [&](OllamaResult value) {
        result = std::move(value);
        loop.quit();
    });
    guard.start(1000);
    loop.exec();
    return result;
}

OllamaConfig configFor(const QString& url, const QString& model) {
    OllamaConfig config;
    config.deployment = QStringLiteral("ollama");
    config.baseUrl = url;
    config.model = model;
    config.connectionTimeoutMs = 100;
    config.totalTimeoutMs = 500;
    return config;
}

TEST(OllamaDynamicContractTest, BuildsStructuredMessagesWithCompleteRequestMetadata) {
    const OllamaPlanRequest request = planningRequest();

    const QJsonArray messages = OllamaProvider::messagesFor(request);

    ASSERT_EQ(messages.size(), 2);
    EXPECT_EQ(messages.at(0).toObject().value(QStringLiteral("role")).toString(),
              QStringLiteral("system"));
    EXPECT_FALSE(messages.at(0).toObject().value(QStringLiteral("content")).toString().isEmpty());
    EXPECT_EQ(messages.at(1).toObject().value(QStringLiteral("role")).toString(),
              QStringLiteral("user"));
    const QJsonDocument user = QJsonDocument::fromJson(
        messages.at(1).toObject().value(QStringLiteral("content")).toString().toUtf8());
    ASSERT_TRUE(user.isObject());
    const QJsonObject metadata = user.object().value(QStringLiteral("metadata")).toObject();
    EXPECT_EQ(metadata.value(QStringLiteral("schemaVersion")).toInt(), 1);
    EXPECT_EQ(metadata.value(QStringLiteral("requestId")).toString(), request.requestId);
    EXPECT_EQ(metadata.value(QStringLiteral("matchGeneration")).toInteger(), 7);
    EXPECT_EQ(metadata.value(QStringLiteral("sourceStateRevision")).toInteger(), 11);
    EXPECT_EQ(metadata.value(QStringLiteral("planningGeneration")).toInteger(), 3);
    EXPECT_DOUBLE_EQ(metadata.value(QStringLiteral("validUntil")).toDouble(), 42.5);
    EXPECT_EQ(user.object().value(QStringLiteral("projectedState")).toObject(), request.projection);
    EXPECT_EQ(user.object().value(QStringLiteral("mobileSeats")).toArray().size(), 2);
}

TEST(OllamaDynamicContractTest, BindsSchemaToRequestSeatsTargetsAndMap) {
    const OllamaPlanRequest request = planningRequest();

    const QJsonObject schema = OllamaProvider::schemaFor(request);

    const QJsonObject properties = schema.value(QStringLiteral("properties")).toObject();
    EXPECT_EQ(properties.value(QStringLiteral("requestId")).toObject()
                  .value(QStringLiteral("const")).toString(), request.requestId);
    EXPECT_EQ(properties.value(QStringLiteral("matchGeneration")).toObject()
                  .value(QStringLiteral("const")).toInteger(), 7);
    EXPECT_EQ(properties.value(QStringLiteral("sourceStateRevision")).toObject()
                  .value(QStringLiteral("const")).toInteger(), 11);
    EXPECT_EQ(properties.value(QStringLiteral("planningGeneration")).toObject()
                  .value(QStringLiteral("const")).toInteger(), 3);
    const QJsonObject objectives = properties.value(QStringLiteral("objectives")).toObject();
    EXPECT_EQ(objectives.value(QStringLiteral("minItems")).toInt(), 2);
    EXPECT_EQ(objectives.value(QStringLiteral("maxItems")).toInt(), 2);
    const QJsonObject objectiveProperties = objectives.value(QStringLiteral("items")).toObject()
                                                .value(QStringLiteral("properties")).toObject();
    EXPECT_EQ(objectiveProperties.value(QStringLiteral("seatId")).toObject()
                  .value(QStringLiteral("enum")).toArray().size(), 2);
    EXPECT_EQ(objectiveProperties.value(QStringLiteral("targetId")).toObject()
                  .value(QStringLiteral("enum")).toArray().size(), 2);
    EXPECT_TRUE(objectiveProperties.value(QStringLiteral("action")).toObject()
                    .value(QStringLiteral("enum")).toArray().contains(QStringLiteral("attack")));
    const QJsonObject regionProperties = objectiveProperties.value(QStringLiteral("region"))
                                             .toObject().value(QStringLiteral("properties")).toObject();
    EXPECT_DOUBLE_EQ(regionProperties.value(QStringLiteral("x")).toObject()
                         .value(QStringLiteral("maximum")).toDouble(), request.mapWidth);
    EXPECT_DOUBLE_EQ(regionProperties.value(QStringLiteral("y")).toObject()
                         .value(QStringLiteral("maximum")).toDouble(), request.mapHeight);
}

TEST(OllamaDynamicContractTest, RejectsStructurallyValidPlanThatViolatesSeatConstraints) {
    DynamicOllamaServer server;
    ASSERT_TRUE(server.listen());
    server.enqueue(QJsonObject{{QStringLiteral("models"), QJsonArray{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("test-model")}}}}});
    QJsonObject plan = validPlan();
    QJsonArray objectives = plan.value(QStringLiteral("objectives")).toArray();
    QJsonObject reconObjective = objectives.at(1).toObject();
    reconObjective[QStringLiteral("action")] = QStringLiteral("attack");
    reconObjective[QStringLiteral("targetId")] = QStringLiteral("red_a1");
    reconObjective.remove(QStringLiteral("region"));
    objectives[1] = reconObjective;
    plan[QStringLiteral("objectives")] = objectives;
    server.enqueue(QJsonObject{{QStringLiteral("message"), QJsonObject{
        {QStringLiteral("content"), QString::fromUtf8(
             QJsonDocument(plan).toJson(QJsonDocument::Compact))}}}});
    QNetworkAccessManager network;
    OllamaProvider provider(configFor(server.url(), QStringLiteral("test-model")), &network);

    const OllamaResult result = awaitRequest(provider, planningRequest());

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.failureClass, QStringLiteral("semantic_action_not_allowed"));
    EXPECT_TRUE(result.parsedPlan.isObject());
}

TEST(OllamaDynamicModelTest, AutoPrefersQwen35TwoBWithCompletionCapability) {
    DynamicOllamaServer server;
    ASSERT_TRUE(server.listen());
    server.enqueue(QJsonObject{{QStringLiteral("models"), QJsonArray{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("tiny-embed" )},
                    {QStringLiteral("size"), 1},
                    {QStringLiteral("capabilities"), QJsonArray{QStringLiteral("embedding")}}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("legacy-small")},
                    {QStringLiteral("size"), 10}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("qwen3.5:2b")},
                    {QStringLiteral("size"), 100},
                    {QStringLiteral("capabilities"), QJsonArray{QStringLiteral("completion")}}}}}});
    server.enqueue(chatResponse());
    QNetworkAccessManager network;
    OllamaProvider provider(configFor(server.url(), QStringLiteral("auto")), &network);

    const OllamaResult result = awaitRequest(provider, planningRequest());

    ASSERT_TRUE(result.ok) << result.failureClass.toStdString();
    EXPECT_EQ(result.configuredModel, QStringLiteral("auto"));
    EXPECT_EQ(result.resolvedModel, QStringLiteral("qwen3.5:2b"));
    EXPECT_EQ(provider.configuredModel(), QStringLiteral("auto"));
    EXPECT_EQ(provider.resolvedModel(), QStringLiteral("qwen3.5:2b"));
    const QJsonObject payload = QJsonDocument::fromJson(server.bodies().at(1)).object();
    EXPECT_EQ(payload.value(QStringLiteral("model")).toString(), QStringLiteral("qwen3.5:2b"));
}

TEST(OllamaDynamicModelTest, AutoFallbackSortsEligibleModelsBySizeThenName) {
    DynamicOllamaServer server;
    ASSERT_TRUE(server.listen());
    server.enqueue(QJsonObject{{QStringLiteral("models"), QJsonArray{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("model-b")},
                    {QStringLiteral("size"), 10},
                    {QStringLiteral("capabilities"), QJsonArray{QStringLiteral("completion")}}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("model-a")},
                    {QStringLiteral("size"), 10}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("model-c")},
                    {QStringLiteral("size"), 5},
                    {QStringLiteral("capabilities"), QJsonArray{QStringLiteral("embedding")}}}}}});
    server.enqueue(chatResponse());
    QNetworkAccessManager network;
    OllamaProvider provider(configFor(server.url(), QStringLiteral("auto")), &network);

    const OllamaResult result = awaitRequest(provider, planningRequest());

    ASSERT_TRUE(result.ok) << result.failureClass.toStdString();
    EXPECT_EQ(result.resolvedModel, QStringLiteral("model-a"));
}

TEST(OllamaDynamicModelTest, ExplicitModelNeverSubstitutesIncompatibleInstall) {
    DynamicOllamaServer server;
    ASSERT_TRUE(server.listen());
    server.enqueue(QJsonObject{{QStringLiteral("models"), QJsonArray{
        QJsonObject{{QStringLiteral("name"), QStringLiteral("explicit-model")},
                    {QStringLiteral("size"), 10},
                    {QStringLiteral("capabilities"), QJsonArray{QStringLiteral("embedding")}}},
        QJsonObject{{QStringLiteral("name"), QStringLiteral("qwen3.5:2b")},
                    {QStringLiteral("size"), 20},
                    {QStringLiteral("capabilities"), QJsonArray{QStringLiteral("completion")}}}}}});
    QNetworkAccessManager network;
    OllamaProvider provider(configFor(server.url(), QStringLiteral("explicit-model")), &network);

    const OllamaResult result = awaitRequest(provider, planningRequest());

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.failureClass, QStringLiteral("model_incompatible"));
    EXPECT_TRUE(result.resolvedModel.isEmpty());
    EXPECT_EQ(server.paths(), (QList<QByteArray>{"/api/tags"}));
}

}
