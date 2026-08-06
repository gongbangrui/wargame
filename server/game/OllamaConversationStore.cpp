#include "OllamaConversationStore.h"

// allow: SIZE_OK - one durable redacted JSONL store owns append, retention, and readback invariants.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSet>
#include <QSaveFile>
#include <QTimeZone>

#include <algorithm>
#include <limits>

#ifdef Q_OS_UNIX
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <io.h>
#endif

namespace gbr {

namespace {

constexpr qint64 kMaxStoreBytes = 10 * 1024 * 1024;
constexpr qint64 kMaxFileBytes = 1024 * 1024;
constexpr qint64 kMaxRecordBytes = kMaxStoreBytes;
constexpr int kRetentionDays = 7;
const QString kFilePrefix = QStringLiteral("ai-conversations-");

QDateTime currentUtc(const OllamaConversationStore::Clock& clock) {
    const QDateTime value = clock ? clock() : QDateTime::currentDateTimeUtc();
    return (value.isValid() ? value : QDateTime::currentDateTimeUtc()).toUTC();
}

void errorText(QString* error, const QString& message) {
    if (error) *error = message;
}

bool pathIsUnder(const QString& root, const QString& path) {
    const QString relative = QDir(root).relativeFilePath(path);
    return relative != QLatin1String("..") && !relative.startsWith(QStringLiteral("../"))
        && !QDir::isAbsolutePath(relative);
}

QString normalizedKey(const QString& key) {
    QString normalized;
    normalized.reserve(key.size());
    for (const QChar character : key.toLower()) {
        if (character.isLetterOrNumber()) normalized.append(character);
    }
    return normalized;
}

bool sensitiveKey(const QString& key) {
    const QString normalized = normalizedKey(key);
    for (const QString& marker : {QStringLiteral("password"), QStringLiteral("passwd"),
                                  QStringLiteral("token"), QStringLiteral("authorization"),
                                  QStringLiteral("authheader"), QStringLiteral("cookie"),
                                  QStringLiteral("secret"), QStringLiteral("apikey"),
                                  QStringLiteral("credential"), QStringLiteral("privatekey"),
                                  QStringLiteral("internalkey"), QStringLiteral("bearer")}) {
        if (normalized.contains(marker)) return true;
    }
    return false;
}

QString redactString(const QString& value) {
    QString result = value;
    static const QRegularExpression bearer(
        QStringLiteral("\\bBearer\\s+[^\\s,;]+"), QRegularExpression::CaseInsensitiveOption);
    result.replace(bearer, QStringLiteral("Bearer [REDACTED]"));
    static const QRegularExpression assignment(
        QStringLiteral("(?i)(authorization|set-cookie|cookie|x-api-key|api[_-]?key|"
                       "password|passwd|token|secret|credential)\\s*([:=])\\s*"
                       "(\\\"[^\\\"]*\\\"|'[^']*'|[^\\s,;]+)"));
    result.replace(assignment, QStringLiteral("\\1\\2[REDACTED]"));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(result.toUtf8(), &parseError);
    if (parseError.error == QJsonParseError::NoError
        && (document.isObject() || document.isArray())) {
        const QJsonValue redacted = OllamaConversationStore::redact(
            document.isObject() ? QJsonValue(document.object()) : QJsonValue(document.array()));
        return QString::fromUtf8(QJsonDocument::fromVariant(redacted.toVariant())
                                     .toJson(QJsonDocument::Compact));
    }
    return result;
}

QJsonValue redactValue(const QJsonValue& value) {
    if (value.isObject()) return OllamaConversationStore::redact(value.toObject());
    if (value.isArray()) {
        QJsonArray array;
        for (const QJsonValue& child : value.toArray()) array.append(redactValue(child));
        return array;
    }
    if (value.isString()) return redactString(value.toString());
    return value;
}

bool durableFlush(QFileDevice& file, QString* error) {
    if (qEnvironmentVariableIntValue("WARGAME_FORCE_CONVERSATION_STORE_FLUSH_FAILURE") != 0
        || qEnvironmentVariableIntValue("WARGAME_FORCE_FLUSH_FAILURE") != 0) {
        errorText(error, QStringLiteral("conversation store flush failure"));
        return false;
    }
    if (!file.flush()) {
        errorText(error, QStringLiteral("conversation store write failure"));
        return false;
    }
#ifdef Q_OS_UNIX
    if (::fsync(file.handle()) != 0) {
        errorText(error, QStringLiteral("conversation store fsync failure"));
        return false;
    }
#elif defined(Q_OS_WIN)
    const intptr_t descriptor = static_cast<intptr_t>(file.handle());
    const intptr_t nativeHandle = _get_osfhandle(static_cast<int>(descriptor));
    if (nativeHandle == -1 || !FlushFileBuffers(reinterpret_cast<HANDLE>(nativeHandle))) {
        errorText(error, QStringLiteral("conversation store fsync failure"));
        return false;
    }
#else
    Q_UNUSED(file);
#endif
    return true;
}

QString datePart(const QString& path) {
    static const QRegularExpression expression(
        QStringLiteral("^ai-conversations-(\\d{4}-\\d{2}-\\d{2})(?:-\\d+)?\\.jsonl$"));
    const QRegularExpressionMatch match = expression.match(QFileInfo(path).fileName());
    return match.hasMatch() ? match.captured(1) : QString();
}

QDateTime recordTime(const QJsonObject& object) {
    const QString value = object.value(QStringLiteral("time")).toString();
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    return parsed.isValid() ? parsed.toUTC() : QDateTime();
}

bool keepByAge(const QJsonObject& object, const QDateTime& cutoff, const QString& path) {
    const QDateTime time = recordTime(object);
    if (time.isValid()) return time >= cutoff;
    const QDate date = QDate::fromString(datePart(path), QStringLiteral("yyyy-MM-dd"));
    if (!date.isValid()) return true;
    return QDateTime(date, QTime(0, 0), QTimeZone::utc()).addDays(1) > cutoff;
}

qint64 appendBoundaryBytes(const QString& path, QString* error) {
    const QFileInfo info(path);
    if (!info.exists() || info.size() == 0) return 0;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        errorText(error, QStringLiteral("cannot inspect conversation store file"));
        return -1;
    }
    if (!file.seek(info.size() - 1)) {
        errorText(error, QStringLiteral("cannot inspect conversation store file"));
        return -1;
    }
    const QByteArray lastByte = file.read(1);
    if (lastByte.size() != 1) {
        errorText(error, QStringLiteral("cannot inspect conversation store file"));
        return -1;
    }
    return lastByte.at(0) == '\n' ? 0 : 1;
}

bool sameFinalRequest(const QJsonObject& left, const QJsonObject& right) {
    const QString leftConversation = left.value(QStringLiteral("conversationId")).toString();
    const QString rightConversation = right.value(QStringLiteral("conversationId")).toString();
    const QString leftRequest = left.value(QStringLiteral("requestId")).toString();
    const QString rightRequest = right.value(QStringLiteral("requestId")).toString();
    return !leftConversation.isEmpty() && leftConversation == rightConversation
        && !leftRequest.isEmpty() && leftRequest == rightRequest;
}

}

