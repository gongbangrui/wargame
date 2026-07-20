#pragma once

#include <QJsonObject>
#include <QString>

namespace gbr {

class ClientStateStore final {
public:
    enum class Disposition {
        Ignored,
        Accepted,
        SnapshotApplied,
        DeltaApplied,
        ResyncRequired,
        Fatal
    };

    struct Result {
        Disposition disposition = Disposition::Ignored;
        QString type;
        QJsonObject payload;
        QString code;
        QString message;
    };

    void reset();
    void beginConnection();
    Result applyEnvelope(const QJsonObject& envelope);

    quint64 lastSequence() const { return m_lastSequence; }
    qint64 stateRevision() const;
    const QJsonObject& snapshot() const { return m_snapshot; }
    bool waitingForSnapshot() const { return m_waitingForSnapshot; }
    bool waitingForResync() const { return m_waitingForResync; }

private:
    quint64 m_lastSequence = 0;
    QJsonObject m_snapshot;
    bool m_waitingForSnapshot = true;
    bool m_waitingForResync = false;
};

} // namespace gbr
