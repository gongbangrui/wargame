#include "TileCacheLocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace gbr {

QString TileCacheLocator::normalizedUsableDirectory(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) return {};

    const QFileInfo info(trimmed);
    if (!info.isDir()) return {};

    const QDir directory(info.absoluteFilePath());
    if (!QDir(directory.filePath(QStringLiteral("12"))).exists()
        || !QFileInfo(directory.filePath(QStringLiteral("metadata.json"))).isFile()
        || !QFileInfo(directory.filePath(QStringLiteral("tilejson.json"))).isFile()) {
        return {};
    }
    return directory.canonicalPath();
}

bool TileCacheLocator::isUsableMapDirectory(const QString& path) {
    return !normalizedUsableDirectory(path).isEmpty();
}

QString TileCacheLocator::resolve(const QString& explicitDir) {
    if (!explicitDir.trimmed().isEmpty()) {
        return normalizedUsableDirectory(explicitDir);
    }

    if (qEnvironmentVariableIsSet("WARGAME_MAP_DIR")) {
        return normalizedUsableDirectory(qEnvironmentVariable("WARGAME_MAP_DIR"));
    }

    return normalizedUsableDirectory(
        QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("map")));
}

} // namespace gbr
