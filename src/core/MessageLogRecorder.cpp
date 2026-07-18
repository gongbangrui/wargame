#include "MessageLogRecorder.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>

namespace gbr {

MessageLogRecorder::MessageLogRecorder(QObject* parent) : QObject(parent) {}

MessageLogRecorder::~MessageLogRecorder() {
    if (m_file.isOpen()) m_file.close();
}

bool MessageLogRecorder::setEnabled(bool enabled, const QString& path) {
    if (!enabled) {
        m_enabled.store(false);
        if (m_file.isOpen()) m_file.close();
        return true;
    }
    if (path.isEmpty()) {
        m_enabled.store(false);
        if (m_file.isOpen()) m_file.close();
        m_path.clear();
        return false;
    }
    if (m_enabled.load() && m_file.isOpen() && m_path == path) return true;

    m_enabled.store(false);
    if (m_file.isOpen()) m_file.close();
    m_path = path;
    const QString parentDir = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parentDir)) return false;
    m_file.setFileName(path);
    if (!m_file.open(QIODevice::Append | QIODevice::WriteOnly)) return false;
    m_enabled.store(true);
    return true;
}

void MessageLogRecorder::record(const QJsonObject& msg) {
    if (!m_enabled.load()) return;
    if (!m_file.isOpen()) return;
    const QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    m_file.write(line);
    m_file.write("\n");
    // Don't flush() every line — Qt buffers; flushing on shutdown is fine.
}

} // namespace gbr
