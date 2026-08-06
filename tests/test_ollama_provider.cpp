#include <gtest/gtest.h>

#include "OllamaProvider.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>

using namespace gbr;

namespace {

struct HttpResponse {
    int status = 200;
    QByteArray body;
    bool hold = false;
    QByteArray extraHeaders;
    int delayMs = 0;
};

class FakeOllamaServer final : public QObject {
public:
    FakeOllamaServer() {
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
                    const HttpResponse response = m_responses.takeFirst();
                    if (response.hold) return;
                    const auto sendResponse = [socket, response]() {
                        const QByteArray reason = response.status == 200 ? "OK" : "Error";
                        socket->write("HTTP/1.1 " + QByteArray::number(response.status) + " " + reason
                                      + "\r\nContent-Type: application/json\r\nContent-Length: "
                                      + QByteArray::number(response.body.size())
                                      + "\r\nConnection: close\r\n" + response.extraHeaders
                                      + "\r\n" + response.body);
                        socket->disconnectFromHost();
                    };
                    if (response.delayMs > 0) {
                        QTimer::singleShot(response.delayMs, socket, sendResponse);
                    } else {
                        sendResponse();
                    }
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    void enqueue(const HttpResponse& response) { m_responses.append(response); }
    const QList<QByteArray>& paths() const { return m_paths; }
    const QList<QByteArray>& bodies() const { return m_bodies; }

private:
    QTcpServer m_server;
    QList<HttpResponse> m_responses;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QList<QByteArray> m_paths;
    QList<QByteArray> m_bodies;
};

OllamaConfig configFor(const QString& url) {
    OllamaConfig config;
    config.deployment = QStringLiteral("ollama");
    config.baseUrl = url;
    config.model = QStringLiteral("test-model");
    config.connectionTimeoutMs = 40;
    config.totalTimeoutMs = 120;
    return config;
}

QByteArray tags(bool present = true) {
    const QJsonArray models = present
        ? QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("test-model")}}}
        : QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("other-model")}}};
    return QJsonDocument(QJsonObject{{QStringLiteral("models"), models}}).toJson(QJsonDocument::Compact);
}

QJsonObject validPlan(quint64 match = 7, quint64 planning = 3,
                      const QString& request = QStringLiteral("ai-plan:7:3")) {
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("requestId"), request},
            {QStringLiteral("matchGeneration"), static_cast<qint64>(match)},
            {QStringLiteral("sourceStateRevision"), 11},
            {QStringLiteral("planningGeneration"), static_cast<qint64>(planning)},
            {QStringLiteral("objectives"), QJsonArray{}}};
}

QByteArray chat(const QJsonObject& plan) {
    const QString content = QString::fromUtf8(QJsonDocument(plan).toJson(QJsonDocument::Compact));
    return QJsonDocument(QJsonObject{{QStringLiteral("message"),
                                      QJsonObject{{QStringLiteral("content"), content}}}})
        .toJson(QJsonDocument::Compact);
}

bool waitUntil(const std::function<bool()>& condition, int timeoutMs = 1000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return condition();
}

OllamaResult awaitPlan(OllamaProvider& provider, int timeoutMs = 1000) {
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    OllamaResult result;
    bool called = false;
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    provider.requestPlan({}, QStringLiteral("ai-plan:7:3"), 7, 11, 3,
                         [&](OllamaResult value) { result = value; called = true; loop.quit(); });
    guard.start(timeoutMs);
    loop.exec();
    EXPECT_TRUE(called);
    return result;
}

class InvalidUrlTest : public testing::TestWithParam<const char*> {};

TEST_P(InvalidUrlTest, RejectsUnsafeBoundaryComponents) {
    EXPECT_FALSE(OllamaProvider::validateBaseUrl(QString::fromLatin1(GetParam())));
}

INSTANTIATE_TEST_SUITE_P(UserInfoQueryFragmentAndScheme, InvalidUrlTest,
                         testing::Values("http://user@127.0.0.1:11434",
                                         "http://127.0.0.1:11434?x=1",
                                         "http://127.0.0.1:11434/#fragment",
                                         "file:///tmp/ollama.sock"));

TEST(OllamaProviderTest, UsesExactProductionBoundsByDefault) {
    const OllamaConfig config;
    EXPECT_EQ(config.connectionTimeoutMs, DefaultOllamaConnectionTimeoutMs);
    EXPECT_EQ(config.totalTimeoutMs, DefaultOllamaRequestTimeoutMs);
    EXPECT_EQ(config.maxResponseBytes, 64 * 1024);
}

