#include "PersistenceStore.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace gbr {

PersistenceStore::PersistenceStore()
    : m_connectionName(QStringLiteral("wargame_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))) {}

PersistenceStore::~PersistenceStore() {
    if (m_database.isValid()) m_database.close();
    m_database = {};
    QSqlDatabase::removeDatabase(m_connectionName);
}

bool PersistenceStore::exec(QSqlQuery& query, QString* error) {
    if (query.exec()) return true;
    if (error) *error = query.lastError().text();
    return false;
}

bool PersistenceStore::open(const QString& path, QString* error) {
    if (error) error->clear();
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error) *error = QStringLiteral("无法创建数据库目录: %1").arg(info.absolutePath());
        return false;
    }
    m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_database.setDatabaseName(path);
    if (!m_database.open()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    QSqlQuery pragma(m_database);
    if (!pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"))
        || !pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"))
        || !pragma.exec(QStringLiteral("PRAGMA synchronous=FULL"))) {
        if (error) *error = pragma.lastError().text();
        return false;
    }
    return migrate(error);
}

bool PersistenceStore::migrate(QString* error) {
    QSqlQuery query(m_database);
    const QStringList statements{
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_meta (version INTEGER NOT NULL)"),
        QStringLiteral("INSERT INTO schema_meta(version) SELECT 1 WHERE NOT EXISTS (SELECT 1 FROM schema_meta)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS token_identities (token_hash TEXT PRIMARY KEY, user_id TEXT NOT NULL, role TEXT NOT NULL, side TEXT NOT NULL, room_id TEXT NOT NULL, expires_at TEXT, revoked INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS password_accounts (username TEXT PRIMARY KEY, salt TEXT NOT NULL, password_hash TEXT NOT NULL, user_id TEXT NOT NULL, role TEXT NOT NULL, side TEXT NOT NULL, room_id TEXT NOT NULL, expires_at TEXT, disabled INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS checkpoints (id INTEGER PRIMARY KEY AUTOINCREMENT, room_id TEXT NOT NULL, revision INTEGER NOT NULL, server_tick INTEGER NOT NULL, created_at TEXT NOT NULL, payload BLOB NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS checkpoints_room_tick ON checkpoints(room_id, server_tick DESC)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS audit_events (id INTEGER PRIMARY KEY AUTOINCREMENT, created_at TEXT NOT NULL, room_id TEXT NOT NULL, user_id TEXT NOT NULL, role TEXT NOT NULL, command_id TEXT NOT NULL, action TEXT NOT NULL, result_code TEXT NOT NULL, server_tick INTEGER NOT NULL, details TEXT NOT NULL)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS audit_room_time ON audit_events(room_id, created_at)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS command_results (room_id TEXT NOT NULL, user_id TEXT NOT NULL, command_id TEXT NOT NULL, created_at INTEGER NOT NULL, result TEXT NOT NULL, PRIMARY KEY(room_id, user_id, command_id))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS command_results_created_at ON command_results(created_at)"),
    };
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            if (error) *error = query.lastError().text();
            return false;
        }
    }
    return true;
}

bool PersistenceStore::saveCheckpoint(const QString& roomId, qint64 revision, qint64 serverTick,
                                      const QJsonObject& checkpoint, QString* error) {
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO checkpoints(room_id, revision, server_tick, created_at, payload) VALUES(?, ?, ?, ?, ?)"));
    query.addBindValue(roomId);
    query.addBindValue(revision);
    query.addBindValue(serverTick);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(qCompress(QJsonDocument(checkpoint).toJson(QJsonDocument::Compact), 9));
    return exec(query, error);
}

QJsonObject PersistenceStore::loadLatestCheckpoint(const QString& roomId, qint64* revision,
                                                   qint64* serverTick, QString* error) const {
    if (error) error->clear();
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT revision, server_tick, payload FROM checkpoints WHERE room_id=? ORDER BY server_tick DESC, id DESC"));
    query.addBindValue(roomId);
    if (!query.exec()) {
        if (error) *error = query.lastError().text();
        return {};
    }
    bool foundCheckpoint = false;
    while (query.next()) {
        foundCheckpoint = true;
        const QByteArray json = qUncompress(query.value(2).toByteArray());
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || document.object().isEmpty()) {
            continue;
        }
        if (revision) *revision = query.value(0).toLongLong();
        if (serverTick) *serverTick = query.value(1).toLongLong();
        return document.object();
    }
    if (foundCheckpoint && error) {
        *error = QStringLiteral("房间存在检查点，但没有可解析的有效记录");
    }
    return {};
}

bool PersistenceStore::appendAudit(const SessionIdentity& identity, const QString& commandId,
                                   const QString& action, const QString& resultCode,
                                   qint64 serverTick, const QJsonObject& details, QString* error) {
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT INTO audit_events(created_at, room_id, user_id, role, command_id, action, result_code, server_tick, details) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(identity.roomId);
    query.addBindValue(identity.userId);
    query.addBindValue(identity.role);
    query.addBindValue(commandId);
    query.addBindValue(action);
    query.addBindValue(resultCode);
    query.addBindValue(serverTick);
    query.addBindValue(QString::fromUtf8(QJsonDocument(details).toJson(QJsonDocument::Compact)));
    return exec(query, error);
}

