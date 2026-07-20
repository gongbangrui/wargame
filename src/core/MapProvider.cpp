#include "MapProvider.h"

#include <algorithm>

namespace gbr {

MapProvider::MapProvider() {
    m_center = GeoPos{m_widthM / 2.0, m_heightM / 2.0, 0.0};
    m_origin = GeoCoord{25.40, 119.30};
    m_mercatorOrigin = Mercator::latLonToMeters(m_origin.lat, m_origin.lon);
}

void MapProvider::setLogicalSizeMeters(double w, double h) {
    if (!std::isfinite(w)) w = 1.0;
    if (!std::isfinite(h)) h = 1.0;
    m_widthM = std::max(1.0, w);
    m_heightM = std::max(1.0, h);
    m_center = GeoPos{m_widthM / 2.0, m_heightM / 2.0, 0.0};
    m_mercatorOrigin = Mercator::latLonToMeters(m_origin.lat, m_origin.lon);
}

void MapProvider::setOrigin(const GeoCoord& c) {
    m_origin = c;
    m_mercatorOrigin = Mercator::latLonToMeters(m_origin.lat, m_origin.lon);
}

QPointF MapProvider::toPixel(double viewportW, double viewportH, const GeoPos& logical) const {
    const double baseScale = std::min(viewportW / m_widthM, viewportH / m_heightM);
    const double scale = baseScale * m_zoom;
    const double cx = viewportW * 0.5;
    const double cy = viewportH * 0.5;
    const double dx = (logical.x - m_center.x) * scale;
    const double dy = (m_center.y - logical.y) * scale;
    return QPointF{cx + dx, cy + dy};
}

GeoPos MapProvider::fromPixel(double viewportW, double viewportH, const QPointF& px) const {
    const double baseScale = std::min(viewportW / m_widthM, viewportH / m_heightM);
    const double scale = baseScale * m_zoom;
    if (scale <= 0.0) return m_center;
    const double dx = px.x() - viewportW * 0.5;
    const double dy = px.y() - viewportH * 0.5;
    return GeoPos{m_center.x + dx / scale, m_center.y - dy / scale, 0.0};
}

double MapProvider::radiusToPixels(double viewportW, double viewportH, double radiusMeters) const {
    const double baseScale = std::min(viewportW / m_widthM, viewportH / m_heightM);
    return radiusMeters * baseScale * m_zoom;
}

QJsonObject MapProvider::describe() const {
    QJsonObject o;
    o["name"] = m_name;
    o["widthMeters"] = m_widthM;
    o["heightMeters"] = m_heightM;
    o["originLat"] = m_origin.lat;
    o["originLon"] = m_origin.lon;
    o["tileZoom"] = m_tileZoom;
    return o;
}

QPointF MapProvider::toMercator(const GeoPos& logical) const {
    return QPointF(m_mercatorOrigin.x() + logical.x,
                   m_mercatorOrigin.y() + logical.y);
}

} // namespace gbr
