#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QHash>
#include <QString>
#include <QtGlobal>

namespace gbr::Protocol::Dds {

// The DDS type is intentionally transport-neutral. The payload remains the
// existing validated JSON envelope until a later protocol version introduces
// a binary representation.
struct WargameEnvelope {
    quint32 protocolVersion = 0;
    quint32 schemaVersion = 0;
    QString messageType;
    QString messageId;
    quint64 sequence = 0;
    quint64 stateRevision = 0;
    quint64 scenarioRevision = 0;
    quint64 serverTick = 0;
    QString sentAt;
    QByteArray payload;
};

inline constexpr qsizetype MaxEnvelopePayloadBytes = 8 * 1024 * 1024;
inline constexpr qsizetype MaxEnvelopeMessageIdLength = 64;
inline constexpr qsizetype MaxEnvelopeTypeLength = 64;
inline constexpr qsizetype MaxChunkPayloadBytes = 64 * 1024;
inline constexpr int MaxEnvelopeChunks = 128;
inline constexpr qsizetype MaxReassemblyBytes = MaxEnvelopePayloadBytes;
inline constexpr qint64 MaxReassemblyAgeMs = 5000;

struct EnvelopeChunk {
    QString transferId;
    quint32 index = 0;
    quint32 count = 0;
    QByteArray payload;
};

struct ChunkResult {
    bool complete = false;
    QString code;
    QString message;
    QByteArray payload;

    static ChunkResult accepted();
    static ChunkResult completed(const QByteArray& payload);
    static ChunkResult failure(const QString& code, const QString& message);
};

QList<EnvelopeChunk> splitPayload(const QByteArray& payload,
                                  const QString& transferId,
                                  qsizetype chunkBytes = MaxChunkPayloadBytes,
                                  QString* error = nullptr);

class ChunkReassembler final {
public:
    ChunkResult add(const EnvelopeChunk& chunk, qint64 nowMs = -1);
    void expire(qint64 nowMs = -1);
    void clear();
    qsizetype pendingBytes() const { return m_pendingBytes; }

private:
    struct Pending {
        quint32 count = 0;
        qint64 lastUpdatedMs = 0;
        QList<QByteArray> chunks;
        qsizetype bytes = 0;
    };
    QHash<QString, Pending> m_pending;
    qsizetype m_pendingBytes = 0;
};

struct DecodeResult {
    bool valid = false;
    QString code;
    QString message;
    WargameEnvelope envelope;

    static DecodeResult success(const WargameEnvelope& envelope);
    static DecodeResult failure(const QString& code, const QString& message);
};

QByteArray encode(const WargameEnvelope& envelope, QString* error = nullptr);
DecodeResult decode(const QByteArray& encoded);
QJsonObject toJson(const WargameEnvelope& envelope);
DecodeResult fromJson(const QJsonObject& object);

} // namespace gbr::Protocol::Dds
