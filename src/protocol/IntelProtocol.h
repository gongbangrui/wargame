#pragma once

#include "Protocol.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace gbr::Protocol {

// The intelligence ledger is deliberately a protocol value model.  It contains
// only information that a server has already projected for the receiving seat;
// account/session data and authoritative hidden-unit records do not belong here.
struct IntelContact {
    QString intelId;
    QString type; // sensorContact or manualReport
    QString targetId;
    QJsonObject knownAttributes;
    QJsonObject lastPosition;
    QString sourceSeatId;
    QString sourceUnitId;
    QString firstDiscoveredAt;
    QString lastObservedAt;
    QString receivedAt;
    double confidence = 0.0;
    QString freshness; // live, stale or archived
    QString note;
    QJsonArray propagationSources;
    bool actionable = false;

    QJsonObject toJson() const;
    static ValidationResult fromJson(const QJsonObject& object, IntelContact* contact);
};

struct IntelHistoryEntry {
    QString historyId;
    QString intelId;
    QString eventType; // discovered, updated, shared, freshnessChanged, archived, deleted
    QString occurredAt;
    QString sourceSeatId;
    QString sourceUnitId;
    QString recipientSeatId;
    QString note;
    QString freshness;
    double confidence = 0.0;
    QJsonObject position;
    QJsonObject knownAttributes;
    QString propagationSource;
    QString targetId;

    QJsonObject toJson() const;
    static ValidationResult fromJson(const QJsonObject& object, IntelHistoryEntry* entry);
};

struct IntelState {
    qint64 revision = 0;
    QList<IntelContact> records;
    QStringList shareTargets;

    QJsonObject toJson() const;
    static ValidationResult fromJson(const QJsonObject& object, IntelState* state);
};

struct IntelShareRequest {
    QString intelId;
    QStringList recipientSeatIds;
    QString note;

    QJsonObject toJson() const;
    static ValidationResult fromJson(const QJsonObject& object, IntelShareRequest* request);
};

struct IntelHistoryQuery {
    QString cursor;
    int pageSize = 50;
    QString target;
    QString type;
    QString freshness;
    QString sourceSeatId;
    QString from;
    QString to;

    QJsonObject toJson() const;
    static ValidationResult fromJson(const QJsonObject& object, IntelHistoryQuery* query);
};

struct IntelHistoryPage {
    QList<IntelHistoryEntry> entries;
    QString nextCursor;
    bool hasMore = false;
    qint64 revision = 0;

    QJsonObject toJson() const;
    static ValidationResult fromJson(const QJsonObject& object, IntelHistoryPage* page);
};

// Generic value-level helpers used by protocol validators and non-Qt-Quick
// clients.  `fromJson` never performs a best-effort conversion: malformed or
// out-of-range input returns INVALID_PAYLOAD and leaves the output unchanged.
ValidationResult validateIntelContact(const QJsonObject& object);
ValidationResult validateIntelHistoryEntry(const QJsonObject& object);
ValidationResult validateIntelState(const QJsonObject& object);
ValidationResult validateIntelShareRequest(const QJsonObject& object);
ValidationResult validateIntelHistoryQuery(const QJsonObject& object);
ValidationResult validateIntelHistoryPage(const QJsonObject& object);

QJsonObject toJson(const IntelContact& value);
QJsonObject toJson(const IntelHistoryEntry& value);
QJsonObject toJson(const IntelState& value);
QJsonObject toJson(const IntelShareRequest& value);
QJsonObject toJson(const IntelHistoryQuery& value);
QJsonObject toJson(const IntelHistoryPage& value);

ValidationResult fromJson(const QJsonObject& object, IntelContact* value);
ValidationResult fromJson(const QJsonObject& object, IntelHistoryEntry* value);
ValidationResult fromJson(const QJsonObject& object, IntelState* value);
ValidationResult fromJson(const QJsonObject& object, IntelShareRequest* value);
ValidationResult fromJson(const QJsonObject& object, IntelHistoryQuery* value);
ValidationResult fromJson(const QJsonObject& object, IntelHistoryPage* value);

// An intelligence delta is an ordered list of durable operations.  Upsert
// carries a projected contact; archive/delete carry only the stable intel id.
QJsonObject makeIntelDelta(const IntelState& before, const IntelState& after);
ValidationResult applyIntelDelta(IntelState* state, const QJsonObject& delta);

} // namespace gbr::Protocol
