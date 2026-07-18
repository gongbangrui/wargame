#pragma once

#include <QQuickPaintedItem>
#include <QImage>
#include <QCache>
#include <QMutex>
#include <QPointF>
#include <QString>

namespace gbr {

/// @brief QML item that renders map tiles from a local disk cache.
class MapTileRenderer : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(double centerX READ centerX WRITE setCenterX NOTIFY centerChanged)
    Q_PROPERTY(double centerY READ centerY WRITE setCenterY NOTIFY centerChanged)
    Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY centerChanged)
    Q_PROPERTY(double originLon READ originLon WRITE setOriginLon NOTIFY originChanged)
    Q_PROPERTY(double originLat READ originLat WRITE setOriginLat NOTIFY originChanged)
    Q_PROPERTY(int tileZoom READ tileZoom WRITE setTileZoom NOTIFY tileZoomChanged)
    Q_PROPERTY(QString tileCacheDir READ tileCacheDir WRITE setTileCacheDir NOTIFY tileCacheDirChanged)
public:
    explicit MapTileRenderer(QQuickItem* parent = nullptr);

    double centerX() const { return m_centerX; }
    void setCenterX(double v);
    double centerY() const { return m_centerY; }
    void setCenterY(double v);
    double zoom() const { return m_zoom; }
    void setZoom(double v);
    double originLon() const { return m_originLon; }
    void setOriginLon(double v);
    double originLat() const { return m_originLat; }
    void setOriginLat(double v);
    int tileZoom() const { return m_tileZoom; }
    void setTileZoom(int v);
    QString tileCacheDir() const;
    void setTileCacheDir(const QString& d);
    void setCacheDir(const QString& d) { setTileCacheDir(d); }

    Q_INVOKABLE QPointF simToScreen(double sx, double sy) const;
    Q_INVOKABLE QPointF screenToSim(double px, double py) const;

    void paint(QPainter* painter) override;

signals:
    void centerChanged();
    void originChanged();
    void tileZoomChanged();
    void tileCacheDirChanged();

private:
    QImage loadTile(int z, int x, int y, bool& found) const;
    int renderTiles(QPainter* painter, int zoom);
    QPointF mercatorFromSim(double sx, double sy) const;
    QPointF simFromMercator(double mx, double my) const;
    void updateOrigin();

    double m_centerX = 10000;
    double m_centerY = 7500;
    double m_zoom = 1.0;
    double m_originLon = 119.30;
    double m_originLat = 25.40;
    int m_tileZoom = 12;
    QString m_cacheDir;
    QPointF m_mercatorOrigin;
    QImage m_placeholder;
    mutable QCache<QString, QImage> m_tileCache;
    mutable QMutex m_tileCacheMutex;
};

} // namespace gbr
