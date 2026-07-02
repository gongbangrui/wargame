#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QFont>
#include <QFontDatabase>
#include "src/view/SimulationController.h"
#include "src/view/ScenarioEditor.h"

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
    app.setFont(pickChineseFont());

    gbr::SimulationController controller;
    gbr::ScenarioEditor editor;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("controller", &controller);
    engine.rootContext()->setContextProperty("editor", &editor);

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("index", "Main");

    return QGuiApplication::exec();
}
