#pragma once

#include "MessageBus.h"
#include <QObject>
#include <QString>
#include <QFile>
#include <memory>
#include <atomic>

namespace gbr {

/// @brief Optional append-only message log for replay/catch-up after reconnect.
/// @details Disabled by default (zero IO). Enable via setEnabled(true, path).
/// Records each emitted JSON message to disk as one line of compact JSON
/// (newline-delimited). On reconnect a peer can read the log to catch up
/// to current state. Note: the file is opened in Append mode; truncated
/// writes (last partial line) may occur on crash — consumers should skip
/// lines that don't parse.
class MessageLogRecorder : public QObject {
    Q_OBJECT
public:
    explicit MessageLogRecorder(QObject* parent = nullptr);
    ~MessageLogRecorder() override;

    /// @brief Enable logging and open @p path for append.
    /// @return false when enabling fails; disabling always returns true.
    bool setEnabled(bool enabled, const QString& path = QString());
    bool isEnabled() const { return m_enabled; }
    QString path() const { return m_path; }

    /// @brief Called by the engine for every emitted message. Cheap when disabled.
    void record(const QJsonObject& msg);

private:
    std::atomic<bool> m_enabled{false};
    QString m_path;
    QFile m_file;
};

} // namespace gbr
