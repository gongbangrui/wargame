#include "MapProvider.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <cmath>
#include <limits>

namespace gbr {

namespace {

bool readInteger(const QJsonValue& value, int minimum, int maximum, int& result) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
        || number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
        return false;
    }
    result = static_cast<int>(number);
    return true;
}

}

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

bool MapProvider::loadMetadataFile(const QString& path, QString* error) {
    auto reject = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (error) error->clear();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return reject(QStringLiteral("无法读取地图元数据: %1").arg(path));
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return reject(QStringLiteral("地图元数据 JSON 无效: %1").arg(parseError.errorString()));
    }
    return applyMetadata(document.object(), error);
}

bool MapProvider::applyMetadata(const QJsonObject& metadata, QString* error) {
    auto reject = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (error) error->clear();
    if (metadata.value(QStringLiteral("projection")).toString() != QLatin1String("EPSG:3857")
        || metadata.value(QStringLiteral("scheme")).toString() != QLatin1String("xyz")
        || metadata.value(QStringLiteral("format")).toString(QStringLiteral("png"))
               != QLatin1String("png")) {
        return reject(QStringLiteral("地图投影、瓦片方案或瓦片尺寸无效"));
    }

    int tileSize = 0;
    int minZoom = 0;
    int maxZoom = 0;
    if (!readInteger(metadata.value(QStringLiteral("tileSize")), Mercator::kTileSize,
                     Mercator::kTileSize, tileSize)
        || !readInteger(metadata.value(QStringLiteral("minZoom")), Mercator::kMinZoom,
                        Mercator::kMaxZoom, minZoom)
        || !readInteger(metadata.value(QStringLiteral("maxZoom")), Mercator::kMinZoom,
                        Mercator::kMaxZoom, maxZoom)
        || maxZoom < minZoom) {
        return reject(QStringLiteral("地图瓦片缩放级别无效"));
    }

    const QJsonValue metadataRevision = metadata.value(QStringLiteral("revision"));
    int revision = 0;
    if (!readInteger(metadataRevision, 1, std::numeric_limits<int>::max(), revision)) {
        return reject(QStringLiteral("地图修订版本无效"));
    }
    if (revision <= m_metadataRevision) {
        return reject(QStringLiteral("地图修订版本过期"));
    }

    const QJsonObject alignment = metadata.value(QStringLiteral("projectAlignment")).toObject();
    const QJsonValue originLat = alignment.value(QStringLiteral("originLat"));
    const QJsonValue originLon = alignment.value(QStringLiteral("originLon"));
    const QJsonValue width = alignment.value(QStringLiteral("logicalWidthMeters"));
    const QJsonValue height = alignment.value(QStringLiteral("logicalHeightMeters"));
    if (!originLat.isDouble() || !originLon.isDouble() || !width.isDouble() || !height.isDouble()
        || alignment.value(QStringLiteral("logicalXAxis")).toString() != QLatin1String("east")
        || alignment.value(QStringLiteral("logicalYAxis")).toString() != QLatin1String("north")) {
        return reject(QStringLiteral("地图逻辑坐标定义无效"));
    }

    const double latitude = originLat.toDouble();
    const double longitude = originLon.toDouble();
    const double widthMeters = width.toDouble();
    const double heightMeters = height.toDouble();
    constexpr double maxLatitude = 85.05112878;
    if (!std::isfinite(latitude) || !std::isfinite(longitude) || !std::isfinite(widthMeters)
        || !std::isfinite(heightMeters) || latitude < -maxLatitude || latitude > maxLatitude
        || longitude < -180.0 || longitude > 180.0 || widthMeters <= 0.0 || heightMeters <= 0.0) {
        return reject(QStringLiteral("地图原点或范围无效"));
    }

    m_widthM = widthMeters;
    m_heightM = heightMeters;
    m_center = GeoPos{m_widthM * 0.5, m_heightM * 0.5, 0.0};
    m_origin = GeoCoord{latitude, longitude};
    m_mercatorOrigin = Mercator::latLonToMeters(latitude, longitude);
    m_minTileZoom = minZoom;
    m_maxTileZoom = maxZoom;
    m_tileZoom = maxZoom;
    m_metadataRevision = revision;
    m_name = metadata.value(QStringLiteral("name")).toString(m_name);
    return true;
}

bool MapProvider::contains(const GeoPos& logical) const {
    return std::isfinite(logical.x) && std::isfinite(logical.y) && logical.x >= 0.0
        && logical.y >= 0.0 && logical.x <= m_widthM && logical.y <= m_heightM;
}

GeoPos MapProvider::clampToExtent(const GeoPos& logical) const {
    if (!std::isfinite(logical.x) || !std::isfinite(logical.y)) {
        return GeoPos{m_center.x, m_center.y, logical.alt};
    }
    return MapCoordinates::clampToExtent(logical, QSizeF(m_widthM, m_heightM));
}

QPointF MapProvider::toPixel(double viewportW, double viewportH, const GeoPos& logical) const {
    const double baseScale = std::min(viewportW / m_widthM, viewportH / m_heightM);
    const double scale = baseScale * m_zoom;
    return MapCoordinates::logicalToScreen(logical, m_center, QSizeF(viewportW, viewportH), scale);
}

GeoPos MapProvider::fromPixel(double viewportW, double viewportH, const QPointF& px) const {
    const double baseScale = std::min(viewportW / m_widthM, viewportH / m_heightM);
    const double scale = baseScale * m_zoom;
    return MapCoordinates::screenToBoundedLogical(px, m_center, QSizeF(viewportW, viewportH),
                                                  scale, QSizeF(m_widthM, m_heightM));
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
    o["tileMinZoom"] = m_minTileZoom;
    o["tileMaxZoom"] = m_maxTileZoom;
    o["tilePixelsPerMeterAtZoom0"] = 1.0 / Mercator::kInitialRes;
    o["mapRevision"] = m_metadataRevision;
    return o;
}

QPointF MapProvider::toMercator(const GeoPos& logical) const {
    return QPointF(m_mercatorOrigin.x() + logical.x,
                   m_mercatorOrigin.y() + logical.y);
}

GeoPos MapProvider::fromMercator(const QPointF& mercator) const {
    if (!std::isfinite(mercator.x()) || !std::isfinite(mercator.y())) {
        return GeoPos{m_center.x, m_center.y, 0.0};
    }
    return GeoPos{mercator.x() - m_mercatorOrigin.x(),
                  mercator.y() - m_mercatorOrigin.y(), 0.0};
}

GeoCoord MapProvider::logicalToGeo(const GeoPos& logical) const {
    return Mercator::metersToLatLon(toMercator(logical).x(), toMercator(logical).y());
}

GeoPos MapProvider::geoToLogical(const GeoCoord& coordinate) const {
    const QPointF mercator = Mercator::latLonToMeters(coordinate.lat, coordinate.lon);
    return fromMercator(mercator);
}

} // namespace gbr
