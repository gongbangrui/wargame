#pragma once

#include "Geo.h"
#include <QString>
#include <QRectF>
#include <QJsonObject>
#include <QJsonArray>

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

    QPointF toPixel(double viewportW, double viewportH, const GeoPos& logical) const;
    GeoPos fromPixel(double viewportW, double viewportH, const QPointF& px) const;
    double radiusToPixels(double viewportW, double viewportH, double radiusMeters) const;

    QJsonObject describe() const;

private:
    double m_widthM = 40000;
    double m_heightM = 30000;
    double m_zoom = 1.0;
    QString m_name;
    GeoPos m_center;
};

} // namespace gbr

