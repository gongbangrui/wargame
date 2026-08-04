#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QFont>
#include <QFontDatabase>
#include <QDebug>
#include <QCommandLineParser>
#include <QVariantMap>
#include "src/view/SimulationController.h"
#include "src/view/ScenarioEditor.h"
#include "src/view/MapTileRenderer.h"
#include "src/core/TileImageProvider.h"

static QFont pickChineseFont()
{
    const QStringList candidates = {
        QStringLiteral("Microsoft YaHei UI"),
        QStringLiteral("Microsoft YaHei"),
        QStringLiteral("SimHei"),
        QStringLiteral("SimSun"),
        QStringLiteral("Source Han Sans CN"),
        QStringLiteral("Noto Sans CJK SC"),
    };
    QFontDatabase db;
    const auto families = db.families();
    for (const auto& name : candidates) {
        if (families.contains(name, Qt::CaseInsensitive)) {
            QFont f(name);
            f.setPointSize(10);
            return f;
        }
    }
    return QFont();
}

int main(int argc, char *argv[])
{
    qputenv("QT_LOGGING_RULES", "qt.text.codecs.warning=false");

    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("wargame-client"));
    app.setApplicationVersion(QStringLiteral("%1+%2")
                                  .arg(QStringLiteral(WARGAME_VERSION),
                                       QStringLiteral(WARGAME_SOURCE_DIGEST)));
    app.setProperty("sourceDigest", QStringLiteral(WARGAME_SOURCE_DIGEST));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);
    app.setFont(pickChineseFont());

    gbr::SimulationController controller;
    gbr::ScenarioEditor editor;

    qmlRegisterType<gbr::MapTileRenderer>("Wargame", 1, 0, "MapTileRenderer");
    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("tiles"), new gbr::TileImageProvider());
    engine.setInitialProperties({
        {QStringLiteral("simulationController"), QVariant::fromValue(&controller)},
        {QStringLiteral("scenarioEditor"), QVariant::fromValue(&editor)},
    });

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        [](const QUrl& url) {
            qCritical() << "QML 界面创建失败:" << url;
            QCoreApplication::exit(-1);
        },
        Qt::QueuedConnection);
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                     &app, [](const QList<QQmlError>& warnings) {
                         for (const QQmlError& warning : warnings) qWarning() << warning;
                     });
    qInfo() << "开始加载 QML" << WARGAME_VERSION << WARGAME_SOURCE_DIGEST;
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/index/Main.qml")));
    qInfo() << "QML 加载完成，根对象数量:" << engine.rootObjects().size();
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "QML 根对象为空，模块加载失败";
        return -1;
    }

    return QGuiApplication::exec();
}
