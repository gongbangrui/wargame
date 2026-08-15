#pragma once

#include "Geo.h"
#include <QString>
#include <QRectF>
#include <QJsonObject>
#include <QJsonArray>
#include <QPointF>

namespace gbr {

class MapProvider {
public:
    MapProvider();

    void setLogicalSizeMeters(double w, double h);
    double widthMeters() const { return m_widthM; }
    double heightMeters() const { return m_heightM; }

    void setName(const QString& name) { m_name = name; }
    QString name() const { return m_name; }

    void setZoom(double z) { m_zoom = z; }
    double zoom() const { return m_zoom; }

    void setCenter(const GeoPos& c) { m_center = c; }
    GeoPos center() const { return m_center; }

    void setOrigin(const GeoCoord& c);
    GeoCoord origin() const { return m_origin; }

    bool loadMetadataFile(const QString& path, QString* error = nullptr);
    bool applyMetadata(const QJsonObject& metadata, QString* error = nullptr);
    bool contains(const GeoPos& logical) const;
    GeoPos clampToExtent(const GeoPos& logical) const;

    QPointF toPixel(double viewportW, double viewportH, const GeoPos& logical) const;
    GeoPos fromPixel(double viewportW, double viewportH, const QPointF& px) const;
    double radiusToPixels(double viewportW, double viewportH, double radiusMeters) const;

    /// @brief Convert simulation logical coordinates (meters) to Mercator meters.
    QPointF toMercator(const GeoPos& logical) const;

    /// @brief Tile zoom level for current map view.
    int tileZoomForView() const { return m_tileZoom; }
    void setTileZoom(int z) { m_tileZoom = z; }
    int minTileZoom() const { return m_minTileZoom; }
    int maxTileZoom() const { return m_maxTileZoom; }
    int metadataRevision() const { return m_metadataRevision; }

    QJsonObject describe() const;

private:
    double m_widthM = 20000;
    double m_heightM = 15000;
    double m_zoom = 1.0;
    QString m_name;
    GeoPos m_center;
    GeoCoord m_origin;
    int m_tileZoom = 12;
    int m_minTileZoom = 12;
    int m_maxTileZoom = 12;
    int m_metadataRevision = 0;
    QPointF m_mercatorOrigin;
};

} // namespace gbr
