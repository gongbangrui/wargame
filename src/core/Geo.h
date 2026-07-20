#pragma once

#include <QPointF>
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
