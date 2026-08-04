#pragma once

#include <QPointF>
#include <QSizeF>
#include <algorithm>
#include <cmath>

namespace gbr {

struct GeoPos {
    double x = 0.0;
    double y = 0.0;
    double alt = 0.0;

    QPointF toPointF() const { return QPointF(x, y); }
    static GeoPos fromPointF(const QPointF& p, double altitude = 0.0) {
        return GeoPos{p.x(), p.y(), altitude};
    }

    /// @brief 3D Euclidean distance. Used only where altitude matters.
    double distanceTo(const GeoPos& other) const {
        const double dx = x - other.x;
        const double dy = y - other.y;
        const double dz = alt - other.alt;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    /// @brief 2D planar distance (xy only). Used for detectRange/attackRange
    /// /commRange semantics — altitude does not count.
    double distanceTo2D(const GeoPos& other) const {
        const double dx = x - other.x;
        const double dy = y - other.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    GeoPos lerp(const GeoPos& target, double t) const {
        return GeoPos{
            x + (target.x - x) * t,
            y + (target.y - y) * t,
            alt + (target.alt - alt) * t
        };
    }
};

/// @brief Geographic coordinate (latitude/longitude in degrees).
struct GeoCoord {
    double lat = 0.0;
    double lon = 0.0;
};

namespace MapCoordinates {

inline QPointF logicalToScreen(const GeoPos& logical, const GeoPos& center,
                               const QSizeF& viewport, double pixelsPerMeter) {
    if (!std::isfinite(viewport.width()) || !std::isfinite(viewport.height())
        || viewport.width() <= 0.0 || viewport.height() <= 0.0
        || !std::isfinite(pixelsPerMeter) || pixelsPerMeter <= 0.0
        || !std::isfinite(logical.x) || !std::isfinite(logical.y)
        || !std::isfinite(center.x) || !std::isfinite(center.y)) {
        return QPointF{viewport.width() * 0.5, viewport.height() * 0.5};
    }
    return QPointF{viewport.width() * 0.5 + (logical.x - center.x) * pixelsPerMeter,
                   viewport.height() * 0.5 - (logical.y - center.y) * pixelsPerMeter};
}

inline GeoPos screenToLogical(const QPointF& screen, const GeoPos& center,
                              const QSizeF& viewport, double pixelsPerMeter) {
    if (!std::isfinite(screen.x()) || !std::isfinite(screen.y())
        || !std::isfinite(viewport.width()) || !std::isfinite(viewport.height())
        || viewport.width() <= 0.0 || viewport.height() <= 0.0
        || !std::isfinite(pixelsPerMeter) || pixelsPerMeter <= 0.0
        || !std::isfinite(center.x) || !std::isfinite(center.y)) return center;
    return GeoPos{center.x + (screen.x() - viewport.width() * 0.5) / pixelsPerMeter,
                  center.y - (screen.y() - viewport.height() * 0.5) / pixelsPerMeter,
                  0.0};
}

inline GeoPos clampToExtent(const GeoPos& logical, const QSizeF& extent) {
    if (!std::isfinite(extent.width()) || !std::isfinite(extent.height())
        || extent.width() <= 0.0 || extent.height() <= 0.0) {
        return logical;
    }
    const double safeX = std::isfinite(logical.x) ? logical.x : extent.width() * 0.5;
    const double safeY = std::isfinite(logical.y) ? logical.y : extent.height() * 0.5;
    const double safeAlt = std::isfinite(logical.alt) ? logical.alt : 0.0;
    return GeoPos{std::clamp(safeX, 0.0, extent.width()),
                  std::clamp(safeY, 0.0, extent.height()), safeAlt};
}

inline GeoPos screenToBoundedLogical(const QPointF& screen, const GeoPos& center,
                                     const QSizeF& viewport, double pixelsPerMeter,
                                     const QSizeF& extent) {
    return clampToExtent(screenToLogical(screen, center, viewport, pixelsPerMeter), extent);
}

}

/// @brief Web Mercator projection utilities for tile-based GIS map.
namespace Mercator {

constexpr double kEarthRadius = 6378137.0;
constexpr double kOriginShift = M_PI * kEarthRadius;
constexpr double kInitialRes = 2.0 * kOriginShift / 256.0;
constexpr int    kTileSize = 256;
constexpr int    kMinZoom = 0;
constexpr int    kMaxZoom = 22;

inline int safeZoomShift(int zoom) {
    if (zoom < kMinZoom) zoom = kMinZoom;
    if (zoom > kMaxZoom) zoom = kMaxZoom;
    return 1 << zoom;
}

/// @brief Convert lat/lon (degrees) to Mercator meters.
inline QPointF latLonToMeters(double lat, double lon) {
    constexpr double kMaxLatitude = 85.05112878;
    lat = std::clamp(lat, -kMaxLatitude, kMaxLatitude);
    lon = std::clamp(lon, -180.0, 180.0);
    double mx = lon * kOriginShift / 180.0;
    double my = std::log(std::tan((90.0 + lat) * M_PI / 360.0)) / (M_PI / 180.0);
    my = my * kOriginShift / 180.0;
    return {mx, my};
}

/// @brief Convert Mercator meters to tile X/Y at a given zoom level.
inline void metersToTile(double mx, double my, int zoom, int& tx, int& ty) {
    int shift = safeZoomShift(zoom);
    double res = kInitialRes / shift;
    tx = static_cast<int>((mx + kOriginShift) / (kTileSize * res));
    ty = static_cast<int>((kOriginShift - my) / (kTileSize * res));
    tx = std::clamp(tx, 0, shift - 1);
    ty = std::clamp(ty, 0, shift - 1);
}

/// @brief Convert Mercator meters to pixel position within the world at a zoom level.
inline QPointF metersToPixels(double mx, double my, int zoom) {
    int shift = safeZoomShift(zoom);
    double res = kInitialRes / shift;
    double px = (mx + kOriginShift) / res;
    double py = (kOriginShift - my) / res;
    return {px, py};
}

/// @brief Convert pixel position to Mercator meters.
inline QPointF pixelsToMeters(double px, double py, int zoom) {
    int shift = safeZoomShift(zoom);
    double res = kInitialRes / shift;
    double mx = px * res - kOriginShift;
    double my = kOriginShift - py * res;
    return {mx, my};
}

} // namespace Mercator

} // namespace gbr

inline gbr::GeoPos operator+(const gbr::GeoPos& a, const gbr::GeoPos& b) {
    return gbr::GeoPos{a.x + b.x, a.y + b.y, a.alt + b.alt};
}
inline gbr::GeoPos operator-(const gbr::GeoPos& a, const gbr::GeoPos& b) {
    return gbr::GeoPos{a.x - b.x, a.y - b.y, a.alt - b.alt};
}
inline gbr::GeoPos operator*(const gbr::GeoPos& a, double k) {
    return gbr::GeoPos{a.x * k, a.y * k, a.alt * k};
}
