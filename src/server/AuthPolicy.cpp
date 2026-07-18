#include "AuthPolicy.h"

#include <QCryptographicHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QRandomGenerator>

namespace gbr {

QByteArray AuthPolicy::hashToken(const QString& token, const QByteArray& pepper) {
    return QCryptographicHash::hash(pepper + ':' + token.toUtf8(), QCryptographicHash::Sha256).toHex();
}

namespace {

QByteArray hmacSha256(const QByteArray& key, const QByteArray& message) {
    QByteArray block = key;
    if (block.size() > 64) block = QCryptographicHash::hash(block, QCryptographicHash::Sha256);
    block = block.leftJustified(64, '\0', true);
    QByteArray inner(64, '\x36');
    QByteArray outer(64, '\x5c');
    for (int i = 0; i < 64; ++i) {
        inner[i] = static_cast<char>(inner.at(i) ^ block.at(i));
        outer[i] = static_cast<char>(outer.at(i) ^ block.at(i));
    }
    const QByteArray innerHash = QCryptographicHash::hash(inner + message, QCryptographicHash::Sha256);
    return QCryptographicHash::hash(outer + innerHash, QCryptographicHash::Sha256);
}

}

QByteArray AuthPolicy::hashPassword(const QString& password, const QByteArray& salt, int iterations) {
    const QByteArray pass = password.toUtf8();
    const QByteArray blockIndex("\0\0\0\1", 4);
    QByteArray u = hmacSha256(pass, salt + blockIndex);
    QByteArray result = u;
    for (int i = 1; i < iterations; ++i) {
        u = hmacSha256(pass, u);
        for (int j = 0; j < result.size(); ++j) result[j] ^= u.at(j);
    }
    return result.toHex();
}

PasswordAccount AuthPolicy::makePasswordAccount(const QString& username, const QString& password,
                                                const SessionIdentity& identity) {
    PasswordAccount account;
    account.username = username.trimmed();
    account.salt.resize(16);
    auto* begin = reinterpret_cast<quint32*>(account.salt.data());
    QRandomGenerator::global()->generate(begin, begin + 4);
    account.passwordHash = hashPassword(password, account.salt);
    account.identity = identity;
    return account;
}

bool AuthPolicy::constantTimeEquals(const QByteArray& left, const QByteArray& right) {
    if (left.size() != right.size()) return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i) {
        difference |= static_cast<unsigned char>(left.at(i) ^ right.at(i));
    }
    return difference == 0;
}

bool AuthPolicy::isKnownRole(const QString& role) {
    return role == QLatin1String(SessionRole::Red)
        || role == QLatin1String(SessionRole::Blue)
        || role == QLatin1String(SessionRole::Director)
        || role == QLatin1String(SessionRole::Editor)
        || role == QLatin1String(SessionRole::Observer);
}

bool AuthPolicy::load(const QJsonArray& tokens, const QByteArray& pepper, QString* error) {
    if (error) error->clear();
    m_records.clear();
    m_pepper = pepper;
    if (m_pepper.size() < 32) {
        if (error) *error = QStringLiteral("token pepper 至少需要 32 字节");
        return false;
    }
    static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    static const QSet<QString> tokenFields{
        QStringLiteral("tokenHash"), QStringLiteral("userId"),
        QStringLiteral("role"), QStringLiteral("side"),
        QStringLiteral("roomId"), QStringLiteral("expiresAt"),
        QStringLiteral("revoked"),
    };
    QSet<QByteArray> seenHashes;
    for (qsizetype index = 0; index < tokens.size(); ++index) {
        if (!tokens.at(index).isObject()) {
            if (error) *error = QStringLiteral("tokens[%1] 必须是对象").arg(index);
            m_records.clear();
            return false;
        }
        const QJsonObject object = tokens.at(index).toObject();
        for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
            if (!tokenFields.contains(it.key())) {
                if (error) *error = QStringLiteral("tokens[%1] 包含未知字段: %2")
                                        .arg(index).arg(it.key());
                m_records.clear();
                return false;
            }
        }
        TokenRecord record;
        record.tokenHash = object.value(QStringLiteral("tokenHash")).toString().toLatin1().toLower();
        record.identity.userId = object.value(QStringLiteral("userId")).toString().trimmed();
        record.identity.role = object.value(QStringLiteral("role")).toString().trimmed().toLower();
        record.identity.side = object.value(QStringLiteral("side")).toString().trimmed().toLower();
        record.identity.roomId = object.value(QStringLiteral("roomId")).toString().trimmed();
        record.identity.expiresAt = QDateTime::fromString(
            object.value(QStringLiteral("expiresAt")).toString(), Qt::ISODate);
        record.revoked = object.value(QStringLiteral("revoked")).toBool(false);
        if (!sha256Pattern.match(QString::fromLatin1(record.tokenHash)).hasMatch()
            || seenHashes.contains(record.tokenHash)
            || record.identity.userId.isEmpty()
            || record.identity.userId.size() > 128
            || record.identity.roomId.isEmpty()
            || record.identity.roomId.size() > 128
            || !object.value(QStringLiteral("expiresAt")).isString()
            || !record.identity.expiresAt.isValid()
            || !isKnownRole(record.identity.role)) {
            if (error) *error = QStringLiteral("tokens[%1] 结构无效").arg(index);
            m_records.clear();
            return false;
        }
        if (record.identity.role == QLatin1String(SessionRole::Red)
            || record.identity.role == QLatin1String(SessionRole::Blue)) {
            const QString expectedSide = record.identity.role;
            if (record.identity.side.isEmpty()) record.identity.side = expectedSide;
            if (record.identity.side != expectedSide) {
                if (error) *error = QStringLiteral("tokens[%1] 的角色与阵营不一致").arg(index);
                m_records.clear();
                return false;
            }
        } else {
            record.identity.side.clear();
        }
        seenHashes.insert(record.tokenHash);
        m_records.push_back(record);
    }
    if (m_records.isEmpty()) {
        if (error) *error = QStringLiteral("至少需要配置一个 tokenHash");
        return false;
    }
    return true;
}

