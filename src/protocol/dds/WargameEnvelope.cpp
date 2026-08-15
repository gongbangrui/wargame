#include "WargameEnvelope.h"

#include "protocol/Protocol.h"

#include <QDateTime>
#include <QJsonDocument>
#include <algorithm>

namespace gbr::Protocol::Dds {

namespace {

DecodeResult invalid(const QString& message) {
    return DecodeResult::failure(QStringLiteral("INVALID_DDS_ENVELOPE"), message);
}

bool validText(const QJsonValue& value, qsizetype maxLength, bool allowEmpty = false) {
    if (!value.isString()) return false;
    const QString text = value.toString();
    return (allowEmpty || !text.isEmpty()) && text.size() <= maxLength;
}

bool validUnsigned(const QJsonValue& value) {
    return value.isDouble() && value.toInteger() >= 0
        && value.toDouble() <= static_cast<double>(Protocol::MaxSafeJsonInteger);
}

qint64 effectiveNow(qint64 nowMs) {
    return nowMs >= 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
}

} // namespace

ChunkResult ChunkResult::accepted() {
    ChunkResult result;
    result.code = QStringLiteral("ACCEPTED");
    return result;
}

ChunkResult ChunkResult::completed(const QByteArray& payload) {
    ChunkResult result;
    result.complete = true;
    result.code = QStringLiteral("COMPLETE");
    result.payload = payload;
    return result;
}

ChunkResult ChunkResult::failure(const QString& code, const QString& message) {
    ChunkResult result;
    result.code = code;
    result.message = message;
    return result;
}

QList<EnvelopeChunk> splitPayload(const QByteArray& payload, const QString& transferId,
                                  qsizetype chunkBytes, QString* error) {
    if (error) error->clear();
    const QString id = transferId.trimmed();
    if (id.isEmpty() || id.size() > MaxEnvelopeMessageIdLength) {
        if (error) *error = QStringLiteral("分片传输 ID 无效");
        return {};
    }
    if (payload.isEmpty() || payload.size() > MaxReassemblyBytes) {
        if (error) *error = QStringLiteral("分片 payload 大小无效");
        return {};
    }
    if (chunkBytes <= 0 || chunkBytes > MaxChunkPayloadBytes) {
        if (error) *error = QStringLiteral("分片大小无效");
        return {};
    }
    const qsizetype count = (payload.size() + chunkBytes - 1) / chunkBytes;
    if (count <= 0 || count > MaxEnvelopeChunks) {
        if (error) *error = QStringLiteral("分片数量超出限制");
        return {};
    }
    QList<EnvelopeChunk> chunks;
    chunks.reserve(static_cast<int>(count));
    for (qsizetype offset = 0; offset < payload.size(); offset += chunkBytes) {
        EnvelopeChunk chunk;
        chunk.transferId = id;
        chunk.index = static_cast<quint32>(chunks.size());
        chunk.count = static_cast<quint32>(count);
        chunk.payload = payload.mid(offset, chunkBytes);
        chunks.append(std::move(chunk));
    }
    return chunks;
}

ChunkResult ChunkReassembler::add(const EnvelopeChunk& chunk, qint64 nowMs) {
    const qint64 now = effectiveNow(nowMs);
    expire(now);
    if (chunk.transferId.trimmed().isEmpty()
        || chunk.transferId.size() > MaxEnvelopeMessageIdLength
        || chunk.count == 0 || chunk.count > MaxEnvelopeChunks
        || chunk.index >= chunk.count || chunk.payload.isEmpty()
        || chunk.payload.size() > MaxChunkPayloadBytes) {
        return ChunkResult::failure(QStringLiteral("INVALID_CHUNK"), QStringLiteral("DDS 分片字段无效"));
    }
    Pending& pending = m_pending[chunk.transferId];
    if (pending.count == 0) {
        pending.count = chunk.count;
        pending.lastUpdatedMs = now;
        pending.chunks.resize(static_cast<int>(chunk.count));
    } else if (pending.count != chunk.count) {
        m_pending.remove(chunk.transferId);
        return ChunkResult::failure(QStringLiteral("CHUNK_COUNT_MISMATCH"), QStringLiteral("DDS 分片总数不一致"));
    }
    if (!pending.chunks.at(static_cast<int>(chunk.index)).isEmpty()) {
        return ChunkResult::accepted();
    }
    if (m_pendingBytes + chunk.payload.size() > MaxReassemblyBytes) {
        m_pending.remove(chunk.transferId);
        return ChunkResult::failure(QStringLiteral("REASSEMBLY_TOO_LARGE"), QStringLiteral("DDS 分片重组总大小超限"));
    }
    pending.chunks[static_cast<int>(chunk.index)] = chunk.payload;
    pending.bytes += chunk.payload.size();
    pending.lastUpdatedMs = now;
    m_pendingBytes += chunk.payload.size();
    if (pending.bytes == 0 || std::any_of(pending.chunks.cbegin(), pending.chunks.cend(),
                                         [](const QByteArray& value) { return value.isEmpty(); })) {
        return ChunkResult::accepted();
    }
    QByteArray result;
    result.reserve(static_cast<int>(pending.bytes));
    for (const QByteArray& value : pending.chunks) result.append(value);
    m_pendingBytes -= pending.bytes;
    m_pending.remove(chunk.transferId);
    return ChunkResult::completed(result);
}

void ChunkReassembler::expire(qint64 nowMs) {
    const qint64 now = effectiveNow(nowMs);
    for (auto it = m_pending.begin(); it != m_pending.end();) {
        if (now - it->lastUpdatedMs > MaxReassemblyAgeMs) {
            m_pendingBytes -= it->bytes;
            it = m_pending.erase(it);
        } else {
            ++it;
        }
    }
}

void ChunkReassembler::clear() {
    m_pending.clear();
    m_pendingBytes = 0;
}

DecodeResult DecodeResult::success(const WargameEnvelope& envelope) {
    DecodeResult result;
    result.valid = true;
    result.code = QStringLiteral("OK");
    result.envelope = envelope;
    return result;
}

DecodeResult DecodeResult::failure(const QString& code, const QString& message) {
    DecodeResult result;
    result.code = code;
    result.message = message;
    return result;
}

QJsonObject toJson(const WargameEnvelope& envelope) {
    return {
        {QStringLiteral("protocolVersion"), static_cast<qint64>(envelope.protocolVersion)},
        {QStringLiteral("schemaVersion"), static_cast<qint64>(envelope.schemaVersion)},
        {QStringLiteral("messageType"), envelope.messageType},
        {QStringLiteral("messageId"), envelope.messageId},
        {QStringLiteral("sequence"), static_cast<qint64>(envelope.sequence)},
        {QStringLiteral("stateRevision"), static_cast<qint64>(envelope.stateRevision)},
        {QStringLiteral("scenarioRevision"), static_cast<qint64>(envelope.scenarioRevision)},
        {QStringLiteral("serverTick"), static_cast<qint64>(envelope.serverTick)},
        {QStringLiteral("sentAt"), envelope.sentAt},
        {QStringLiteral("payload"), QString::fromUtf8(envelope.payload.toBase64())}
    };
}

DecodeResult fromJson(const QJsonObject& object) {
    if (!Protocol::isSupportedWireVersion(
            object.value(QStringLiteral("protocolVersion")).toInt(),
            object.value(QStringLiteral("schemaVersion")).toInt())) {
        return invalid(QStringLiteral("DDS 协议或模式版本不兼容"));
    }
    if (!validText(object.value(QStringLiteral("messageType")), MaxEnvelopeTypeLength)
        || !validText(object.value(QStringLiteral("messageId")), MaxEnvelopeMessageIdLength)
        || !validText(object.value(QStringLiteral("sentAt")), 64)) {
        return invalid(QStringLiteral("DDS envelope 文本字段无效"));
    }
    const QString sentAt = object.value(QStringLiteral("sentAt")).toString();
    if (!QDateTime::fromString(sentAt, Qt::ISODateWithMs).isValid()) {
        return invalid(QStringLiteral("DDS envelope 时间戳无效"));
    }
    for (const QString& field : {QStringLiteral("sequence"), QStringLiteral("stateRevision"),
                                 QStringLiteral("scenarioRevision"), QStringLiteral("serverTick")}) {
        if (!validUnsigned(object.value(field))) return invalid(QStringLiteral("DDS envelope 游标无效"));
    }
    if (object.value(QStringLiteral("sequence")).toInteger() == 0) {
        return invalid(QStringLiteral("DDS envelope sequence 必须从 1 开始"));
    }
    const QJsonValue payload = object.value(QStringLiteral("payload"));
    if (!payload.isString()) return invalid(QStringLiteral("DDS envelope payload 必须是 base64 字符串"));
    const QByteArray encodedPayload = payload.toString().toUtf8();
    const QByteArray bytes = QByteArray::fromBase64(encodedPayload);
    if (bytes.toBase64() != encodedPayload) {
        return invalid(QStringLiteral("DDS envelope payload base64 编码无效"));
    }
    if (bytes.isEmpty() || bytes.size() > MaxEnvelopePayloadBytes) {
        return invalid(QStringLiteral("DDS envelope payload 大小无效"));
    }
    WargameEnvelope envelope;
    envelope.protocolVersion = object.value(QStringLiteral("protocolVersion")).toInt();
    envelope.schemaVersion = object.value(QStringLiteral("schemaVersion")).toInt();
    envelope.messageType = object.value(QStringLiteral("messageType")).toString();
    envelope.messageId = object.value(QStringLiteral("messageId")).toString();
    envelope.sequence = static_cast<quint64>(object.value(QStringLiteral("sequence")).toInteger());
    envelope.stateRevision = static_cast<quint64>(object.value(QStringLiteral("stateRevision")).toInteger());
    envelope.scenarioRevision = static_cast<quint64>(object.value(QStringLiteral("scenarioRevision")).toInteger());
    envelope.serverTick = static_cast<quint64>(object.value(QStringLiteral("serverTick")).toInteger());
    envelope.sentAt = sentAt;
    envelope.payload = bytes;
    return DecodeResult::success(envelope);
}

QByteArray encode(const WargameEnvelope& envelope, QString* error) {
    if (error) error->clear();
    const DecodeResult validation = fromJson(toJson(envelope));
    if (!validation.valid) {
        if (error) *error = validation.message;
        return {};
    }
    return QJsonDocument(toJson(envelope)).toJson(QJsonDocument::Compact);
}

DecodeResult decode(const QByteArray& encoded) {
    if (encoded.isEmpty() || encoded.size() > MaxEnvelopePayloadBytes * 2) {
        return invalid(QStringLiteral("DDS envelope 大小无效"));
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(encoded, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return invalid(QStringLiteral("DDS envelope JSON 无效"));
    }
    return fromJson(document.object());
}

} // namespace gbr::Protocol::Dds
