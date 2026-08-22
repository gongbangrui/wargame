#pragma once

#include "core/Scenario.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

namespace gbr {

class AuthoritativeRoom final {
public:
    struct Seat {
        QString seatId;
        QString seatType;
        QString side;
        qint64 userId = 0;
        qint64 humanUserId = 0;
        QString username;
        QString controllerType = QStringLiteral("human");
        QString controllerId;
        QString selectedTemplate;
        QString unitId;
        QString unitName;
        GeoPos position;
        bool connected = true;
        bool deployed = false;
        bool ready = false;
        quint64 revision = 0;
        bool pendingTransfer = false;
        bool redeployRequested = false;
    };

    struct Result {
        bool ok = false;
        QString code;
        quint64 revision = 0;
        bool duplicate = false;
        qint64 successorUserId = 0;
        bool forfeit = false;
        QString winner;
    };

    explicit AuthoritativeRoom(quint64 rngSeed = 1);

    Result setMode(const QString& mode);
    bool setSeatLimits(const QHash<QString, int>& limits, QString* error = nullptr);
    Result syncAiRoster();
    Result installPlaceholdersForMissing();
    Result removePlaceholders();
    Result deployAiSeats(double mapWidth, double mapHeight, quint64 matchGeneration);

    static QHash<QString, ScenarioUnit> defaultTemplateCatalog();
    bool setTemplateCatalog(const QHash<QString, ScenarioUnit>& catalog, QString* error = nullptr);

    Result claimSeat(qint64 userId, const QString& username, const QString& seatId,
                     const QString& templateId);
    Result requestTransfer(qint64 userId, const QString& targetSeatId,
                           const QString& templateId);
    Result cancelTransfer(qint64 userId, quint64 requestedRevision);
    Result rejectTransfer(qint64 commanderUserId, qint64 userId,
                           quint64 requestedRevision);
    Result approveTransfer(qint64 commanderUserId, qint64 userId, quint64 requestedRevision);
    Result leave(qint64 userId, qint64 successorUserId = 0);
    Result leaveRoom(qint64 userId);
    Result disconnect(qint64 userId);
    Result deploy(qint64 commanderUserId, const QString& seatId, const GeoPos& position);
    Result deployInitial(const QString& seatId, const GeoPos& position);
    Result requestRedeploy(qint64 userId);
    Result redeploy(qint64 commanderUserId, const QString& seatId = {});
    Result setUnitName(qint64 userId, const QString& unitName);
    Result setReady(qint64 userId, bool ready);
    void clearReadiness();
    Result start();
    Result pause();
    Result finish(const QString& winner);
    Result applyOperation(const QString& operationId, const QString& action,
                          quint64 requestedRevision);
    bool removeUser(qint64 userId);

    QJsonObject toJson() const;
    bool restore(const QJsonObject& object, QString* error = nullptr);
    QJsonArray seatProjection() const;
    QJsonObject pendingTransfer(qint64 userId) const;
    QJsonObject readiness() const;
    QJsonArray runtimeUnits() const;

    const QHash<QString, Seat>& seats() const { return m_seats; }
    Seat seat(const QString& seatId) const { return m_seats.value(seatId); }
    bool hasSeat(const QString& seatId) const { return m_seats.contains(seatId); }
    bool hasUser(qint64 userId) const;
    QString phase() const { return m_phase; }
    QString roomStatus() const;
    QString winner() const { return m_winner; }
    quint64 revision() const { return m_revision; }
    quint64 rngState() const { return m_rngState; }
    QString mode() const { return m_mode; }
    bool isAiSeat(const QString& seatId) const;

private:
    struct Transfer {
        qint64 userId = 0;
        QString targetSeatId;
        QString templateId;
        quint64 revision = 0;
    };

    Result failure(const QString& code) const;
    Result success();
    Result invalidateTransfer(qint64 userId, const QString& code);
    QString seatForUser(qint64 userId) const;
    bool validSeatTemplate(const QString& seatId, const QString& templateId,
                           QString* side = nullptr, QString* type = nullptr) const;
    QString allocateUnitId(const QString& side, const QString& templateId);
    qint64 chooseSuccessor(const QString& side);
    Result promote(qint64 commanderUserId, qint64 successorUserId);
    Result installAiSeat(const QString& seatId, const QString& templateId);
    bool validAiPosition(const GeoPos& position, double mapWidth, double mapHeight,
                         const QHash<QString, GeoPos>& placements) const;
    static quint64 deterministicSeed(const QString& value, quint64 generation);
    void clearDeployment();
    void clearDeployment(Seat& seat);

    QHash<QString, ScenarioUnit> m_templates;
    // Optional for standalone room tests and legacy callers. The game server
    // always supplies this map from the complete initial scenario.
    QHash<QString, int> m_seatLimits;
    QHash<QString, Seat> m_seats;
    QHash<qint64, Transfer> m_transfers;
    QHash<QString, Result> m_operations;
    QStringList m_operationOrder;
    QSet<qint64> m_departedUsers;
    QString m_phase = QStringLiteral("preparing");
    QString m_mode = QStringLiteral("pvp");
    QString m_winner;
    quint64 m_revision = 1;
    quint64 m_nextUnitSequence = 1;
    quint64 m_rngState = 1;
};

}
