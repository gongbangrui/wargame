#include "IntelLedger.h"

#include <QDateTime>
#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gbr {

namespace {

Protocol::IntelContact* findContact(Protocol::IntelState& state, const QString& intelId) {
    for (Protocol::IntelContact& contact : state.records) {
        if (contact.intelId == intelId) return &contact;
    }
    return nullptr;
}

const Protocol::IntelContact* findContact(const Protocol::IntelState& state,
                                          const QString& intelId) {
    for (const Protocol::IntelContact& contact : state.records) {
        if (contact.intelId == intelId) return &contact;
    }
    return nullptr;
}

Protocol::IntelContact* findSensorContactByTarget(Protocol::IntelState& state,
                                                  const QString& targetId) {
    if (targetId.isEmpty()) return nullptr;
    for (Protocol::IntelContact& contact : state.records) {
        if (contact.type == QLatin1String("sensorContact")
            && contact.targetId == targetId) {
            return &contact;
        }
    }
    return nullptr;
}

QString freshnessForAge(double age, const IntelLedger::Config& config) {
    if (age >= config.archiveAfterSec) return QStringLiteral("archived");
    if (age >= config.staleAfterSec) return QStringLiteral("stale");
    return QStringLiteral("live");
}

qint64 historySequence(const QString& historyId) {
    if (!historyId.startsWith(QLatin1String("ih_"))) return -1;
    bool ok = false;
    const qint64 sequence = historyId.mid(3).toLongLong(&ok);
    return ok && sequence > 0 ? sequence : -1;
}

QDateTime parseTimestamp(const QString& value) {
    QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid()) parsed = QDateTime::fromString(value, Qt::ISODate);
    return parsed;
}

} // namespace

