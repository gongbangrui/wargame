#pragma once

#include "AuthPolicy.h"

#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

namespace gbr {

class PersistenceStore {
public:
    PersistenceStore();
    ~PersistenceStore();

    bool open(const QString& path, QString* error = nullptr);
    bool isOpen() const { return m_database.isOpen(); }
    bool saveCheckpoint(const QString& roomId, qint64 revision, qint64 serverTick,
                        const QJsonObject& checkpoint, QString* error = nullptr);
    QJsonObject loadLatestCheckpoint(const QString& roomId, qint64* revision = nullptr,
                                     qint64* serverTick = nullptr, QString* error = nullptr) const;
    bool appendAudit(const SessionIdentity& identity, const QString& commandId,
                     const QString& action, const QString& resultCode,
                     qint64 serverTick, const QJsonObject& details = {},
                     QString* error = nullptr);
    bool storeCommandResult(const QString& roomId, const QString& userId,
                            const QString& commandId, const QJsonObject& result,
                            QString* error = nullptr);
    QJsonObject commandResult(const QString& roomId, const QString& userId,
                              const QString& commandId) const;
    bool syncTokenRecords(const QVector<AuthPolicy::TokenRecord>& records,
                          QString* error = nullptr);
    bool syncPasswordAccounts(const QVector<PasswordAccount>& accounts, QString* error = nullptr);
    QVector<PasswordAccount> passwordAccounts(QString* error = nullptr) const;
    bool upsertPasswordAccount(const PasswordAccount& account, QString* error = nullptr);
    bool deletePasswordAccount(const QString& username, QString* error = nullptr);
    bool backupTo(const QString& destination, QString* error = nullptr) const;

private:
    bool migrate(QString* error);
    static bool exec(QSqlQuery& query, QString* error);

    QString m_connectionName;
    QSqlDatabase m_database;
};

} // namespace gbr