TEST(OllamaProviderTest, SendsBoundedNonStreamingSchemaRequestAndAcceptsExactPlan) {
    FakeOllamaServer server;
    ASSERT_TRUE(server.listen());
    server.enqueue({200, tags()});
    server.enqueue({200, chat(validPlan())});
    QNetworkAccessManager network;
    OllamaProvider provider(configFor(server.url()), &network);

    const OllamaResult result = awaitPlan(provider);

    ASSERT_TRUE(result.ok) << result.failureClass.toStdString();
    EXPECT_TRUE(result.failureClass.isEmpty());
    ASSERT_EQ(server.paths(), (QList<QByteArray>{"/api/tags", "/api/chat"}));
    const QJsonObject payload = QJsonDocument::fromJson(server.bodies().at(1)).object();
    EXPECT_EQ(payload.value(QStringLiteral("model")).toString(), QStringLiteral("test-model"));
    EXPECT_FALSE(payload.value(QStringLiteral("stream")).toBool(true));
    EXPECT_FALSE(payload.value(QStringLiteral("think")).toBool(true));
    const QJsonObject format = payload.value(QStringLiteral("format")).toObject();
    EXPECT_FALSE(format.value(QStringLiteral("additionalProperties")).toBool(true));
    EXPECT_TRUE(format.value(QStringLiteral("properties")).toObject()
                    .contains(QStringLiteral("planningGeneration")));
}

TEST(OllamaProviderTest, SharesOneTimeoutBudgetAcrossProbeAndChat) {
    FakeOllamaServer server;
    ASSERT_TRUE(server.listen());
    server.enqueue({200, tags(), false, {}, 120});
    server.enqueue({200, {}, true});
    QNetworkAccessManager network;
    OllamaConfig config = configFor(server.url());
    config.totalTimeoutMs = 250;
    OllamaProvider provider(config, &network);

    QElapsedTimer elapsed;
    elapsed.start();
    const OllamaResult result = awaitPlan(provider, 1000);

    EXPECT_EQ(result.failureClass, QStringLiteral("timeout"));
    EXPECT_EQ(server.paths(), (QList<QByteArray>{"/api/tags", "/api/chat"}));
    EXPECT_GE(elapsed.elapsed(), 200);
    EXPECT_LT(elapsed.elapsed(), 325);
    EXPECT_GE(result.latencyMs, 200);
    EXPECT_LT(result.latencyMs, 325);
}

TEST(OllamaProviderTest, MissingModelIsCachedOnlyForOneMatch) {
    FakeOllamaServer server;
    ASSERT_TRUE(server.listen());
    server.enqueue({200, tags(false)});
    server.enqueue({200, tags()});
    server.enqueue({200, chat(validPlan(8, 3, QStringLiteral("ai-plan:8:3")))});
    QNetworkAccessManager network;
    OllamaProvider provider(configFor(server.url()), &network);
    OllamaResult first;
    provider.requestPlan({}, QStringLiteral("ai-plan:7:3"), 7, 11, 3,
                         [&](OllamaResult value) { first = value; });
    ASSERT_TRUE(waitUntil([&provider]() { return !provider.inFlight(); }));
    EXPECT_EQ(first.failureClass, QStringLiteral("model_missing"));
    provider.requestPlan({}, QStringLiteral("ai-plan:7:4"), 7, 11, 4, [](OllamaResult) {});
    EXPECT_EQ(server.paths().size(), 1);

    QEventLoop loop;
    OllamaResult next;
    provider.requestPlan({}, QStringLiteral("ai-plan:8:3"), 8, 11, 3,
                         [&](OllamaResult value) { next = value; loop.quit(); });
    QTimer::singleShot(1000, &loop, &QEventLoop::quit);
    loop.exec();
    EXPECT_TRUE(next.ok);
    EXPECT_EQ(server.paths().count("/api/tags"), 2);
}

