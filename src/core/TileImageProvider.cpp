#include "TileImageProvider.h"
#include "TileCacheLocator.h"
#include <QPainter>
#include <QMutexLocker>
#include <QRegularExpression>

namespace gbr {

TileImageProvider::TileImageProvider(const QString& cacheDir)
    : QQuickImageProvider(QQuickImageProvider::Image)
    , m_cacheDir(TileCacheLocator::resolve(cacheDir)) {
    // Pregenerate dark placeholder for missing tiles
    m_placeholderTile = QImage(256, 256, QImage::Format_ARGB32);
    m_placeholderTile.fill(QColor(10, 14, 24));
    QPainter p(&m_placeholderTile);
    p.setPen(QColor(30, 45, 74));
    p.drawRect(0, 0, 255, 255);
    p.end();
}

void TileImageProvider::setCacheDirectory(const QString& dir) {
    const QString resolved = TileCacheLocator::resolve(dir);
    QMutexLocker lock(&m_cacheDirMutex);
    m_cacheDir = resolved;
}

QString TileImageProvider::cacheDirectory() const {
    QMutexLocker lock(&m_cacheDirMutex);
    return m_cacheDir;
}

QImage TileImageProvider::requestImage(const QString& id, QSize* size, const QSize& /*requestedSize*/) {
    static const QRegularExpression kTilePattern(QStringLiteral("^\\d+/\\d+/\\d+$"));
    if (!kTilePattern.match(id).hasMatch()) {
        if (size) *size = QSize(256, 256);
        return m_placeholderTile;
    }
    const QString cacheDir = cacheDirectory();
    if (cacheDir.isEmpty()) {
        if (size) *size = QSize(256, 256);
        return m_placeholderTile;
    }
    const QString path = cacheDir + QLatin1Char('/') + id + QStringLiteral(".png");

    QImage img(path);
    if (img.isNull()) {
        if (size) *size = QSize(256, 256);
        return m_placeholderTile;
    }

    if (size) *size = img.size();
    return img;
}

} // namespace gbr
