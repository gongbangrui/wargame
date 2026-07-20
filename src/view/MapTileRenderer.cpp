#include "MapTileRenderer.h"
#include "../core/Geo.h"
#include "../core/TileCacheLocator.h"
#include <QPainter>
#include <QDebug>
#include <QMutexLocker>
#include <algorithm>
#include <cmath>
#include <limits>

namespace gbr {

MapTileRenderer::MapTileRenderer(QQuickItem* parent)
    : QQuickPaintedItem(parent) {
    m_cacheDir = TileCacheLocator::resolve();
    if (m_cacheDir.isEmpty()) {
        m_cacheDir.clear();
        qInfo() << "MapTileRenderer: No tile cache directory found, GIS map will be blank.";
    } else {
        qInfo() << "MapTileRenderer: tile cache =" << m_cacheDir;
    }

    auto origin = Mercator::latLonToMeters(m_originLat, m_originLon);
    m_mercatorOrigin = QPointF(origin.x(), origin.y());
    setRenderTarget(QQuickPaintedItem::FramebufferObject);
    setAntialiasing(false);
    setOpaquePainting(true);
    m_placeholder = QImage(256, 256, QImage::Format_ARGB32);
    m_placeholder.fill(QColor(10, 14, 24));
    // QCache cost units are KiB; decoded tiles, not compressed file sizes,
    // determine the actual rendering memory footprint.
    m_tileCache.setMaxCost(64 * 1024);
}

void MapTileRenderer::setCenterX(double v) { if (m_centerX != v) { m_centerX = v; update(); emit centerChanged(); } }
void MapTileRenderer::setCenterY(double v) { if (m_centerY != v) { m_centerY = v; update(); emit centerChanged(); } }
void MapTileRenderer::setZoom(double v) { v = std::max(0.001, v); if (m_zoom != v) { m_zoom = v; update(); emit centerChanged(); } }
void MapTileRenderer::setOriginLon(double v) { m_originLon = v; updateOrigin(); }
void MapTileRenderer::setOriginLat(double v) { m_originLat = v; updateOrigin(); }
void MapTileRenderer::setTileZoom(int v) { v = std::clamp(v, Mercator::kMinZoom, Mercator::kMaxZoom); if (m_tileZoom != v) { m_tileZoom = v; update(); emit tileZoomChanged(); } }
QString MapTileRenderer::tileCacheDir() const {
    QMutexLocker lock(&m_tileCacheMutex);
    return m_cacheDir;
}

void MapTileRenderer::setTileCacheDir(const QString& d) {
    const QString resolved = TileCacheLocator::resolve(d);
    {
        QMutexLocker lock(&m_tileCacheMutex);
        if (m_cacheDir == resolved) return;
        m_cacheDir = resolved;
        m_tileCache.clear();
    }
    update();
    emit tileCacheDirChanged();
}

void MapTileRenderer::updateOrigin() {
    auto origin = Mercator::latLonToMeters(m_originLat, m_originLon);
    m_mercatorOrigin = QPointF(origin.x(), origin.y());
    emit originChanged();
    update();
}

QPointF MapTileRenderer::mercatorFromSim(double sx, double sy) const {
    return { m_mercatorOrigin.x() + sx, m_mercatorOrigin.y() + sy };
}

QPointF MapTileRenderer::simFromMercator(double mx, double my) const {
    return { mx - m_mercatorOrigin.x(), my - m_mercatorOrigin.y() };
}

QPointF MapTileRenderer::simToScreen(double sx, double sy) const {
    const double cx = width() * 0.5;
    const double cy = height() * 0.5;
    return { cx + (sx - m_centerX) * m_zoom, cy - (sy - m_centerY) * m_zoom };
}

QPointF MapTileRenderer::screenToSim(double px, double py) const {
    const double cx = width() * 0.5;
    const double cy = height() * 0.5;
    return { m_centerX + (px - cx) / m_zoom, m_centerY - (py - cy) / m_zoom };
}

QImage MapTileRenderer::loadTile(int z, int x, int y, bool& found) const {
    QString cacheDir;
    {
        QMutexLocker lock(&m_tileCacheMutex);
        cacheDir = m_cacheDir;
    }
    if (cacheDir.isEmpty()) {
        found = false;
        return m_placeholder;
    }

    const QString relativePath = QStringLiteral("%1/%2/%3.png").arg(z).arg(x).arg(y);
    const QString path = cacheDir + QLatin1Char('/') + relativePath;
    {
        QMutexLocker lock(&m_tileCacheMutex);
        if (const QImage* cached = m_tileCache.object(path)) {
            found = true;
            return *cached;
        }
    }

    QImage tile(path);
    if (tile.isNull()) {
        found = false;
        return m_placeholder;
    }

    const qsizetype sizeKiB = std::max<qsizetype>(1, (tile.sizeInBytes() + 1023) / 1024);
    {
        QMutexLocker lock(&m_tileCacheMutex);
        m_tileCache.insert(path, new QImage(tile),
                           static_cast<int>(std::min<qsizetype>(
                               sizeKiB, std::numeric_limits<int>::max())));
    }
    found = true;
    return tile;
}

int MapTileRenderer::renderTiles(QPainter* painter, int zoom) {
    int realTilesFound = 0;

    const int shift = Mercator::safeZoomShift(zoom);
    const double res = Mercator::kInitialRes / shift;
    const double tileWorld = 256.0 * res;
    const double margin = tileWorld * 1.5;

    auto tlSim = screenToSim(0, 0);
    auto brSim = screenToSim(width(), height());
    double vpLeft = std::min(tlSim.x(), brSim.x()) - margin;
    double vpRight = std::max(tlSim.x(), brSim.x()) + margin;
    double vpBottom = std::min(tlSim.y(), brSim.y()) - margin;
    double vpTop = std::max(tlSim.y(), brSim.y()) + margin;

    auto vpLeftM = mercatorFromSim(vpLeft, vpBottom);
    auto vpRightM = mercatorFromSim(vpRight, vpTop);

    int tileMinX = std::max(0, (int)std::floor((vpLeftM.x() + Mercator::kOriginShift) / tileWorld));
    int tileMaxX = std::min(shift - 1, (int)std::floor((vpRightM.x() + Mercator::kOriginShift) / tileWorld));
    int tileMinY = std::max(0, (int)std::floor((Mercator::kOriginShift - vpRightM.y()) / tileWorld));
    int tileMaxY = std::min(shift - 1, (int)std::floor((Mercator::kOriginShift - vpLeftM.y()) / tileWorld));

    const int maxTiles = 144;
    int drawn = 0;

    for (int tx = tileMinX; tx <= tileMaxX && drawn < maxTiles; tx++) {
        for (int ty = tileMinY; ty <= tileMaxY && drawn < maxTiles; ty++) {
            double tileMX = tx * tileWorld - Mercator::kOriginShift;
            double tileMY = Mercator::kOriginShift - ty * tileWorld;
            double tileMY_bottom = tileMY - tileWorld;

            double tileSimX = tileMX - m_mercatorOrigin.x();
            double tileSimY = tileMY - m_mercatorOrigin.y();
            double tileSimY_bottom = tileMY_bottom - m_mercatorOrigin.y();

            auto screenTopLeft = simToScreen(tileSimX, tileSimY);
            auto screenBottomRight = simToScreen(tileSimX + tileWorld, tileSimY_bottom);

            QRectF targetRect(screenTopLeft, screenBottomRight);
            targetRect = targetRect.normalized();

            if (targetRect.right() < 0.0 || targetRect.left() > width() ||
                targetRect.bottom() < 0.0 || targetRect.top() > height())
                continue;

            bool found = false;
            QImage tile = loadTile(zoom, tx, ty, found);
            painter->drawImage(targetRect, tile);
            if (found) realTilesFound++;
            drawn++;
        }
    }

    return realTilesFound;
}

void MapTileRenderer::paint(QPainter* painter) {
    painter->fillRect(QRect(0, 0, (int)width(), (int)height()), QColor(8, 11, 20));

    if (tileCacheDir().isEmpty()) return;

    int realTiles = renderTiles(painter, m_tileZoom);

    // Fallback: if no tiles found at current zoom, retry with zoom 12
    // (the only zoom level with guaranteed correct tile data on disk)
    if (realTiles == 0 && m_tileZoom != 12) {
        painter->fillRect(QRect(0, 0, (int)width(), (int)height()), QColor(8, 11, 20));
        renderTiles(painter, 12);
    }
}

} // namespace gbr
