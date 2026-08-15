#pragma once

#include "protocol/IntelProtocol.h"

#include <QHash>
#include <QDateTime>
#include <QJsonObject>
#include <QString>

namespace gbr {

// Server-owned, seat-scoped intelligence state.  The ledger deliberately
// stores protocol values rather than engine pointers so checkpoints and event
// replay cannot accidentally expose or retain transient QObject state.
class IntelLedger final {
public:
    struct Config {
        double staleAfterSec = 10.0;
        double archiveAfterSec = 120.0;
    };

    struct Result {
        bool ok = false;
        // `ok` reports command validation/execution.  `changed` is reserved
        // for callers that need to decide whether to advance a projection or
        // write a checkpoint; repeated successful observations may be no-ops.
        bool changed = false;
        QString code;
        QString intelId;
        QStringList affectedSeats;
    };

    IntelLedger() = default;

    void clear();
    void setConfig(const Config& config);
    Config config() const { return m_config; }
    void ensureSeat(const QString& seatId);
    void removeSeat(const QString& seatId);

    const Protocol::IntelState& state(const QString& seatId) const;
    Protocol::IntelState projectedState(const QString& seatId,
                                        const QStringList& shareTargets = {}) const;
    QJsonArray history(const QString& seatId) const;

    Result observeSensor(const QString& seatId, const QString& targetId,
                         const QJsonObject& knownAttributes, const QJsonObject& position,
                         const QString& sourceUnitId, const QDateTime& observedAt);
    Result createManualReport(const QString& seatId, const QString& category,
                              const QString& title, const QJsonObject& position,
                              const QString& note, const QDateTime& receivedAt);
    Result share(const QString& senderSeatId, const QString& recipientSeatId,
                 const QString& senderSide, const QString& recipientSide, bool reachable,
                 const QString& intelId, const QString& note, const QDateTime& receivedAt);
    int advance(const QDateTime& now);

    Protocol::IntelHistoryPage historyPage(const QString& seatId,
                                            const Protocol::IntelHistoryQuery& query) const;
    QJsonObject toJson() const;
    bool restore(const QJsonObject& object, QString* error = nullptr);

private:
    struct SeatLedger {
        Protocol::IntelState state;
        QList<Protocol::IntelHistoryEntry> history;
        quint64 nextIntel = 1;
        quint64 nextHistory = 1;
    };

    static QString timestamp(const QDateTime& value);
    static QJsonObject positionObject(const QJsonObject& position);
    static QString contactKey(const QString& seatId, const QString& targetId);
    static bool validPosition(const QJsonObject& position);
    void record(SeatLedger& ledger, Protocol::IntelHistoryEntry entry);
    void bump(SeatLedger& ledger);
    QHash<QString, SeatLedger> m_seats;
    Config m_config;
};

} // namespace gbr