QJsonObject OllamaConversationRecord::toJson() const {
    const auto valueOrNull = [](const QJsonValue& value) {
        return value.isUndefined() ? QJsonValue(QJsonValue::Null) : value;
    };
    return QJsonObject{{QStringLiteral("conversationId"), conversationId},
                       {QStringLiteral("requestId"), requestId},
                       {QStringLiteral("roomId"), roomId},
                       {QStringLiteral("generation"), static_cast<qint64>(generation)},
                       {QStringLiteral("time"), time.isValid()
                                                      ? time.toUTC().toString(Qt::ISODateWithMs)
                                                      : QString()},
                       {QStringLiteral("model"), model},
                       {QStringLiteral("configuredModel"), configuredModel},
                       {QStringLiteral("resolvedModel"), resolvedModel},
                       {QStringLiteral("status"), status},
                       {QStringLiteral("failure"), failure},
                       {QStringLiteral("latencyMs"), latencyMs},
                       {QStringLiteral("messages"), messages},
                       {QStringLiteral("raw"), valueOrNull(raw)},
                       {QStringLiteral("parsed"), valueOrNull(parsed)},
                       {QStringLiteral("final"), valueOrNull(final)},
                       {QStringLiteral("fallback"), valueOrNull(fallback)}};
}

OllamaConversationStore::OllamaConversationStore(QString directory, Clock clock)
    : m_directory(QDir::cleanPath(directory.trimmed())), m_clock(std::move(clock)) {
    if (m_directory.isEmpty()) {
        m_configurationError = QStringLiteral("conversation store directory is empty");
        return;
    }
    if (!ensureDirectory(&m_configurationError)) return;
    cleanup();
}

