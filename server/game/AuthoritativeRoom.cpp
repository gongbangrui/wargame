#include "AuthoritativeRoom.h"

#include <QJsonValue>

#include <algorithm>
#include <cmath>

namespace gbr {

namespace {

constexpr qsizetype kMaxRememberedOperations = 256;

QString typeForTemplate(const QString& templateId) {
    if (templateId == QLatin1String("commandpost")) return QStringLiteral("commander");
    if (templateId == QLatin1String("attackuav")) return QStringLiteral("attack");
    if (templateId == QLatin1String("reconuav")) return QStringLiteral("recon");
    if (templateId == QLatin1String("groundscout")) return QStringLiteral("ground");
    if (templateId == QLatin1String("jammeruav")) return QStringLiteral("jammer");
    return {};
}

QStringList seatParts(const QString& seatId) {
    return seatId.split(QLatin1Char('_'), Qt::SkipEmptyParts);
}

}

AuthoritativeRoom::AuthoritativeRoom(quint64 rngSeed)
    : m_rngState(rngSeed == 0 ? 1 : rngSeed) {}

QHash<QString, ScenarioUnit> AuthoritativeRoom::defaultTemplateCatalog() {
    QHash<QString, ScenarioUnit> catalog;
    for (const ScenarioUnit& unit : ScenarioIo::defaultScenario().units) {
        if (!catalog.contains(unit.kind)) catalog.insert(unit.kind, unit);
    }
    return catalog;
}

bool AuthoritativeRoom::setTemplateCatalog(const QHash<QString, ScenarioUnit>& catalog,
                                           QString* error) {
    if (error) error->clear();
    const QSet<QString> required{QStringLiteral("commandpost"), QStringLiteral("attackuav"),
                                 QStringLiteral("reconuav"), QStringLiteral("groundscout"),
                                 QStringLiteral("jammeruav")};
    for (const QString& id : required) {
        const ScenarioUnit unit = catalog.value(id);
        if (unit.kind != id || !std::isfinite(unit.maxHp) || unit.maxHp <= 0.0) {
            if (error) *error = QStringLiteral("invalid unit template: %1").arg(id);
            return false;
        }
    }
    m_templates = catalog;
    return true;
}

AuthoritativeRoom::Result AuthoritativeRoom::failure(const QString& code) const {
    return Result{false, code, m_revision};
}

AuthoritativeRoom::Result AuthoritativeRoom::success() {
    ++m_revision;
    return Result{true, {}, m_revision};
}

QString AuthoritativeRoom::seatForUser(qint64 userId) const {
    for (auto it = m_seats.cbegin(); it != m_seats.cend(); ++it) {
        if (it->userId == userId) return it.key();
    }
    return {};
}

bool AuthoritativeRoom::hasUser(qint64 userId) const {
    return !seatForUser(userId).isEmpty();
}

bool AuthoritativeRoom::validSeatTemplate(const QString& seatId, const QString& templateId,
                                          QString* side, QString* type) const {
    const QStringList parts = seatParts(seatId);
    if (parts.size() < 2 || parts.size() > 3 || !m_templates.contains(templateId)) return false;
    if (parts.first() != QLatin1String("red") && parts.first() != QLatin1String("blue")) return false;
    const QString expectedType = typeForTemplate(templateId);
    if (expectedType.isEmpty() || parts.at(1) != expectedType) return false;
    if (expectedType == QLatin1String("commander") && parts.size() != 2) return false;
    if (expectedType != QLatin1String("commander")) {
        bool ok = false;
        const int index = parts.value(2).toInt(&ok);
        if (!ok || index <= 0) return false;
    }
    if (side) *side = parts.first();
    if (type) *type = expectedType;
    return true;
}

QString AuthoritativeRoom::allocateUnitId(const QString& side, const QString& templateId) {
    return QStringLiteral("%1_%2_%3").arg(side, templateId).arg(m_nextUnitSequence++);
}

AuthoritativeRoom::Result AuthoritativeRoom::claimSeat(qint64 userId, const QString& username,
                                                       const QString& seatId,
                                                       const QString& templateId) {
    if (m_phase != QLatin1String("preparing") || userId <= 0 || username.trimmed().isEmpty()) {
        return failure(QStringLiteral("INVALID_STATE"));
    }
    QString side;
    QString type;
    if (!validSeatTemplate(seatId, templateId, &side, &type)) {
        return failure(QStringLiteral("INVALID_TEMPLATE"));
    }
    if (m_seats.contains(seatId) && m_seats.value(seatId).userId != userId) {
        return failure(QStringLiteral("SEAT_OCCUPIED"));
    }
    if (type == QLatin1String("commander") && side == QLatin1String("blue")
        && !m_seats.contains(QStringLiteral("red_commander"))) {
        return failure(QStringLiteral("COMMANDER_PRIORITY"));
    }
    if (type != QLatin1String("commander")
        && (!m_seats.contains(QStringLiteral("red_commander"))
            || !m_seats.contains(QStringLiteral("blue_commander")))) {
        return failure(QStringLiteral("COMMANDER_PRIORITY"));
    }
    const QString current = seatForUser(userId);
    if (!current.isEmpty() && current != seatId) return failure(QStringLiteral("TRANSFER_REQUIRED"));
    if (current == seatId) return Result{true, {}, m_revision, true};

    Seat seat{seatId, type, side, userId, username, templateId,
              allocateUnitId(side, templateId), {}, {}, true, false, false, m_revision + 1};
    m_seats.insert(seatId, seat);
    if (type != QLatin1String("commander")) {
        const QString commanderSeatId = side + QStringLiteral("_commander");
        if (m_seats.contains(commanderSeatId) && m_seats[commanderSeatId].ready) {
            m_seats[commanderSeatId].ready = false;
            m_seats[commanderSeatId].revision = m_revision + 1;
        }
    }
    m_departedUsers.remove(userId);
    return success();
}

AuthoritativeRoom::Result AuthoritativeRoom::requestTransfer(qint64 userId,
                                                             const QString& targetSeatId,
                                                             const QString& templateId) {
    if (m_phase != QLatin1String("preparing")) {
        return failure(QStringLiteral("SEAT_LOCKED"));
    }
    const QString current = seatForUser(userId);
    if (current.isEmpty()) return failure(QStringLiteral("SEAT_REQUIRED"));
    const Seat source = m_seats.value(current);
    QString side;
    QString type;
    if (source.seatType == QLatin1String("commander")) {
        return failure(QStringLiteral("SUCCESSOR_REQUIRED"));
    }
    if (m_transfers.contains(userId)) {
        return failure(QStringLiteral("TRANSFER_ALREADY_PENDING"));
    }
    if (!validSeatTemplate(targetSeatId, templateId, &side, &type) || side != source.side
        || type == QLatin1String("commander") || m_seats.contains(targetSeatId)) {
        return failure(QStringLiteral("INVALID_TRANSFER"));
    }
    Result result = success();
    m_transfers.insert(userId, Transfer{userId, targetSeatId, templateId, result.revision});
    m_seats[current].pendingTransfer = true;
    result.ok = false;
    result.code = QStringLiteral("TRANSFER_PENDING");
    return result;
}

AuthoritativeRoom::Result AuthoritativeRoom::invalidateTransfer(qint64 userId,
                                                                const QString& code) {
    const QString sourceSeat = seatForUser(userId);
    if (!m_transfers.remove(userId)) return failure(code);
    if (!sourceSeat.isEmpty()) m_seats[sourceSeat].pendingTransfer = false;
    return success();
}

AuthoritativeRoom::Result AuthoritativeRoom::cancelTransfer(qint64 userId,
                                                            quint64 requestedRevision) {
    const Transfer transfer = m_transfers.value(userId);
    if (!m_transfers.contains(userId) || transfer.revision != requestedRevision) {
        if (m_transfers.contains(userId)) invalidateTransfer(userId, QStringLiteral("STALE_TRANSFER"));
        return failure(QStringLiteral("STALE_TRANSFER"));
    }
    return invalidateTransfer(userId, QStringLiteral("TRANSFER_NOT_FOUND"));
}

AuthoritativeRoom::Result AuthoritativeRoom::rejectTransfer(qint64 commanderUserId,
                                                             qint64 userId,
                                                             quint64 requestedRevision) {
    const QString commanderSeat = seatForUser(commanderUserId);
    const QString sourceSeat = seatForUser(userId);
    if (commanderSeat.isEmpty() || sourceSeat.isEmpty()
        || m_seats.value(commanderSeat).seatType != QLatin1String("commander")
        || m_seats.value(commanderSeat).side != m_seats.value(sourceSeat).side) {
        return failure(QStringLiteral("PERMISSION_DENIED"));
    }
    const Transfer transfer = m_transfers.value(userId);
    if (!m_transfers.contains(userId) || transfer.revision != requestedRevision
        || requestedRevision != m_revision) {
        if (m_transfers.contains(userId)) invalidateTransfer(userId, QStringLiteral("STALE_TRANSFER"));
        return failure(QStringLiteral("STALE_TRANSFER"));
    }
    return invalidateTransfer(userId, QStringLiteral("TRANSFER_NOT_FOUND"));
}

AuthoritativeRoom::Result AuthoritativeRoom::approveTransfer(qint64 commanderUserId,
                                                             qint64 userId,
                                                             quint64 requestedRevision) {
    if (m_phase != QLatin1String("preparing")) {
        return failure(QStringLiteral("SEAT_LOCKED"));
    }
    const QString commanderSeat = seatForUser(commanderUserId);
    const QString sourceSeat = seatForUser(userId);
    if (commanderSeat.isEmpty() || sourceSeat.isEmpty()
        || m_seats.value(commanderSeat).seatType != QLatin1String("commander")
        || m_seats.value(commanderSeat).side != m_seats.value(sourceSeat).side) {
        return failure(QStringLiteral("PERMISSION_DENIED"));
    }
    if (!m_transfers.contains(userId) || requestedRevision != m_revision) {
        if (m_transfers.contains(userId)) invalidateTransfer(userId, QStringLiteral("STALE_TRANSFER"));
        return failure(QStringLiteral("STALE_TRANSFER"));
    }
    const Transfer transfer = m_transfers.value(userId);
    if (transfer.revision != requestedRevision || m_seats.contains(transfer.targetSeatId)) {
        invalidateTransfer(userId, QStringLiteral("STALE_TRANSFER"));
        return failure(QStringLiteral("STALE_TRANSFER"));
    }
    Seat seat = m_seats.take(sourceSeat);
    QString side;
    QString type;
    if (!validSeatTemplate(transfer.targetSeatId, transfer.templateId, &side, &type)) {
        m_seats.insert(sourceSeat, seat);
        invalidateTransfer(userId, QStringLiteral("INVALID_TRANSFER"));
        return failure(QStringLiteral("INVALID_TRANSFER"));
    }
    m_transfers.remove(userId);
    seat.seatId = transfer.targetSeatId;
    seat.seatType = type;
    seat.selectedTemplate = transfer.templateId;
    seat.unitId = allocateUnitId(side, transfer.templateId);
    seat.position = {};
    seat.deployed = false;
    seat.ready = false;
    seat.pendingTransfer = false;
    seat.revision = m_revision + 1;
    m_seats.insert(seat.seatId, seat);
    return success();
}

AuthoritativeRoom::Result AuthoritativeRoom::promote(qint64 commanderUserId,
                                                     qint64 successorUserId) {
    const QString commanderSeat = seatForUser(commanderUserId);
    const QString successorSeat = seatForUser(successorUserId);
    if (commanderSeat.isEmpty() || successorSeat.isEmpty()) {
        return failure(QStringLiteral("SUCCESSOR_NOT_FOUND"));
    }
    const Seat commander = m_seats.value(commanderSeat);
    Seat successor = m_seats.value(successorSeat);
    if (commander.seatType != QLatin1String("commander")
        || successor.seatType == QLatin1String("commander") || commander.side != successor.side
        || !successor.connected) {
        return failure(QStringLiteral("INVALID_SUCCESSOR"));
    }
    m_seats.remove(commanderSeat);
    m_seats.remove(successorSeat);
    successor.seatId = commanderSeat;
    successor.seatType = QStringLiteral("commander");
    if (m_phase == QLatin1String("preparing")) {
        successor.selectedTemplate = QStringLiteral("commandpost");
        successor.unitId = allocateUnitId(successor.side, successor.selectedTemplate);
        successor.unitName.clear();
        successor.position = {};
        successor.deployed = false;
        successor.ready = false;
    } else {
        successor.selectedTemplate = commander.selectedTemplate;
        successor.unitId = commander.unitId;
        successor.unitName = commander.unitName;
        successor.position = commander.position;
        successor.deployed = commander.deployed;
        successor.ready = commander.ready;
    }
    successor.pendingTransfer = false;
    m_transfers.remove(successorUserId);
    successor.revision = m_revision + 1;
    m_seats.insert(commanderSeat, successor);
    m_departedUsers.insert(commanderUserId);
    Result result = success();
    result.successorUserId = successorUserId;
    return result;
}

AuthoritativeRoom::Result AuthoritativeRoom::leave(qint64 userId, qint64 successorUserId) {
    const QString current = seatForUser(userId);
    if (current.isEmpty()) return Result{true, {}, m_revision, true};
    if (m_seats.value(current).seatType == QLatin1String("commander")) {
        if (successorUserId <= 0) return failure(QStringLiteral("SUCCESSOR_REQUIRED"));
        return promote(userId, successorUserId);
    }
    m_seats.remove(current);
    m_transfers.remove(userId);
    m_departedUsers.insert(userId);
    return success();
}

AuthoritativeRoom::Result AuthoritativeRoom::leaveRoom(qint64 userId) {
    const QString current = seatForUser(userId);
    if (current.isEmpty()) return Result{true, {}, m_revision, true};
    const Seat seat = m_seats.value(current);
    if (seat.seatType != QLatin1String("commander")) return leave(userId);
    const qint64 successor = chooseSuccessor(seat.side);
    if (successor > 0) return promote(userId, successor);
    if (m_phase != QLatin1String("preparing")) return failure(QStringLiteral("SUCCESSOR_REQUIRED"));
    m_seats.remove(current);
    m_departedUsers.insert(userId);
    return success();
}

qint64 AuthoritativeRoom::chooseSuccessor(const QString& side) {
    QList<qint64> candidates;
    for (const Seat& seat : m_seats) {
        if (seat.side == side && seat.seatType != QLatin1String("commander") && seat.connected) {
            candidates.append(seat.userId);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    if (candidates.isEmpty()) return 0;
    m_rngState ^= m_rngState << 13;
    m_rngState ^= m_rngState >> 7;
    m_rngState ^= m_rngState << 17;
    return candidates.at(static_cast<qsizetype>(m_rngState % candidates.size()));
}

AuthoritativeRoom::Result AuthoritativeRoom::disconnect(qint64 userId) {
    const QString current = seatForUser(userId);
    if (current.isEmpty()) {
        return Result{true, {}, m_revision, m_departedUsers.contains(userId)};
    }
    const Seat seat = m_seats.value(current);
    if (seat.seatType != QLatin1String("commander")) return leave(userId);
    const qint64 successor = chooseSuccessor(seat.side);
    if (successor > 0) return promote(userId, successor);
    if (m_phase == QLatin1String("preparing")) {
        m_seats.remove(current);
        m_departedUsers.insert(userId);
        return success();
    }
    if (m_phase == QLatin1String("finished")) return Result{true, {}, m_revision, true};
    m_seats.remove(current);
    m_departedUsers.insert(userId);
    m_phase = QStringLiteral("finished");
    m_winner = seat.side == QLatin1String("red") ? QStringLiteral("blue")
                                                  : QStringLiteral("red");
    Result result = success();
    result.forfeit = true;
    result.winner = m_winner;
    return result;
}

AuthoritativeRoom::Result AuthoritativeRoom::deploy(qint64 commanderUserId,
                                                    const QString& seatId,
                                                    const GeoPos& position) {
    const QString commanderSeat = seatForUser(commanderUserId);
    if (commanderSeat.isEmpty() || !m_seats.contains(seatId)
        || m_seats.value(commanderSeat).seatType != QLatin1String("commander")
        || m_seats.value(commanderSeat).side != m_seats.value(seatId).side
        || !std::isfinite(position.x) || !std::isfinite(position.y)
        || !std::isfinite(position.alt) || position.x < 0.0 || position.y < 0.0) {
        return failure(QStringLiteral("INVALID_DEPLOYMENT"));
    }
    Seat& seat = m_seats[seatId];
    seat.position = position;
    seat.deployed = true;
    seat.ready = false;
    seat.revision = m_revision + 1;
    return success();
}

AuthoritativeRoom::Result AuthoritativeRoom::setReady(qint64 userId, bool ready) {
    const QString current = seatForUser(userId);
    if (current.isEmpty() || (ready && !m_seats.value(current).deployed)) {
        return failure(QStringLiteral("NOT_DEPLOYED"));
    }
    const Seat currentSeat = m_seats.value(current);
    if (ready && currentSeat.seatType == QLatin1String("commander")) {
        for (const Seat& seat : m_seats) {
            if (seat.side == currentSeat.side && seat.seatType != QLatin1String("commander")
                && (!seat.deployed || !seat.ready)) {
                return failure(QStringLiteral("FRIENDLY_SEATS_NOT_READY"));
            }
        }
    }
    if (m_seats.value(current).ready == ready) return Result{true, {}, m_revision, true};
    m_seats[current].ready = ready;
    m_seats[current].revision = m_revision + 1;
    return success();
}

AuthoritativeRoom::Result AuthoritativeRoom::setUnitName(qint64 userId, const QString& unitName) {
    const QString current = seatForUser(userId);
    const QString normalized = unitName.trimmed();
    if (m_phase != QLatin1String("preparing") || current.isEmpty()
        || normalized.isEmpty() || normalized.size() > 128) {
        return failure(QStringLiteral("INVALID_UNIT_NAME"));
    }
    Seat& seat = m_seats[current];
    if (seat.unitName == normalized) return Result{true, {}, m_revision, true};
    seat.unitName = normalized;
    seat.revision = m_revision + 1;
    return success();
}

QJsonObject AuthoritativeRoom::readiness() const {
    const bool commanders = m_seats.contains(QStringLiteral("red_commander"))
        && m_seats.contains(QStringLiteral("blue_commander"));
    bool allReady = commanders && !m_seats.isEmpty();
    for (const Seat& seat : m_seats) allReady = allReady && seat.deployed && seat.ready;
    return {{QStringLiteral("commandersPresent"), commanders},
            {QStringLiteral("allOccupiedDeployed"), allReady},
            {QStringLiteral("ready"), allReady}};
}

AuthoritativeRoom::Result AuthoritativeRoom::start() {
    if ((m_phase != QLatin1String("preparing") && m_phase != QLatin1String("paused"))
        || !readiness().value(QStringLiteral("ready")).toBool()) {
        return failure(QStringLiteral("NOT_READY"));
    }
    m_phase = QStringLiteral("running");
    return success();
}

AuthoritativeRoom::Result AuthoritativeRoom::pause() {
    if (m_phase != QLatin1String("running")) return failure(QStringLiteral("INVALID_STATE"));
    m_phase = QStringLiteral("paused");
    return success();
}

AuthoritativeRoom::Result AuthoritativeRoom::finish(const QString& winner) {
    if (winner != QLatin1String("red") && winner != QLatin1String("blue")
        && winner != QLatin1String("draw")) {
        return failure(QStringLiteral("INVALID_RESULT"));
    }
    if (m_phase == QLatin1String("finished")) {
        if (m_winner != winner) return failure(QStringLiteral("RESULT_CONFLICT"));
        Result result{true, {}, m_revision, true};
        result.winner = m_winner;
        return result;
    }
    m_phase = QStringLiteral("finished");
    m_winner = winner;
    Result result = success();
    result.winner = m_winner;
    return result;
}

void AuthoritativeRoom::clearDeployment() {
    for (auto it = m_seats.begin(); it != m_seats.end(); ++it) {
        it->unitId = allocateUnitId(it->side, it->selectedTemplate);
        it->position = {};
        it->deployed = false;
        it->ready = false;
        it->pendingTransfer = false;
        it->redeployRequested = false;
        it->revision = m_revision + 1;
    }
}

void AuthoritativeRoom::clearDeployment(Seat& seat) {
    seat.position = {};
    seat.deployed = false;
    seat.ready = false;
    seat.pendingTransfer = false;
    seat.redeployRequested = false;
    seat.revision = m_revision + 1;
}

AuthoritativeRoom::Result AuthoritativeRoom::requestRedeploy(qint64 userId) {
    const QString current = seatForUser(userId);
    if (m_phase != QLatin1String("preparing") || current.isEmpty()
        || !m_seats.value(current).deployed) {
        return failure(QStringLiteral("NOT_DEPLOYED"));
    }
    Seat& seat = m_seats[current];
    if (seat.redeployRequested) return Result{true, {}, m_revision, true};
    seat.redeployRequested = true;
    seat.revision = m_revision + 1;
    return success();
}

AuthoritativeRoom::Result AuthoritativeRoom::redeploy(qint64 commanderUserId,
                                                      const QString& requestedSeatId) {
    const QString commanderSeat = seatForUser(commanderUserId);
    if (m_phase != QLatin1String("preparing") || commanderSeat.isEmpty()
        || m_seats.value(commanderSeat).seatType != QLatin1String("commander")) {
        return failure(QStringLiteral("PERMISSION_DENIED"));
    }
    QString targetSeatId = requestedSeatId;
    if (targetSeatId.isEmpty()) {
        for (const Seat& seat : m_seats) {
            if (seat.side != m_seats.value(commanderSeat).side || !seat.redeployRequested) continue;
            if (!targetSeatId.isEmpty()) return failure(QStringLiteral("TARGET_REQUIRED"));
            targetSeatId = seat.seatId;
        }
    }
    if (!m_seats.contains(targetSeatId)) return failure(QStringLiteral("INVALID_REDEPLOY_TARGET"));
    Seat& target = m_seats[targetSeatId];
    if (target.side != m_seats.value(commanderSeat).side || !target.deployed) {
        return failure(QStringLiteral("INVALID_REDEPLOY_TARGET"));
    }
    m_transfers.remove(target.userId);
    clearDeployment(target);
    if (targetSeatId != commanderSeat && m_seats[commanderSeat].ready) {
        m_seats[commanderSeat].ready = false;
        m_seats[commanderSeat].revision = m_revision + 1;
    }
    return success();
}

void AuthoritativeRoom::clearReadiness() {
    for (auto it = m_seats.begin(); it != m_seats.end(); ++it) {
        it->ready = false;
        it->revision = m_revision + 1;
    }
    ++m_revision;
}

AuthoritativeRoom::Result AuthoritativeRoom::applyOperation(const QString& operationId,
                                                            const QString& action,
                                                            quint64 requestedRevision) {
    if (operationId.trimmed().isEmpty()) return failure(QStringLiteral("INVALID_OPERATION"));
    if (m_operations.contains(operationId)) {
        Result result = m_operations.value(operationId);
        result.duplicate = true;
        return result;
    }
    if (requestedRevision != m_revision) return failure(QStringLiteral("STALE_REVISION"));
    if (action != QLatin1String("reset") && action != QLatin1String("redeploy")) {
        return failure(QStringLiteral("INVALID_OPERATION"));
    }
    m_phase = QStringLiteral("preparing");
    m_winner.clear();
    m_transfers.clear();
    if (action == QLatin1String("reset")) m_seats.clear();
    else clearDeployment();
    Result result = success();
    m_operations.insert(operationId, result);
    m_operationOrder.append(operationId);
    while (m_operationOrder.size() > kMaxRememberedOperations) {
        m_operations.remove(m_operationOrder.takeFirst());
    }
    return result;
}

bool AuthoritativeRoom::removeUser(qint64 userId) {
    const QString current = seatForUser(userId);
    if (current.isEmpty()) return false;
    m_seats.remove(current);
    m_transfers.remove(userId);
    ++m_revision;
    return true;
}

QString AuthoritativeRoom::roomStatus() const {
    if (m_phase == QLatin1String("preparing")
        && readiness().value(QStringLiteral("ready")).toBool()) return QStringLiteral("ready");
    return m_phase;
}

QJsonArray AuthoritativeRoom::runtimeUnits() const {
    QJsonArray units;
    for (const Seat& seat : m_seats) {
        if (!seat.deployed || seat.unitId.isEmpty()) continue;
        ScenarioUnit unit = m_templates.value(seat.selectedTemplate);
        unit.id = seat.unitId;
        unit.side = seat.side;
        unit.pos = seat.position;
        if (!seat.unitName.isEmpty()) {
            unit.callsign = seat.unitName;
        } else if (seat.side == QLatin1String("blue")) {
            unit.callsign.replace(QStringLiteral("红方"), QStringLiteral("蓝方"));
        }
        units.append(ScenarioIo::toJson(Scenario{ScenarioMap{}, {unit}, {}})
                         .value(QStringLiteral("units")).toArray().at(0));
    }
    return units;
}

QJsonArray AuthoritativeRoom::seatProjection() const {
    QJsonArray seats;
    QStringList ids = m_seats.keys();
    std::sort(ids.begin(), ids.end());
    for (const QString& id : ids) {
        const Seat& seat = m_seats.value(id);
        seats.append(QJsonObject{{QStringLiteral("seatId"), seat.seatId},
                                 {QStringLiteral("seatType"), seat.seatType},
                                 {QStringLiteral("side"), seat.side},
                                 {QStringLiteral("userId"), seat.userId},
                                 {QStringLiteral("username"), seat.username},
                                 {QStringLiteral("occupied"), true},
                                 {QStringLiteral("connected"), seat.connected},
                                 {QStringLiteral("commander"), seat.seatType == QLatin1String("commander")},
                                 {QStringLiteral("selectedTemplate"), seat.selectedTemplate},
                                 {QStringLiteral("unitId"), seat.unitId},
                                 {QStringLiteral("unitName"), seat.unitName},
                                 {QStringLiteral("position"), QJsonObject{
                                      {QStringLiteral("x"), seat.position.x},
                                      {QStringLiteral("y"), seat.position.y},
                                      {QStringLiteral("alt"), seat.position.alt}}},
                                 {QStringLiteral("deployed"), seat.deployed},
                                 {QStringLiteral("ready"), seat.ready},
                                 {QStringLiteral("pendingTransfer"), seat.pendingTransfer},
                                 {QStringLiteral("redeployRequested"), seat.redeployRequested},
                                 {QStringLiteral("revision"), static_cast<qint64>(seat.revision)}});
    }
    return seats;
}

QJsonObject AuthoritativeRoom::pendingTransfer(qint64 userId) const {
    const Transfer transfer = m_transfers.value(userId);
    if (!m_transfers.contains(userId)) return {};
    const QString sourceSeat = seatForUser(userId);
    return {{QStringLiteral("userId"), userId},
            {QStringLiteral("sourceSeatId"), sourceSeat},
            {QStringLiteral("sourceSide"), m_seats.value(sourceSeat).side},
            {QStringLiteral("targetSeatId"), transfer.targetSeatId},
            {QStringLiteral("templateId"), transfer.templateId},
            {QStringLiteral("revision"), static_cast<qint64>(transfer.revision)}};
}

QJsonObject AuthoritativeRoom::toJson() const {
    QJsonArray operations;
    for (const QString& operationId : m_operationOrder) {
        const Result result = m_operations.value(operationId);
        operations.append(QJsonObject{{QStringLiteral("operationId"), operationId},
                                      {QStringLiteral("revision"), static_cast<qint64>(result.revision)}});
    }
    QJsonArray transfers;
    for (const Transfer& transfer : m_transfers) {
        transfers.append(QJsonObject{{QStringLiteral("userId"), transfer.userId},
                                     {QStringLiteral("targetSeatId"), transfer.targetSeatId},
                                     {QStringLiteral("templateId"), transfer.templateId},
                                     {QStringLiteral("revision"),
                                      static_cast<qint64>(transfer.revision)}});
    }
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("phase"), m_phase},
            {QStringLiteral("winner"), m_winner},
            {QStringLiteral("revision"), static_cast<qint64>(m_revision)},
            {QStringLiteral("nextUnitSequence"), static_cast<qint64>(m_nextUnitSequence)},
            {QStringLiteral("rngState"), static_cast<qint64>(m_rngState)},
            {QStringLiteral("seats"), seatProjection()},
            {QStringLiteral("transfers"), transfers},
            {QStringLiteral("operations"), operations}};
}

bool AuthoritativeRoom::restore(const QJsonObject& object, QString* error) {
    if (error) error->clear();
    const qint64 revision = object.value(QStringLiteral("revision")).toInteger();
    const qint64 nextUnit = object.value(QStringLiteral("nextUnitSequence")).toInteger();
    const qint64 rng = object.value(QStringLiteral("rngState")).toInteger();
    const QString phase = object.value(QStringLiteral("phase")).toString();
    if (object.value(QStringLiteral("schemaVersion")).toInt() != 1 || revision <= 0
        || nextUnit <= 0 || rng <= 0
        || (phase != QLatin1String("preparing") && phase != QLatin1String("running")
            && phase != QLatin1String("paused") && phase != QLatin1String("finished"))) {
        if (error) *error = QStringLiteral("invalid authoritative room state");
        return false;
    }
    QHash<QString, Seat> seats;
    for (const QJsonValue& value : object.value(QStringLiteral("seats")).toArray()) {
        const QJsonObject item = value.toObject();
        Seat seat{item.value(QStringLiteral("seatId")).toString(),
                  item.value(QStringLiteral("seatType")).toString(),
                  item.value(QStringLiteral("side")).toString(),
                  item.value(QStringLiteral("userId")).toInteger(),
                  item.value(QStringLiteral("username")).toString(),
                  item.value(QStringLiteral("selectedTemplate")).toString(),
                  item.value(QStringLiteral("unitId")).toString(),
                  item.value(QStringLiteral("unitName")).toString(),
                  GeoPos{item.value(QStringLiteral("position")).toObject()
                             .value(QStringLiteral("x")).toDouble(),
                         item.value(QStringLiteral("position")).toObject()
                             .value(QStringLiteral("y")).toDouble(),
                         item.value(QStringLiteral("position")).toObject()
                             .value(QStringLiteral("alt")).toDouble()},
                  item.value(QStringLiteral("connected")).toBool(),
                  item.value(QStringLiteral("deployed")).toBool(),
                  item.value(QStringLiteral("ready")).toBool(),
                  static_cast<quint64>(item.value(QStringLiteral("revision")).toInteger()),
                  item.value(QStringLiteral("pendingTransfer")).toBool(),
                  item.value(QStringLiteral("redeployRequested")).toBool()};
        QString side;
        QString type;
        if (seat.userId <= 0 || seats.contains(seat.seatId)
            || !validSeatTemplate(seat.seatId, seat.selectedTemplate, &side, &type)
            || side != seat.side || type != seat.seatType || (seat.ready && !seat.deployed)) {
            if (error) *error = QStringLiteral("invalid persisted seat state");
            return false;
        }
        seats.insert(seat.seatId, seat);
    }
    m_seats = seats;
    m_phase = phase;
    m_winner = object.value(QStringLiteral("winner")).toString();
    m_revision = static_cast<quint64>(revision);
    m_nextUnitSequence = static_cast<quint64>(nextUnit);
    m_rngState = static_cast<quint64>(rng);
    m_transfers.clear();
    for (const QJsonValue& value : object.value(QStringLiteral("transfers")).toArray()) {
        const QJsonObject item = value.toObject();
        const qint64 userId = item.value(QStringLiteral("userId")).toInteger();
        const QString targetSeatId = item.value(QStringLiteral("targetSeatId")).toString();
        const QString templateId = item.value(QStringLiteral("templateId")).toString();
        const qint64 transferRevision = item.value(QStringLiteral("revision")).toInteger();
        if (userId <= 0 || !hasUser(userId) || targetSeatId.isEmpty()
            || !m_templates.contains(templateId) || transferRevision <= 0) {
            if (error) *error = QStringLiteral("invalid persisted transfer state");
            return false;
        }
        m_transfers.insert(userId,
                           Transfer{userId, targetSeatId, templateId,
                                    static_cast<quint64>(transferRevision)});
    }
    m_operations.clear();
    m_operationOrder.clear();
    for (const QJsonValue& value : object.value(QStringLiteral("operations")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString id = item.value(QStringLiteral("operationId")).toString();
        const qint64 operationRevision = item.value(QStringLiteral("revision")).toInteger();
        if (!id.isEmpty() && operationRevision > 0) {
            m_operations.insert(id, Result{true, {}, static_cast<quint64>(operationRevision)});
            m_operationOrder.removeAll(id);
            m_operationOrder.append(id);
            while (m_operationOrder.size() > kMaxRememberedOperations) {
                m_operations.remove(m_operationOrder.takeFirst());
            }
        }
    }
    return true;
}

}
