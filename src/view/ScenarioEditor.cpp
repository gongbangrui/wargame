#include "ScenarioEditor.h"

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QByteArray>

namespace gbr {

ScenarioEditor::ScenarioEditor(QObject* parent) : QObject(parent) {}

bool ScenarioEditor::saveJsonText(const QString& path, const QString& text) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const QByteArray data = text.toUtf8();
    return f.write(data) == data.size() && f.commit();
}

QString ScenarioEditor::loadJsonText(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

QString ScenarioEditor::scenarioDir() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/scenarios";
    QDir().mkpath(dir);
    return dir;
}

} // namespace gbr