SessionIdentity AuthPolicy::authenticate(const QString& token, QString* error) const {
    if (error) error->clear();
    if (token.size() < 32 || token.size() > 512) {
        if (error) *error = QStringLiteral("token 格式无效");
        return {};
    }
    const QByteArray candidate = hashToken(token, m_pepper);
    for (const auto& record : m_records) {
        if (!constantTimeEquals(candidate, record.tokenHash)) continue;
        if (record.revoked) {
            if (error) *error = QStringLiteral("token 已撤销");
            return {};
        }
        if (record.identity.expiresAt.isValid()
            && record.identity.expiresAt <= QDateTime::currentDateTimeUtc()) {
            if (error) *error = QStringLiteral("token 已过期");
            return {};
        }
        return record.identity;
    }
    if (error) *error = QStringLiteral("token 无效");
    return {};
}

bool AuthPolicy::setPasswordAccounts(const QVector<PasswordAccount>& accounts, QString* error) {
    if (error) error->clear();
    static const QRegularExpression usernamePattern(QStringLiteral("^[A-Za-z0-9_.-]{1,64}$"));
    QSet<QString> names;
    for (const auto& account : accounts) {
        const QString role = account.identity.role.trimmed().toLower();
        const QString side = account.identity.side.trimmed().toLower();
        const bool sideMatchesRole = (role == QLatin1String(SessionRole::Red) && side == QLatin1String("red"))
            || (role == QLatin1String(SessionRole::Blue) && side == QLatin1String("blue"))
            || (role != QLatin1String(SessionRole::Red) && role != QLatin1String(SessionRole::Blue) && side.isEmpty());
        if (!usernamePattern.match(account.username).hasMatch()
            || names.contains(account.username) || account.salt.size() < 16
            || account.passwordHash.size() != 64 || !account.identity.isValid()
            || !account.identity.expiresAt.isValid() || !isKnownRole(role)
            || !sideMatchesRole || account.identity.userId.size() > 128
            || account.identity.roomId.size() > 128) {
            if (error) *error = QStringLiteral("账号密码记录结构无效");
            return false;
        }
        names.insert(account.username);
    }
    m_passwordAccounts = accounts;
    return true;
}

SessionIdentity AuthPolicy::authenticatePassword(const QString& username, const QString& password,
                                                 QString* error) const {
    if (error) error->clear();
    const QString normalized = username.trimmed();
    if (normalized.isEmpty() || normalized.size() > 128 || password.size() < 8 || password.size() > 512) {
        if (error) *error = QStringLiteral("账号或密码格式无效");
        return {};
    }
    for (const auto& account : m_passwordAccounts) {
        if (account.username != normalized) continue;
        if (account.disabled) {
            if (error) *error = QStringLiteral("账号已禁用");
            return {};
        }
        const QByteArray candidate = hashPassword(password, account.salt);
        if (!constantTimeEquals(candidate, account.passwordHash)) break;
        if (!account.identity.isValid()) {
            if (error) *error = QStringLiteral("账号已过期");
            return {};
        }
        return account.identity;
    }
    if (error) *error = QStringLiteral("账号或密码错误");
    return {};
}

SessionIdentity AuthPolicy::authenticateAdmin(const QString& username, const QString& password,
                                              QString* error) const {
    if (error) error->clear();
    if (m_admin.username.isEmpty() || username.trimmed() != m_admin.username
        || password.size() < 8
        || !constantTimeEquals(hashPassword(password, m_admin.salt), m_admin.passwordHash)) {
        if (error) *error = QStringLiteral("管理员账号或密码错误");
        return {};
    }
    return m_admin.identity;
}

} // namespace gbr
