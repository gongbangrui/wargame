#include <QJsonArray>
#include "ScenarioEditor.h"

#include <QFile>
#include <QJsonDocument>

namespace gbr {

ScenarioEditor::ScenarioEditor(QObject* parent) : QObject(parent) {}

bool ScenarioEditor::saveJsonText(const QString& path, const QString& text) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(text.toUtf8());
    return true;
}

QString ScenarioEditor::loadJsonText(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QString ScenarioEditor::defaultScenarioJson() const {
    auto s = ScenarioIo::defaultScenario();
    return QString::fromUtf8(QJsonDocument(ScenarioIo::toJson(s)).toJson(QJsonDocument::Indented));
}

} // namespace gbr