QJsonValue OllamaConversationStore::redact(const QJsonValue& value) {
    return redactValue(value);
}

QJsonObject OllamaConversationStore::redact(const QJsonObject& value) {
    QJsonObject output;
    for (auto iterator = value.constBegin(); iterator != value.constEnd(); ++iterator) {
        output.insert(iterator.key(), sensitiveKey(iterator.key())
                                      ? QJsonValue(QStringLiteral("[REDACTED]"))
                                      : redactValue(iterator.value()));
    }
    return output;
}

QString OllamaConversationStore::filePathForDate(const QDateTime& time) const {
    return QDir(m_directory).filePath(kFilePrefix + currentUtc([time]() { return time; })
                                                    .date().toString(QStringLiteral("yyyy-MM-dd"))
                                      + QStringLiteral(".jsonl"));
}

bool OllamaConversationStore::ensureDirectory(QString* error) const {
    QFileInfo info(m_directory);
    if (info.exists()) {
        if (info.isSymLink() || !info.isDir()) {
            errorText(error, QStringLiteral("conversation store directory is not a directory"));
            return false;
        }
    } else if (!QDir().mkpath(m_directory)) {
        errorText(error, QStringLiteral("cannot create conversation store directory"));
        return false;
    }
    if (!QFile::setPermissions(m_directory,
                               QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                   | QFileDevice::ExeOwner)) {
        errorText(error, QStringLiteral("cannot set conversation store directory permissions"));
        return false;
    }
    return true;
}

bool OllamaConversationStore::safePath(const QString& path, bool allowMissing, QString* error) const {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (!pathIsUnder(m_directory, absolute)) {
        errorText(error, QStringLiteral("conversation store path escapes directory"));
        return false;
    }
    QFileInfo root(m_directory);
    if (root.isSymLink() || !root.isDir()) {
        errorText(error, QStringLiteral("conversation store directory is unsafe"));
        return false;
    }
    const QString relative = QDir(m_directory).relativeFilePath(absolute);
    const QStringList components = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString current = m_directory;
    for (int index = 0; index + 1 < components.size(); ++index) {
        current = QDir(current).filePath(components.at(index));
        const QFileInfo component(current);
        if (component.isSymLink() || (component.exists() && !component.isDir())) {
            errorText(error, QStringLiteral("conversation store parent path is unsafe"));
            return false;
        }
    }
    const QFileInfo target(absolute);
    if (target.isSymLink()) {
        errorText(error, QStringLiteral("conversation store path is a symbolic link"));
        return false;
    }
    if (!target.exists()) {
        if (allowMissing) return true;
        errorText(error, QStringLiteral("conversation store file is missing"));
        return false;
    }
    if (!target.isFile()) {
        errorText(error, QStringLiteral("conversation store path is not a regular file"));
        return false;
    }
    return true;
}

QJsonObject OllamaConversationStore::normalize(const QJsonObject& object, QString* error) const {
    QJsonObject output = object;
    const QJsonValue status = output.value(QStringLiteral("status"));
    if (!status.isString()
        || !QSet<QString>{QStringLiteral("completed"), QStringLiteral("rejected"),
                          QStringLiteral("failed"), QStringLiteral("cancelled")}
               .contains(status.toString())) {
        errorText(error, QStringLiteral("conversation store status is invalid"));
        return {};
    }
    for (const QString& name : {QStringLiteral("conversationId"), QStringLiteral("requestId"),
                                QStringLiteral("roomId"), QStringLiteral("model"),
                                QStringLiteral("configuredModel"),
                                QStringLiteral("resolvedModel"), QStringLiteral("failure")}) {
        if (!output.contains(name)) output.insert(name, QString());
        else if (!output.value(name).isString()) {
            errorText(error, QStringLiteral("conversation store field is not a string: %1").arg(name));
            return {};
        }
    }
    if (!output.contains(QStringLiteral("generation"))) output.insert(QStringLiteral("generation"), 0);
    const QJsonValue generation = output.value(QStringLiteral("generation"));
    if (!generation.isDouble() || generation.toDouble() < 0.0
        || generation.toDouble() > static_cast<double>(std::numeric_limits<quint64>::max())) {
        errorText(error, QStringLiteral("conversation store generation is invalid"));
        return {};
    }
    if (!output.contains(QStringLiteral("time")) || output.value(QStringLiteral("time")).toString().isEmpty()) {
        output.insert(QStringLiteral("time"), currentUtc(m_clock).toString(Qt::ISODateWithMs));
    } else {
        const QDateTime time = QDateTime::fromString(output.value(QStringLiteral("time")).toString(),
                                                      Qt::ISODateWithMs);
        if (!time.isValid()) {
            errorText(error, QStringLiteral("conversation store time is invalid"));
            return {};
        }
        output.insert(QStringLiteral("time"), time.toUTC().toString(Qt::ISODateWithMs));
    }
    if (!output.contains(QStringLiteral("latencyMs"))) output.insert(QStringLiteral("latencyMs"), 0);
    const QJsonValue latency = output.value(QStringLiteral("latencyMs"));
    if (!latency.isDouble() || latency.toDouble() < 0.0) {
        errorText(error, QStringLiteral("conversation store latency is invalid"));
        return {};
    }
    if (!output.contains(QStringLiteral("messages"))) output.insert(QStringLiteral("messages"), QJsonArray());
    if (!output.value(QStringLiteral("messages")).isArray()) {
        errorText(error, QStringLiteral("conversation store messages are invalid"));
        return {};
    }
    for (const QString& name : {QStringLiteral("raw"), QStringLiteral("parsed"),
                                QStringLiteral("final"), QStringLiteral("fallback")}) {
        if (!output.contains(name)) output.insert(name, QJsonValue(QJsonValue::Null));
    }
    return output;
}

