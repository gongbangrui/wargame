#pragma once

#include <QPointF>
#include <cmath>

namespace gbr {

struct GeoPos {
    double x = 0.0; // 米，东向
    double y = 0.0; // 米，北向
    double alt = 0.0; // 米，海拔

    QPointF toPointF() const { return QPointF(x, y); }
    static GeoPos fromPointF(const QPointF& p, double altitude = 0.0) {
        return GeoPos{p.x(), p.y(), altitude};
    }

    double distanceTo(const GeoPos& other) const {
        const double dx = x - other.x;
        const double dy = y - other.y;
        const double dz = alt - other.alt;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    GeoPos lerp(const GeoPos& target, double t) const {
        return GeoPos{
            x + (target.x - x) * t,
            y + (target.y - y) * t,
            alt + (target.alt - alt) * t
        };
    }
};

inline GeoPos operator+(const GeoPos& a, const GeoPos& b) {
    return GeoPos{a.x + b.x, a.y + b.y, a.alt + b.alt};
}
inline GeoPos operator-(const GeoPos& a, const GeoPos& b) {
    return GeoPos{a.x - b.x, a.y - b.y, a.alt - b.alt};
}
inline GeoPos operator*(const GeoPos& a, double k) {
    return GeoPos{a.x * k, a.y * k, a.alt * k};
}

} // namespace gbr

