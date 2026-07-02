#pragma once

#include <QObject>
#include <QString>
#include "../core/Scenario.h"

namespace gbr {

class ScenarioEditor : public QObject {
    Q_OBJECT
public:
    explicit ScenarioEditor(QObject* parent = nullptr);

    Q_INVOKABLE bool saveJsonText(const QString& path, const QString& text);
    Q_INVOKABLE QString loadJsonText(const QString& path);
    Q_INVOKABLE QString defaultScenarioJson() const;
    Q_INVOKABLE bool saveFile(const QString& path, const QString& text) { return saveJsonText(path, text); }
    Q_INVOKABLE QString loadFile(const QString& path) { return loadJsonText(path); }
};

} // namespace gbr