bool PersistenceStore::storeCommandResult(const QString& roomId, const QString& userId,
                                          const QString& commandId, const QJsonObject& result,
                                          QString* error) {
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO command_results(room_id, user_id, command_id, created_at, result) VALUES(?, ?, ?, ?, ?)"));
    query.addBindValue(roomId);
    query.addBindValue(userId);
    query.addBindValue(commandId);
    query.addBindValue(QDateTime::currentSecsSinceEpoch());
    query.addBindValue(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    if (!exec(query, error)) return false;
    QSqlQuery cleanup(m_database);
    cleanup.prepare(QStringLiteral("DELETE FROM command_results WHERE created_at<?"));
    cleanup.addBindValue(QDateTime::currentSecsSinceEpoch() - 600);
    return exec(cleanup, error);
}

QJsonObject PersistenceStore::commandResult(const QString& roomId, const QString& userId,
                                            const QString& commandId) const {
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT result FROM command_results WHERE room_id=? AND user_id=? AND command_id=? AND created_at>=?"));
    query.addBindValue(roomId);
    query.addBindValue(userId);
    query.addBindValue(commandId);
    query.addBindValue(QDateTime::currentSecsSinceEpoch() - 600);
    if (!query.exec() || !query.next()) return {};
    return QJsonDocument::fromJson(query.value(0).toString().toUtf8()).object();
}

bool PersistenceStore::syncTokenRecords(const QVector<AuthPolicy::TokenRecord>& records,
                                        QString* error) {
    if (!m_database.transaction()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    for (const auto& record : records) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT OR REPLACE INTO token_identities(token_hash, user_id, role, side, room_id, expires_at, revoked) VALUES(?, ?, ?, ?, ?, ?, ?)"));
        query.addBindValue(QString::fromLatin1(record.tokenHash));
        query.addBindValue(record.identity.userId);
        query.addBindValue(record.identity.role);
        query.addBindValue(record.identity.side.isEmpty()
                               ? QStringLiteral("")
                               : record.identity.side);
        query.addBindValue(record.identity.roomId);
        query.addBindValue(record.identity.expiresAt.isValid() ? record.identity.expiresAt.toString(Qt::ISODate) : QString());
        query.addBindValue(record.revoked ? 1 : 0);
        if (!exec(query, error)) {
            m_database.rollback();
            return false;
        }
    }
    if (!m_database.commit()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    return true;
}

bool PersistenceStore::syncPasswordAccounts(const QVector<PasswordAccount>& accounts, QString* error) {
    if (!m_database.transaction()) {
        if (error) *error = m_database.lastError().text();
        return false;
    }
    for (const auto& account : accounts) {
        QSqlQuery query(m_database);
        query.prepare(QStringLiteral("INSERT OR REPLACE INTO password_accounts(username, salt, password_hash, user_id, role, side, room_id, expires_at, disabled) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        query.addBindValue(account.username);
        query.addBindValue(QString::fromLatin1(account.salt.toHex()));
        query.addBindValue(QString::fromLatin1(account.passwordHash));
        query.addBindValue(account.identity.userId);
        query.addBindValue(account.identity.role);
        query.addBindValue(account.identity.side);
        query.addBindValue(account.identity.roomId);
        query.addBindValue(account.identity.expiresAt.toString(Qt::ISODate));
        query.addBindValue(account.disabled ? 1 : 0);
        if (!exec(query, error)) { m_database.rollback(); return false; }
    }
    if (!m_database.commit()) { if (error) *error = m_database.lastError().text(); return false; }
    return true;
}

QVector<PasswordAccount> PersistenceStore::passwordAccounts(QString* error) const {
    if (error) error->clear();
    QVector<PasswordAccount> result;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT username, salt, password_hash, user_id, role, side, room_id, expires_at, disabled FROM password_accounts ORDER BY username"))) {
        if (error) *error = query.lastError().text();
        return result;
    }
    while (query.next()) {
        PasswordAccount account;
        account.username = query.value(0).toString();
        account.salt = QByteArray::fromHex(query.value(1).toString().toLatin1());
        account.passwordHash = query.value(2).toString().toLatin1();
        account.identity = {query.value(3).toString(), query.value(4).toString(), query.value(5).toString(),
                            query.value(6).toString(), QDateTime::fromString(query.value(7).toString(), Qt::ISODate)};
        account.disabled = query.value(8).toInt() != 0;
        result.push_back(account);
    }
    return result;
}

bool PersistenceStore::upsertPasswordAccount(const PasswordAccount& account, QString* error) {
    return syncPasswordAccounts(QVector<PasswordAccount>{account}, error);
}

bool PersistenceStore::deletePasswordAccount(const QString& username, QString* error) {
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DELETE FROM password_accounts WHERE username=?"));
    query.addBindValue(username);
    return exec(query, error);
}

bool PersistenceStore::backupTo(const QString& destination, QString* error) const {
    const QFileInfo info(destination);
    if (!QDir().mkpath(info.absolutePath())) {
        if (error) *error = QStringLiteral("无法创建备份目录");
        return false;
    }
    QSqlQuery query(m_database);
    const QString escaped = QString(destination).replace(QLatin1Char('\''), QStringLiteral("''"));
    if (!query.exec(QStringLiteral("VACUUM INTO '%1'").arg(escaped))) {
        if (error) *error = query.lastError().text();
        return false;
    }
    return true;
}

} // namespace gbr