TEST(OllamaProviderTest, CategorizesHttpJsonSchemaOversizeAndStaleFailures) {
    struct Case { HttpResponse response; QString expected; int cap = 64 * 1024; };
    const QList<Case> cases{
        {{503, "{}"}, QStringLiteral("http_error")},
        {{302, "{}", false, "Location: /redirected\r\n"}, QStringLiteral("http_error")},
        {{200, "not-json"}, QStringLiteral("invalid_json")},
        {{200, "{}"}, QStringLiteral("schema_invalid")},
        {{200, chat(QJsonObject{})}, QStringLiteral("schema_invalid")},
        {{200, QByteArray(2048, 'x')}, QStringLiteral("response_too_large"), 1024},
        {{200, chat(validPlan(9))}, QStringLiteral("stale_response")},
        {{200, chat(validPlan(7, 4))}, QStringLiteral("stale_response")},
        {{200, chat(validPlan(7, 3, QStringLiteral("wrong")))}, QStringLiteral("stale_response")}};
    for (const Case& testCase : cases) {
        FakeOllamaServer server;
        ASSERT_TRUE(server.listen());
        server.enqueue({200, tags()});
        server.enqueue(testCase.response);
        QNetworkAccessManager network;
        OllamaConfig config = configFor(server.url());
        config.maxResponseBytes = testCase.cap;
        OllamaProvider provider(config, &network);
        EXPECT_EQ(awaitPlan(provider).failureClass, testCase.expected);
        EXPECT_FALSE(server.paths().contains("/redirected"));
    }
}

TEST(OllamaProviderTest, CategorizesRefusalAndTimeout) {
    QTcpServer reservation;
    ASSERT_TRUE(reservation.listen(QHostAddress::LocalHost, 0));
    const QString refusedUrl = QStringLiteral("http://127.0.0.1:%1").arg(reservation.serverPort());
    reservation.close();
    QNetworkAccessManager refusedNetwork;
    OllamaProvider refused(configFor(refusedUrl), &refusedNetwork);
    EXPECT_EQ(awaitPlan(refused).failureClass, QStringLiteral("unavailable"));

    FakeOllamaServer server;
    ASSERT_TRUE(server.listen());
    server.enqueue({200, {}, true});
    QNetworkAccessManager timeoutNetwork;
    OllamaProvider timeout(configFor(server.url()), &timeoutNetwork);
    EXPECT_EQ(awaitPlan(timeout).failureClass, QStringLiteral("timeout"));
}

TEST(OllamaProviderTest, CancelSuppressesStaleCallbackAndAllowsResume) {
    FakeOllamaServer server;
    ASSERT_TRUE(server.listen());
    server.enqueue({200, {}, true});
    server.enqueue({200, tags()});
    server.enqueue({200, chat(validPlan())});
    QNetworkAccessManager network;
    OllamaProvider provider(configFor(server.url()), &network);
    bool staleCalled = false;
    provider.requestPlan({}, QStringLiteral("ai-plan:7:2"), 7, 11, 2,
                         [&](OllamaResult) { staleCalled = true; });
    ASSERT_TRUE(waitUntil([&server]() { return server.paths().size() == 1; }));
    provider.cancel();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    EXPECT_FALSE(staleCalled);
    const OllamaResult resumed = awaitPlan(provider);
    EXPECT_TRUE(resumed.ok) << resumed.failureClass.toStdString();
}

TEST(OllamaProviderTest, ReconfigureCancelsOldProbeAndUsesNewEndpoint) {
    FakeOllamaServer oldServer;
    FakeOllamaServer newServer;
    ASSERT_TRUE(oldServer.listen());
    ASSERT_TRUE(newServer.listen());
    oldServer.enqueue({200, tags(), true});
    newServer.enqueue({200, tags()});
    QNetworkAccessManager network;
    OllamaProvider provider(configFor(oldServer.url()), &network);
    bool staleCalled = false;
    provider.probe(7, [&](OllamaResult) { staleCalled = true; });
    ASSERT_TRUE(waitUntil([&oldServer]() { return oldServer.paths().size() == 1; }));

    provider.reconfigure(configFor(newServer.url()));
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    OllamaResult result;
    bool called = false;
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    provider.probe(7, [&](OllamaResult value) {
        result = value;
        called = true;
        loop.quit();
    });
    guard.start(1000);
    loop.exec();

    EXPECT_TRUE(called);
    EXPECT_TRUE(result.ok) << result.failureClass.toStdString();
    EXPECT_FALSE(staleCalled);
    EXPECT_EQ(oldServer.paths().size(), 1);
    EXPECT_EQ(newServer.paths(), (QList<QByteArray>{"/api/tags"}));
}

TEST(OllamaProviderTest, DestructionCancelsWithoutCallingBack) {
    FakeOllamaServer server;
    ASSERT_TRUE(server.listen());
    server.enqueue({200, {}, true});
    QNetworkAccessManager network;
    bool called = false;
    auto provider = std::make_unique<OllamaProvider>(configFor(server.url()), &network);
    provider->requestPlan({}, QStringLiteral("ai-plan:7:3"), 7, 11, 3,
                          [&](OllamaResult) { called = true; });
    ASSERT_TRUE(waitUntil([&server]() { return server.paths().size() == 1; }));

    provider.reset();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    EXPECT_FALSE(called);
}

}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