QStringList OllamaConversationStore::storeFilePaths(QString* error) const {
    QStringList paths;
    QDir directory(m_directory);
    const QFileInfoList files = directory.entryInfoList(
        QStringList{kFilePrefix + QStringLiteral("*.jsonl")}, QDir::Files | QDir::Hidden,
        QDir::Name);
    for (const QFileInfo& info : files) {
        QString pathError;
        if (!safePath(info.absoluteFilePath(), false, &pathError)) {
            if (error && error->isEmpty()) *error = pathError;
            continue;
        }
        paths.append(info.absoluteFilePath());
    }
    return paths;
}

QString OllamaConversationStore::nextAppendPath(const QDateTime& time, qint64 lineBytes,
                                                QString* error) const {
    const QString base = filePathForDate(time);
    if (!safePath(base, true, error)) return {};
    const QFileInfo baseInfo(base);
    const qint64 baseBoundaryBytes = appendBoundaryBytes(base, error);
    if (baseBoundaryBytes < 0) return {};
    if (!baseInfo.exists()
        || baseInfo.size() + baseBoundaryBytes + lineBytes <= kMaxFileBytes) {
        return base;
    }
    for (int index = 1; index <= 9999; ++index) {
        const QString candidate = QDir(m_directory).filePath(
            kFilePrefix + currentUtc([time]() { return time; }).date().toString(QStringLiteral("yyyy-MM-dd"))
            + QStringLiteral("-%1.jsonl").arg(index, 3, 10, QLatin1Char('0')));
        if (!safePath(candidate, true, error)) return {};
        const QFileInfo info(candidate);
        const qint64 boundaryBytes = appendBoundaryBytes(candidate, error);
        if (boundaryBytes < 0) return {};
        if (!info.exists() || info.size() + boundaryBytes + lineBytes <= kMaxFileBytes) {
            return candidate;
        }
    }
    errorText(error, QStringLiteral("conversation store has no rollover file available"));
    return {};
}

