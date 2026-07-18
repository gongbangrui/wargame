#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QString>
#include <QVector>
#include <QByteArray>

namespace gbr {

namespace SessionRole {
inline constexpr auto Red = "red";
inline constexpr auto Blue = "blue";
inline constexpr auto Director = "director";
inline constexpr auto Editor = "editor";
inline constexpr auto Observer = "observer";
}

struct SessionIdentity {
    QString userId;
    QString role;
    QString side;
    QString roomId;
    QDateTime expiresAt;

    bool isValid() const {
        return !userId.isEmpty() && !role.isEmpty() && !roomId.isEmpty()
            && (!expiresAt.isValid() || expiresAt > QDateTime::currentDateTimeUtc());
    }
    bool isPrivileged() const {
        return role == QLatin1String(SessionRole::Director)
            || role == QLatin1String(SessionRole::Editor);
    }
};

struct PasswordAccount {
    QString username;
    QByteArray salt;
    QByteArray passwordHash;
    SessionIdentity identity;
    bool disabled = false;
};

class AuthPolicy {
public:
    bool load(const QJsonArray& tokens, const QByteArray& pepper, QString* error = nullptr);
    SessionIdentity authenticate(const QString& token, QString* error = nullptr) const;
    SessionIdentity authenticatePassword(const QString& username, const QString& password,
                                         QString* error = nullptr) const;
    static QByteArray hashToken(const QString& token, const QByteArray& pepper);
    static QByteArray hashPassword(const QString& password, const QByteArray& salt,
                                   int iterations = 210000);
    static PasswordAccount makePasswordAccount(const QString& username, const QString& password,
                                               const SessionIdentity& identity);
    static bool isKnownRole(const QString& role);
    bool setPasswordAccounts(const QVector<PasswordAccount>& accounts, QString* error = nullptr);
    const QVector<PasswordAccount>& passwordAccounts() const { return m_passwordAccounts; }
    void setAdminAccount(const PasswordAccount& admin) { m_admin = admin; }
    SessionIdentity authenticateAdmin(const QString& username, const QString& password,
                                      QString* error = nullptr) const;

    struct TokenRecord {
        QByteArray tokenHash;
        SessionIdentity identity;
        bool revoked = false;
    };
    const QVector<TokenRecord>& records() const { return m_records; }

private:
    static bool constantTimeEquals(const QByteArray& left, const QByteArray& right);

    QByteArray m_pepper;
    QVector<TokenRecord> m_records;
    QVector<PasswordAccount> m_passwordAccounts;
    PasswordAccount m_admin;
};

} // namespace gbr
