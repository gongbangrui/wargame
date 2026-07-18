#include "server/PersistenceStore.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QTextStream>

#include <csignal>
#include <sys/resource.h>

using namespace gbr;

namespace {

bool report(const QString& mode, bool passed, const QString& detail) {
    QTextStream(stdout) << QJsonDocument(QJsonObject{
        {QStringLiteral("mode"), mode},
        {QStringLiteral("passed"), passed},
        {QStringLiteral("detail"), detail},
    }).toJson(QJsonDocument::Compact) << Qt::endl;
    return passed;
}

QJsonObject checkpoint(const QString& value) {
    return {{QStringLiteral("checkpointVersion"), 1},
            {QStringLiteral("value"), value}};
}

bool runReadOnly() {
    QTemporaryDir directory;
    if (!directory.isValid()) return report(QStringLiteral("read-only"), false, QStringLiteral("临时目录无效"));
    const QString databasePath = directory.filePath(QStringLiteral("readonly.sqlite3"));
    QString error;
    {
        PersistenceStore store;
        if (!store.open(databasePath, &error)
            || !store.saveCheckpoint(QStringLiteral("main"), 1, 1, checkpoint(QStringLiteral("before")), &error)) {
            return report(QStringLiteral("read-only"), false, error);
        }
    }
    const QFileDevice::Permissions fileReadOnly = QFileDevice::ReadOwner
        | QFileDevice::ReadGroup | QFileDevice::ReadOther;
    const QFileDevice::Permissions dirReadOnly = QFileDevice::ReadOwner | QFileDevice::ExeOwner
        | QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther;
    if (!QFile::setPermissions(databasePath, fileReadOnly)
        || !QFile::setPermissions(directory.path(), dirReadOnly)) {
        return report(QStringLiteral("read-only"), false, QStringLiteral("无法设置只读权限"));
    }
    PersistenceStore readOnlyStore;
    const bool rejected = !readOnlyStore.open(databasePath, &error);
    QFile::setPermissions(directory.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(databasePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return report(QStringLiteral("read-only"), rejected,
                  rejected ? error : QStringLiteral("只读数据库意外允许写入"));
}

bool runDiskFull() {
    QTemporaryDir directory;
    if (!directory.isValid()) return report(QStringLiteral("disk-full"), false, QStringLiteral("临时目录无效"));
    struct rlimit original {};
    if (getrlimit(RLIMIT_FSIZE, &original) != 0) {
        return report(QStringLiteral("disk-full"), false, QStringLiteral("无法读取文件大小限制"));
    }
    struct rlimit limit = original;
    limit.rlim_cur = std::min<rlim_t>(original.rlim_max, 512 * 1024);
    if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
        return report(QStringLiteral("disk-full"), false, QStringLiteral("无法设置文件大小限制"));
    }
    std::signal(SIGXFSZ, SIG_IGN);
    QString error;
    PersistenceStore store;
    bool rejected = false;
    if (store.open(directory.filePath(QStringLiteral("full.sqlite3")), &error)) {
        QString payload;
        payload.resize(2 * 1024 * 1024);
        for (QChar& character : payload) {
            character = QChar(u'a' + QRandomGenerator::global()->bounded(26));
        }
        rejected = !store.saveCheckpoint(QStringLiteral("main"), 1, 1,
                                         checkpoint(payload), &error);
    }
    setrlimit(RLIMIT_FSIZE, &original);
    return report(QStringLiteral("disk-full"), rejected,
                  rejected ? error : QStringLiteral("文件配额耗尽后写入仍成功"));
}

bool runBackupRestore() {
    QTemporaryDir directory;
    if (!directory.isValid()) return report(QStringLiteral("backup-restore"), false, QStringLiteral("临时目录无效"));
    const QString databasePath = directory.filePath(QStringLiteral("data.sqlite3"));
    const QString backupPath = directory.filePath(QStringLiteral("backup.sqlite3"));
    QString error;
    {
        PersistenceStore store;
        if (!store.open(databasePath, &error)
            || !store.saveCheckpoint(QStringLiteral("main"), 1, 10, checkpoint(QStringLiteral("before")), &error)
            || !store.backupTo(backupPath, &error)
            || !store.saveCheckpoint(QStringLiteral("main"), 2, 20, checkpoint(QStringLiteral("after")), &error)) {
            return report(QStringLiteral("backup-restore"), false, error);
        }
    }
    QFile::remove(databasePath);
    QFile::remove(databasePath + QStringLiteral("-wal"));
    QFile::remove(databasePath + QStringLiteral("-shm"));
    if (!QFile::copy(backupPath, databasePath)) {
        return report(QStringLiteral("backup-restore"), false, QStringLiteral("无法替换备份数据库"));
    }
    PersistenceStore restored;
    if (!restored.open(databasePath, &error)) return report(QStringLiteral("backup-restore"), false, error);
    qint64 revision = 0;
    qint64 tick = 0;
    const QJsonObject restoredCheckpoint = restored.loadLatestCheckpoint(QStringLiteral("main"), &revision, &tick, &error);
    const bool passed = error.isEmpty() && revision == 1 && tick == 10
        && restoredCheckpoint == checkpoint(QStringLiteral("before"));
    return report(QStringLiteral("backup-restore"), passed,
                  passed ? QStringLiteral("备份恢复一致") : error);
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("mode"), QStringLiteral("read-only、disk-full 或 backup-restore"),
                      QStringLiteral("name")});
    parser.process(application);
    const QString mode = parser.value(QStringLiteral("mode"));
    if (mode == QLatin1String("read-only")) return runReadOnly() ? 0 : 1;
    if (mode == QLatin1String("disk-full")) return runDiskFull() ? 0 : 1;
    if (mode == QLatin1String("backup-restore")) return runBackupRestore() ? 0 : 1;
    QTextStream(stderr) << QStringLiteral("--mode 必须是 read-only、disk-full 或 backup-restore") << Qt::endl;
    return 2;
}
