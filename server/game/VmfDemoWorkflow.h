#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

#include <optional>

namespace gbr {

class VmfDemoWorkflow final {
public:
    static constexpr int SchemaVersion = 1;
    static constexpr const char* ProfileId = "vmf-demo-v2";
    static constexpr qsizetype TraceLimit = 200;
    static constexpr qsizetype ActionHistoryLimit = 4096;

    struct ActionSpec {
        QString action;
        QString seatType;
        int phase = 0;
        QString substep;
        QString title;
    };

    struct Result {
        bool ok = false;
        QString status = QStringLiteral("rejected");
        QString code;
        QString message;
        quint64 revision = 0;
        QJsonObject state;
    };

    VmfDemoWorkflow();

    static QList<ActionSpec> actionSpecs();
    static QStringList phaseIds();
    static QStringList phaseTitles();
    static bool validateTargetScript(const QJsonObject& script, QString* error = nullptr);

    Result applyAction(const QJsonObject& command, const QString& actorSeatType,
                       const QJsonObject& trace, double now);
    std::optional<Result> duplicateActionResult(const QJsonObject& command,
                                                const QString& actorSeatType) const;
    Result applyControl(const QString& action, const QJsonObject& payload, double now);

    const ActionSpec* currentAction() const;
    QString currentSeatType() const;
    QJsonObject stateProjection(bool includeTechnicalTrace) const;
    QJsonObject toJson() const;
    bool restore(const QJsonObject& object, QString* error = nullptr);
    void reset(double now = 0.0);

private:
    static bool validIdentifier(const QString& value);
    static QJsonObject normalizedScript(const QJsonObject& script);
    void rebuildTargetStateForCurrentPhase();
    void applyScriptForCurrentPhase();
    Result failure(const QString& code, const QString& message) const;
    Result success(const QString& status = QStringLiteral("accepted")) const;

    quint64 m_generation = 1;
    quint64 m_revision = 1;
    int m_actionIndex = 0;
    QString m_status = QStringLiteral("active");
    QJsonObject m_targetScript;
    int m_scriptCursor = 0;
    QJsonObject m_targetState;
    QJsonArray m_traces;
    QJsonArray m_actionHistory;
    QSet<QString> m_seenActionIds;
};

} // namespace gbr