QString IntelLedger::timestamp(const QDateTime& value) {
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QJsonObject IntelLedger::positionObject(const QJsonObject& position) {
    QJsonObject result{{QStringLiteral("x"), position.value(QStringLiteral("x"))},
                       {QStringLiteral("y"), position.value(QStringLiteral("y"))}};
    if (position.contains(QStringLiteral("alt"))) {
        result[QStringLiteral("alt")] = position.value(QStringLiteral("alt"));
    }
    return result;
}

QString IntelLedger::contactKey(const QString& seatId, const QString& targetId) {
    return seatId + QLatin1Char('\0') + targetId;
}

bool IntelLedger::validPosition(const QJsonObject& position) {
    const double x = position.value(QStringLiteral("x")).toDouble(qQNaN());
    const double y = position.value(QStringLiteral("y")).toDouble(qQNaN());
    return std::isfinite(x) && std::isfinite(y) && x >= 0.0 && y >= 0.0;
}

double IntelLedger::positionDistance(const QJsonObject& left, const QJsonObject& right) {
    if (!validPosition(left) || !validPosition(right)) {
        return std::numeric_limits<double>::infinity();
    }
    return std::hypot(left.value(QStringLiteral("x")).toDouble()
                          - right.value(QStringLiteral("x")).toDouble(),
                      left.value(QStringLiteral("y")).toDouble()
                          - right.value(QStringLiteral("y")).toDouble());
}

const Protocol::IntelHistoryEntry* IntelLedger::latestHistoryEntry(
    const SeatLedger& ledger, const QString& intelId) {
    for (auto it = ledger.history.crbegin(); it != ledger.history.crend(); ++it) {
        if (it->intelId == intelId) return &*it;
    }
    return nullptr;
}

void IntelLedger::clear() { m_seats.clear(); }

void IntelLedger::setConfig(const Config& config) {
    if (std::isfinite(config.staleAfterSec) && config.staleAfterSec > 0.0
        && std::isfinite(config.archiveAfterSec)
        && config.archiveAfterSec > config.staleAfterSec) {
        m_config = config;
    }
}

void IntelLedger::ensureSeat(const QString& seatId) {
    if (!seatId.isEmpty() && !m_seats.contains(seatId)) m_seats.insert(seatId, SeatLedger{});
}

void IntelLedger::removeSeat(const QString& seatId) { m_seats.remove(seatId); }

const Protocol::IntelState& IntelLedger::state(const QString& seatId) const {
    static const Protocol::IntelState empty;
    const auto it = m_seats.constFind(seatId);
    return it == m_seats.cend() ? empty : it->state;
}

Protocol::IntelState IntelLedger::projectedState(const QString& seatId,
                                                 const QStringList& shareTargets) const {
    Protocol::IntelState projected = state(seatId);
    projected.shareTargets = shareTargets;
    return projected;
}

QJsonArray IntelLedger::history(const QString& seatId) const {
    QJsonArray result;
    const auto it = m_seats.constFind(seatId);
    if (it == m_seats.cend()) return result;
    for (const auto& entry : it->history) result.append(Protocol::toJson(entry));
    return result;
}

void IntelLedger::bump(SeatLedger& ledger) { ++ledger.state.revision; }

void IntelLedger::record(SeatLedger& ledger, Protocol::IntelHistoryEntry entry) {
    entry.historyId = entry.historyId.isEmpty()
        ? QStringLiteral("ih_%1").arg(ledger.nextHistory++) : entry.historyId;
    ledger.history.append(std::move(entry));
    while (ledger.history.size() > HistoryLimit) ledger.history.removeFirst();
}

IntelLedger::Result IntelLedger::observeSensor(const QString& seatId, const QString& targetId,
                                               const QJsonObject& knownAttributes,
                                               const QJsonObject& position,
                                               const QString& sourceUnitId,
                                               const QDateTime& observedAt) {
    Result result;
    if (seatId.isEmpty() || targetId.isEmpty() || sourceUnitId.isEmpty()) {
        result.code = QStringLiteral("INVALID_INTEL");
        return result;
    }
    if (!validPosition(position)) {
        result.code = QStringLiteral("MAP_BOUNDS");
        return result;
    }
    ensureSeat(seatId);
    SeatLedger& ledger = m_seats[seatId];
    const QString id = QStringLiteral("sensor_%1_%2").arg(seatId, targetId);
    const QString now = timestamp(observedAt);
    const QJsonObject normalizedPosition = positionObject(position);
    Protocol::IntelContact* contact = findContact(ledger.state, id);
    const bool metadataChange = !contact
        || contact->knownAttributes != knownAttributes
        || contact->sourceUnitId != sourceUnitId
        || contact->freshness != QLatin1String("live") || !contact->actionable
        || std::abs(contact->confidence - 100.0) > 0.001;
    const double movement = contact
        ? positionDistance(contact->lastPosition, normalizedPosition)
        : std::numeric_limits<double>::infinity();
    const QDateTime lastObserved = contact ? parseTimestamp(contact->lastObservedAt) : QDateTime{};
    const double observationAge = lastObserved.isValid()
        ? lastObserved.msecsTo(observedAt) / 1000.0
        : std::numeric_limits<double>::infinity();
    const bool observationDue = !contact || metadataChange
        || observationAge >= ObservationRefreshIntervalSec
        || movement >= ImmediateMovementDistanceM;
    const Protocol::IntelHistoryEntry* previousHistory = contact
        ? latestHistoryEntry(ledger, id) : nullptr;
    const QDateTime previousHistoryAt = previousHistory
        ? parseTimestamp(previousHistory->occurredAt) : QDateTime{};
    const double historyAge = previousHistoryAt.isValid()
        ? previousHistoryAt.msecsTo(observedAt) / 1000.0
        : std::numeric_limits<double>::infinity();
    const double historyMovement = previousHistory
        ? positionDistance(previousHistory->position, normalizedPosition)
        : std::numeric_limits<double>::infinity();
    const bool historyDue = metadataChange
        || (contact && contact->lastPosition != normalizedPosition
            && (historyAge >= MovementHistoryIntervalSec
                || historyMovement >= ImmediateMovementDistanceM));
    if (!contact) {
        Protocol::IntelContact created;
        created.intelId = id;
        created.type = QStringLiteral("sensorContact");
        created.targetId = targetId;
        created.firstDiscoveredAt = now;
        created.receivedAt = now;
        created.confidence = 100.0;
        created.propagationSources = QJsonArray{};
        created.note = {};
        contact = &ledger.state.records.emplaceBack(created);
        result.code = QStringLiteral("CREATED");
        Protocol::IntelHistoryEntry entry{{}, id, QStringLiteral("discovered"), now,
                                          seatId, sourceUnitId, {}, {},
                                          QStringLiteral("live"), 100.0, normalizedPosition,
                                          knownAttributes, {}};
        entry.targetId = targetId;
        record(ledger, std::move(entry));
    } else if (historyDue) {
        result.code = QStringLiteral("UPDATED");
        Protocol::IntelHistoryEntry entry{{}, id, QStringLiteral("updated"), now,
                                          seatId, sourceUnitId, {}, {},
                                          QStringLiteral("live"), 100.0, normalizedPosition,
                                          knownAttributes, {}};
        entry.targetId = targetId;
        record(ledger, std::move(entry));
    } else if (observationDue) {
        result.code = QStringLiteral("UPDATED");
    } else {
        result.code = QStringLiteral("UNCHANGED");
    }
    if (observationDue) {
        contact->knownAttributes = knownAttributes;
        contact->lastPosition = normalizedPosition;
        contact->sourceSeatId = seatId;
        contact->sourceUnitId = sourceUnitId;
        contact->lastObservedAt = now;
        contact->receivedAt = now;
        contact->freshness = QStringLiteral("live");
        contact->actionable = true;
        contact->confidence = 100.0;
        bump(ledger);
    }
    result.ok = true;
    result.changed = observationDue;
    result.intelId = id;
    result.affectedSeats = {seatId};
    return result;
}

IntelLedger::Result IntelLedger::createManualReport(const QString& seatId,
                                                    const QString& category,
                                                    const QString& title,
                                                    const QJsonObject& position,
                                                    const QString& note,
                                                    const QDateTime& receivedAt) {
    Result result;
    if (seatId.isEmpty() || category.isEmpty() || category.size() > Protocol::MaxIdentifierLength
        || title.size() > Protocol::MaxIntelTitleLength || note.size() > Protocol::MaxIntelNoteLength
        || !validPosition(position)) {
        result.code = QStringLiteral("INVALID_INTEL");
        return result;
    }
    ensureSeat(seatId);
    SeatLedger& ledger = m_seats[seatId];
    if (ledger.state.records.size() >= Protocol::MaxIntelRecords) {
        result.code = QStringLiteral("INTEL_CAPACITY");
        return result;
    }
    const QString id = QStringLiteral("manual_%1_%2").arg(seatId).arg(ledger.nextIntel++);
    const QString now = timestamp(receivedAt);
    Protocol::IntelContact contact;
    contact.intelId = id;
    contact.type = QStringLiteral("manualReport");
    contact.knownAttributes = QJsonObject{{QStringLiteral("category"), category},
                                          {QStringLiteral("title"), title}};
    contact.lastPosition = positionObject(position);
    contact.sourceSeatId = seatId;
    contact.firstDiscoveredAt = now;
    contact.lastObservedAt = now;
    contact.receivedAt = now;
    contact.confidence = 50.0;
    contact.freshness = QStringLiteral("live");
    contact.note = note;
    contact.propagationSources = QJsonArray{};
    contact.actionable = false;
    ledger.state.records.append(contact);
    record(ledger, Protocol::IntelHistoryEntry{{}, id, QStringLiteral("discovered"), now,
                                               seatId, {}, {}, note, QStringLiteral("live"),
                                               50.0, positionObject(position),
                                               contact.knownAttributes, {}});
    bump(ledger);
    result.ok = true;
    result.changed = true;
    result.code = QStringLiteral("CREATED");
    result.intelId = id;
    result.affectedSeats = {seatId};
    return result;
}

IntelLedger::Result IntelLedger::share(const QString& senderSeatId,
                                       const QString& recipientSeatId,
                                       const QString& senderSide,
                                       const QString& recipientSide, bool reachable,
                                       const QString& intelId, const QString& note,
                                       const QDateTime& receivedAt) {
    Result result;
    if (senderSeatId.isEmpty() || recipientSeatId.isEmpty() || senderSeatId == recipientSeatId
        || senderSide.isEmpty() || senderSide != recipientSide || !reachable
        || note.size() > Protocol::MaxIntelNoteLength) {
        result.code = !reachable ? QStringLiteral("COMMUNICATION_LOST")
                                 : QStringLiteral("PERMISSION_DENIED");
        return result;
    }
    const auto sourceIt = m_seats.constFind(senderSeatId);
    if (sourceIt == m_seats.cend()) {
        result.code = QStringLiteral("INTEL_NOT_FOUND");
        return result;
    }
    const Protocol::IntelContact* source = findContact(sourceIt->state, intelId);
    if (!source) {
        result.code = QStringLiteral("INTEL_NOT_FOUND");
        return result;
    }
    if (source->freshness == QLatin1String("archived")) {
        result.code = QStringLiteral("INTEL_ARCHIVED");
        return result;
    }
    const Protocol::IntelContact sourceContact = *source;
    // Sharing is itself a durable audit event for the sender. Keep it in the
    // sender's seat ledger as well as the recipient's received history so a
    // later operator can reconstruct who propagated the contact.
    ensureSeat(recipientSeatId);
    SeatLedger& sender = m_seats[senderSeatId];
    SeatLedger& recipient = m_seats[recipientSeatId];
    Protocol::IntelContact* current = findContact(recipient.state, intelId);
    if (!current && sourceContact.type == QLatin1String("sensorContact")) {
        // Intel IDs are scoped to a seat. Merge a shared sensor contact into
        // the recipient's existing contact for the same target instead of
        // creating a second current record when that seat already observed it.
        current = findSensorContactByTarget(recipient.state, sourceContact.targetId);
    }
    if (!current && recipient.state.records.size() >= Protocol::MaxIntelRecords) {
        result.code = QStringLiteral("INTEL_CAPACITY");
        return result;
    }
    const QString now = timestamp(receivedAt);
    Protocol::IntelHistoryEntry sharedEntry{
        {}, intelId, QStringLiteral("shared"), now, sourceContact.sourceSeatId,
        sourceContact.sourceUnitId, recipientSeatId, note, sourceContact.freshness,
        sourceContact.confidence, sourceContact.lastPosition, sourceContact.knownAttributes,
        senderSeatId};
    sharedEntry.targetId = sourceContact.targetId;
    record(sender, std::move(sharedEntry));
    bump(sender);
    if (!current) {
        Protocol::IntelContact received = sourceContact;
        if (received.type == QLatin1String("sensorContact")) {
            received.intelId = QStringLiteral("sensor_%1_%2")
                                   .arg(recipientSeatId, received.targetId);
        }
        recipient.state.records.append(received);
        current = &recipient.state.records.last();
    } else {
        const QString existingId = current->intelId;
        const double existingConfidence = current->confidence;
        const QString existingNote = current->note;
        const QJsonArray existingPropagation = current->propagationSources;
        const QString existingFirstDiscovered = current->firstDiscoveredAt;
        *current = sourceContact;
        current->intelId = existingId;
        // Sharing never increases a seat's confidence. A direct or earlier
        // lower-confidence record remains the limiting value after merge.
        current->confidence = std::min(existingConfidence, sourceContact.confidence);
        current->note = note.isEmpty() ? existingNote : note;
        current->propagationSources = existingPropagation;
        if (!existingFirstDiscovered.isEmpty()
            && (current->firstDiscoveredAt.isEmpty()
                || existingFirstDiscovered < current->firstDiscoveredAt)) {
            current->firstDiscoveredAt = existingFirstDiscovered;
        }
    }
    current->receivedAt = now;
    current->note = note.isEmpty() ? current->note : note;
    QJsonArray propagation = current->propagationSources;
    propagation.append(QJsonObject{{QStringLiteral("sourceSeatId"), senderSeatId},
                                   {QStringLiteral("sourceIntelId"), intelId},
                                   {QStringLiteral("sharedAt"), now}});
    while (propagation.size() > Protocol::MaxIntelPropagationSources) {
        propagation.removeFirst();
    }
    current->propagationSources = propagation;
    Protocol::IntelHistoryEntry receivedEntry{
        {}, current->intelId, QStringLiteral("received"), now, sourceContact.sourceSeatId,
        sourceContact.sourceUnitId, recipientSeatId, note, current->freshness,
        current->confidence, current->lastPosition, current->knownAttributes, senderSeatId};
    receivedEntry.targetId = sourceContact.targetId;
    record(recipient, std::move(receivedEntry));
    bump(recipient);
    result.ok = true;
    result.changed = true;
    result.code = QStringLiteral("SHARED");
    result.intelId = intelId;
    result.affectedSeats = {senderSeatId, recipientSeatId};
    return result;
}

int IntelLedger::advance(const QDateTime& now) {
    int changed = 0;
    for (auto it = m_seats.begin(); it != m_seats.end(); ++it) {
        SeatLedger& ledger = it.value();
        for (Protocol::IntelContact& contact : ledger.state.records) {
            const QDateTime observed = QDateTime::fromString(contact.lastObservedAt,
                                                              Qt::ISODateWithMs);
            if (!observed.isValid()) continue;
            const double age = std::max(0.0, observed.msecsTo(now) / 1000.0);
            const QString next = freshnessForAge(age, m_config);
            double nextConfidence = contact.confidence;
            if (next == QLatin1String("stale")) {
                const double initialConfidence = contact.type == QLatin1String("manualReport")
                    ? 50.0 : 100.0;
                const double decayed = std::max(0.0, initialConfidence
                    * (1.0 - (age - m_config.staleAfterSec)
                       / std::max(0.001, m_config.archiveAfterSec - m_config.staleAfterSec)));
                nextConfidence = std::min(contact.confidence, decayed);
            } else if (next == QLatin1String("archived")) {
                nextConfidence = 0.0;
            }
            if (next == contact.freshness) {
                // Avoid a revision and checkpoint on every 100 ms tick while
                // still exposing a smoothly decaying stale confidence.
                if (std::abs(nextConfidence - contact.confidence) >= 0.5) {
                    contact.confidence = nextConfidence;
                    bump(ledger);
                    ++changed;
                }
                continue;
            }
            contact.freshness = next;
            contact.actionable = next == QLatin1String("live") && contact.type == QLatin1String("sensorContact");
            contact.confidence = nextConfidence;
            Protocol::IntelHistoryEntry entry{
                {}, contact.intelId,
                next == QLatin1String("archived") ? QStringLiteral("archived")
                                                   : QStringLiteral("freshnessChanged"),
                timestamp(now), contact.sourceSeatId, contact.sourceUnitId, {}, {}, next,
                contact.confidence, contact.lastPosition, contact.knownAttributes, {}};
            entry.targetId = contact.targetId;
            record(ledger, std::move(entry));
            bump(ledger);
            ++changed;
        }
    }
    return changed;
}

Protocol::IntelHistoryPage IntelLedger::historyPage(const QString& seatId,
                                                    const Protocol::IntelHistoryQuery& query) const {
    Protocol::IntelHistoryPage page;
    page.revision = state(seatId).revision;
    const auto it = m_seats.constFind(seatId);
    if (it == m_seats.cend()) return page;
    bool cursorOk = false;
    qint64 parsedCursor = query.cursor.isEmpty() ? 0 : query.cursor.toLongLong(&cursorOk);
    if (!query.cursor.isEmpty() && (!cursorOk || parsedCursor < 0)) parsedCursor = 0;
    const int pageSize = qBound(1, query.pageSize, Protocol::MaxIntelHistoryPageSize);
    const QDateTime from = parseTimestamp(query.from);
    const QDateTime to = parseTimestamp(query.to);
    qint64 lastSequence = parsedCursor;
    for (const auto& entry : it->history) {
        const qint64 sequence = historySequence(entry.historyId);
        if (sequence <= parsedCursor) continue;
        bool attributeMatches = false;
        for (auto attribute = entry.knownAttributes.constBegin();
             attribute != entry.knownAttributes.constEnd(); ++attribute) {
            if (attribute.value().isString()
                && attribute.value().toString().contains(query.target, Qt::CaseInsensitive)) {
                attributeMatches = true;
                break;
            }
        }
        const bool targetMatches = query.target.isEmpty()
            || entry.intelId.contains(query.target, Qt::CaseInsensitive)
            || entry.targetId.contains(query.target, Qt::CaseInsensitive)
            || entry.note.contains(query.target, Qt::CaseInsensitive)
            || attributeMatches;
        if (!targetMatches || (!query.type.isEmpty() && query.type !=
                               (entry.knownAttributes.value(QStringLiteral("category")).toString().isEmpty()
                                    ? QStringLiteral("sensorContact") : QStringLiteral("manualReport")))
            || (!query.freshness.isEmpty() && query.freshness != entry.freshness)
            || (!query.sourceSeatId.isEmpty() && query.sourceSeatId != entry.sourceSeatId)) continue;
        const QDateTime occurred = parseTimestamp(entry.occurredAt);
        if (from.isValid() && occurred < from) continue;
        if (to.isValid() && occurred > to) continue;
        if (page.entries.size() >= pageSize) {
            page.hasMore = true;
            page.nextCursor = QString::number(lastSequence);
            break;
        }
        page.entries.append(entry);
        lastSequence = sequence;
    }
    return page;
}

QJsonObject IntelLedger::toJson() const {
    QJsonObject seats;
    QStringList ids = m_seats.keys();
    ids.sort();
    for (const QString& seatId : ids) {
        const SeatLedger& ledger = m_seats.value(seatId);
        QJsonArray historyEntries;
        for (const auto& entry : ledger.history) historyEntries.append(Protocol::toJson(entry));
        seats[seatId] = QJsonObject{{QStringLiteral("state"), Protocol::toJson(ledger.state)},
                                    {QStringLiteral("history"), historyEntries},
                                    {QStringLiteral("nextIntel"), static_cast<qint64>(ledger.nextIntel)},
                                    {QStringLiteral("nextHistory"), static_cast<qint64>(ledger.nextHistory)}};
    }
    return QJsonObject{{QStringLiteral("staleAfterSec"), m_config.staleAfterSec},
                       {QStringLiteral("archiveAfterSec"), m_config.archiveAfterSec},
                       {QStringLiteral("seats"), seats}};
}

bool IntelLedger::restore(const QJsonObject& object, QString* error) {
    if (error) error->clear();
    const QJsonObject seats = object.value(QStringLiteral("seats")).toObject();
    if (!object.isEmpty() && !object.value(QStringLiteral("seats")).isObject()) {
        if (error) *error = QStringLiteral("情报台账检查点结构无效");
        return false;
    }
    Config config{object.value(QStringLiteral("staleAfterSec")).toDouble(m_config.staleAfterSec),
                  object.value(QStringLiteral("archiveAfterSec")).toDouble(m_config.archiveAfterSec)};
    if (config.staleAfterSec <= 0.0 || config.archiveAfterSec <= config.staleAfterSec) {
        if (error) *error = QStringLiteral("情报时效配置无效");
        return false;
    }
    QHash<QString, SeatLedger> restored;
    for (auto it = seats.constBegin(); it != seats.constEnd(); ++it) {
        if (it.key().isEmpty() || !it.value().isObject()) {
            if (error) *error = QStringLiteral("情报台账战位记录无效");
            return false;
        }
        const QJsonObject value = it.value().toObject();
        SeatLedger ledger;
        if (!Protocol::fromJson(value.value(QStringLiteral("state")).toObject(), &ledger.state).valid
            || !value.value(QStringLiteral("history")).isArray()) {
            if (error) *error = QStringLiteral("情报台账状态无效");
            return false;
        }
        for (const QJsonValue& item : value.value(QStringLiteral("history")).toArray()) {
            Protocol::IntelHistoryEntry entry;
            if (!Protocol::fromJson(item.toObject(), &entry).valid) {
                if (error) *error = QStringLiteral("情报历史记录无效");
                return false;
            }
            ledger.history.append(entry);
        }
        while (ledger.history.size() > HistoryLimit) ledger.history.removeFirst();
        ledger.nextIntel = std::max<quint64>(1, value.value(QStringLiteral("nextIntel")).toInteger(1));
        ledger.nextHistory = std::max<quint64>(1, value.value(QStringLiteral("nextHistory")).toInteger(1));
        restored.insert(it.key(), std::move(ledger));
    }
    m_config = config;
    m_seats = std::move(restored);
    return true;
}

} // namespace gbr
