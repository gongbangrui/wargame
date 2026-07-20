#pragma once

#include <QObject>
#include <QString>

namespace gbr {

class ScenarioEditor : public QObject {
    Q_OBJECT
public:
    explicit ScenarioEditor(QObject* parent = nullptr);

    Q_INVOKABLE bool saveJsonText(const QString& path, const QString& text);
    Q_INVOKABLE QString loadJsonText(const QString& path);
    Q_INVOKABLE QString scenarioDir() const;
};

} // namespace gbr