bool OllamaConversationStore::appendObject(const QJsonObject& object, QString* error) {
    if (!ensureDirectory(error)) return false;
    const QDateTime now = currentUtc(m_clock);
    if (!m_lastCleanup.isValid() || m_lastCleanup.secsTo(now) >= 3600) cleanup();
    const QJsonObject normalized = normalize(object, error);
    if (normalized.isEmpty()) return false;
    QString existingError;
    const QVector<QJsonObject> existing = readRecords(&existingError);
    if (!existingError.isEmpty()) {
        errorText(error, existingError);
        return false;
    }
    for (const QJsonObject& record : existing) {
        if (sameFinalRequest(record, normalized)) return true;
    }
    const QByteArray line = QJsonDocument(redact(normalized)).toJson(QJsonDocument::Compact) + '\n';
    if (line.size() > kMaxRecordBytes) {
        errorText(error, QStringLiteral("conversation store record is too large"));
        return false;
    }
    const QString path = nextAppendPath(now, line.size(), error);
    if (path.isEmpty()) return false;
    const qint64 boundaryBytes = appendBoundaryBytes(path, error);
    if (boundaryBytes < 0) return false;
    if (qEnvironmentVariableIntValue("WARGAME_FORCE_CONVERSATION_STORE_WRITE_FAILURE") != 0) {
        errorText(error, QStringLiteral("conversation store write failure"));
        return false;
    }
    QFile file(path);
    const qint64 originalSize = QFileInfo(path).exists() ? QFileInfo(path).size() : 0;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        errorText(error, QStringLiteral("cannot open conversation store file"));
        return false;
    }
    if (!QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.close();
        errorText(error, QStringLiteral("cannot set conversation store file permissions"));
        return false;
    }
    QByteArray payload;
    payload.reserve(line.size() + static_cast<int>(boundaryBytes));
    if (boundaryBytes != 0) payload.append('\n');
    payload.append(line);
    if (file.write(payload) != payload.size()) {
        file.resize(originalSize);
        file.close();
        errorText(error, QStringLiteral("conversation store write failure"));
        return false;
    }
    if (!durableFlush(file, error)) {
        file.resize(originalSize);
        file.close();
        return false;
    }
    file.close();
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    m_lastCleanup = now;
    cleanup();
    return true;
}

bool OllamaConversationStore::appendFinalRecord(const OllamaConversationRecord& record,
                                                QString* error) {
    return appendObject(record.toJson(), error);
}

bool OllamaConversationStore::appendFinalRecord(const QJsonObject& record, QString* error) {
    return appendObject(record, error);
}

QVector<QJsonObject> OllamaConversationStore::readRecords(QString* error) const {
    QVector<QJsonObject> records;
    if (!ensureDirectory(error)) return records;
    const QStringList paths = storeFilePaths(error);
    for (const QString& path : paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine().trimmed();
            if (line.isEmpty()) continue;
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                records.append(redact(document.object()));
            }
        }
    }
    return records;
}

bool OllamaConversationStore::cleanup(QString* error) {
    if (!ensureDirectory(error)) return false;
    const QDateTime cutoff = currentUtc(m_clock).addDays(-kRetentionDays);
    QStringList paths = storeFilePaths(error);
    for (const QString& path : paths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        QVector<QByteArray> kept;
        bool changed = false;
        while (!file.atEnd()) {
            const QByteArray line = file.readLine();
            const QByteArray trimmed = line.trimmed();
            if (trimmed.isEmpty()) continue;
            QJsonParseError parseError;
            const QJsonDocument document = QJsonDocument::fromJson(trimmed, &parseError);
            if (parseError.error == QJsonParseError::NoError && document.isObject()) {
                if (keepByAge(document.object(), cutoff, path)) kept.append(line.endsWith('\n') ? line : line + '\n');
                else changed = true;
            } else {
                const QDate date = QDate::fromString(datePart(path), QStringLiteral("yyyy-MM-dd"));
                if (date.isValid() && QDateTime(date, QTime(0, 0), QTimeZone::utc()).addDays(1) <= cutoff) changed = true;
                else kept.append(line.endsWith('\n') ? line : line + '\n');
            }
        }
        file.close();
        if (!changed) continue;
        if (kept.isEmpty()) {
            QFile::remove(path);
            continue;
        }
        QSaveFile replacement(path);
        if (!replacement.open(QIODevice::WriteOnly)) continue;
        const QByteArray content = [&kept]() {
            QByteArray bytes;
            for (const QByteArray& line : kept) bytes += line;
            return bytes;
        }();
        if (replacement.write(content) != content.size() || !durableFlush(replacement, nullptr)
            || !replacement.commit()) {
            replacement.cancelWriting();
            continue;
        }
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    paths = storeFilePaths(nullptr);
    struct FileAge {
        QString path;
        qint64 size = 0;
        QDateTime modified;
    };
    QVector<FileAge> files;
    qint64 total = 0;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        files.append(FileAge{path, info.size(), info.lastModified().toUTC()});
        total += info.size();
    }
    std::sort(files.begin(), files.end(), [](const FileAge& left, const FileAge& right) {
        if (left.modified != right.modified) return left.modified < right.modified;
        return left.path < right.path;
    });
    for (const FileAge& file : files) {
        if (total <= kMaxStoreBytes) break;
        if (QFile::remove(file.path)) total -= file.size;
    }
    m_lastCleanup = currentUtc(m_clock);
    return true;
}

}
