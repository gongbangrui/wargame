#include "TileCacheLocator.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <algorithm>

namespace gbr {

QString TileCacheLocator::normalizedUsableDirectory(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) return {};

    const QFileInfo info(trimmed);
    if (!info.isDir()) return {};

    const QDir directory(info.absoluteFilePath());
    const QStringList zoomDirectories = directory.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    const bool hasZoomDirectory = std::any_of(
        zoomDirectories.cbegin(), zoomDirectories.cend(), [](const QString& name) {
            bool ok = false;
            const int zoom = name.toInt(&ok);
            return ok && zoom >= 0 && zoom <= 22;
        });
    if (!hasZoomDirectory
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
