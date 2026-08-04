#include <gtest/gtest.h>
#include "core/Geo.h"
#include "core/MapProvider.h"

#include <QFile>
#include <QTemporaryDir>

#include <limits>

using namespace gbr;

TEST(GeoTest, DefaultPosition) {
    GeoPos p;
    EXPECT_DOUBLE_EQ(p.x, 0.0);
    EXPECT_DOUBLE_EQ(p.y, 0.0);
    EXPECT_DOUBLE_EQ(p.alt, 0.0);
}

TEST(GeoTest, DistanceSamePoint) {
    GeoPos a{1000, 2000, 300};
    EXPECT_DOUBLE_EQ(a.distanceTo(a), 0.0);
}

TEST(GeoTest, Distance2D) {
    GeoPos a{0, 0, 0};
    GeoPos b{3000, 4000, 0};
    EXPECT_DOUBLE_EQ(a.distanceTo(b), 5000.0);
}

TEST(GeoTest, Distance3D) {
    GeoPos a{0, 0, 0};
    GeoPos b{300, 400, 500};
    // sqrt(300^2 + 400^2 + 500^2) = sqrt(90000 + 160000 + 250000) = sqrt(500000)
    double expected = std::sqrt(300.0*300 + 400.0*400 + 500.0*500);
    EXPECT_NEAR(a.distanceTo(b), expected, 1e-9);
}

TEST(GeoTest, LerpMidpoint) {
    GeoPos a{0, 0, 0};
    GeoPos b{100, 200, 300};
    GeoPos mid = a.lerp(b, 0.5);
    EXPECT_DOUBLE_EQ(mid.x, 50.0);
    EXPECT_DOUBLE_EQ(mid.y, 100.0);
    EXPECT_DOUBLE_EQ(mid.alt, 150.0);
}

TEST(GeoTest, LerpEndpoint) {
    GeoPos a{10, 20, 30};
    GeoPos b{100, 200, 300};
    GeoPos r0 = a.lerp(b, 0.0);
    EXPECT_DOUBLE_EQ(r0.x, 10.0);
    GeoPos r1 = a.lerp(b, 1.0);
    EXPECT_DOUBLE_EQ(r1.x, 100.0);
}

TEST(GeoTest, ToPointF) {
    GeoPos p{42.5, 17.3, 5000};
    QPointF pf = p.toPointF();
    EXPECT_DOUBLE_EQ(pf.x(), 42.5);
    EXPECT_DOUBLE_EQ(pf.y(), 17.3);
}

TEST(GeoTest, MercatorClampsProjectionAndTileBounds) {
    const QPointF projected = Mercator::latLonToMeters(90.0, 200.0);
    EXPECT_TRUE(std::isfinite(projected.x()));
    EXPECT_TRUE(std::isfinite(projected.y()));

    int tx = -1;
    int ty = -1;
    Mercator::metersToTile(projected.x(), projected.y(), 4, tx, ty);
    EXPECT_GE(tx, 0);
    EXPECT_LT(tx, 16);
    EXPECT_GE(ty, 0);
    EXPECT_LT(ty, 16);
}

TEST(GeoTest, MapProviderOriginUpdateAffectsProjection) {
    MapProvider map;
    const QPointF before = map.toMercator(GeoPos{});
    map.setOrigin(GeoCoord{0.0, 0.0});
    const QPointF after = map.toMercator(GeoPos{});

    EXPECT_NE(before, after);
    EXPECT_NEAR(after.x(), 0.0, 1e-6);
    EXPECT_NEAR(after.y(), 0.0, 1e-6);
}

TEST(GeoTest, MapProviderMetadataDefinesBoundedCoordinateContract) {
    MapProvider map;
    const QJsonObject metadata{
        {QStringLiteral("revision"), 1},
        {QStringLiteral("projection"), QStringLiteral("EPSG:3857")},
        {QStringLiteral("scheme"), QStringLiteral("xyz")},
        {QStringLiteral("tileSize"), 256},
        {QStringLiteral("minZoom"), 12},
        {QStringLiteral("maxZoom"), 12},
        {QStringLiteral("projectAlignment"), QJsonObject{
            {QStringLiteral("originLat"), 25.4},
            {QStringLiteral("originLon"), 119.3},
            {QStringLiteral("logicalWidthMeters"), 20000.0},
            {QStringLiteral("logicalHeightMeters"), 15000.0},
            {QStringLiteral("logicalXAxis"), QStringLiteral("east")},
            {QStringLiteral("logicalYAxis"), QStringLiteral("north")},
        }},
    };
    QString error;

    ASSERT_TRUE(map.applyMetadata(metadata, &error)) << error.toStdString();
    EXPECT_DOUBLE_EQ(map.widthMeters(), 20000.0);
    EXPECT_DOUBLE_EQ(map.heightMeters(), 15000.0);
    EXPECT_EQ(map.tileZoomForView(), 12);
    EXPECT_EQ(map.metadataRevision(), 1);
    EXPECT_TRUE(map.contains(GeoPos{20000.0, 15000.0, 0.0}));
    EXPECT_FALSE(map.contains(GeoPos{20000.01, 15000.0, 0.0}));

    const GeoPos logical{17500.0, 1250.0, 300.0};
    const QPointF pixel = map.toPixel(1280.0, 720.0, logical);
    const GeoPos restored = map.fromPixel(1280.0, 720.0, pixel);
    EXPECT_NEAR(restored.x, logical.x, 1e-9);
    EXPECT_NEAR(restored.y, logical.y, 1e-9);
    EXPECT_DOUBLE_EQ(restored.alt, 0.0);
    const GeoPos clamped = map.clampToExtent(GeoPos{-5.0, 20000.0, 99.0});
    EXPECT_DOUBLE_EQ(clamped.x, 0.0);
    EXPECT_DOUBLE_EQ(clamped.y, 15000.0);
    EXPECT_DOUBLE_EQ(clamped.alt, 99.0);
}

TEST(GeoTest, MapProviderRejectsStaleMetadataRevisionWithoutMutation) {
    MapProvider map;
    QJsonObject currentMetadata{
        {QStringLiteral("revision"), 2},
        {QStringLiteral("projection"), QStringLiteral("EPSG:3857")},
        {QStringLiteral("scheme"), QStringLiteral("xyz")},
        {QStringLiteral("tileSize"), 256},
        {QStringLiteral("minZoom"), 12},
        {QStringLiteral("maxZoom"), 12},
        {QStringLiteral("projectAlignment"), QJsonObject{
            {QStringLiteral("originLat"), 25.4},
            {QStringLiteral("originLon"), 119.3},
            {QStringLiteral("logicalWidthMeters"), 20000.0},
            {QStringLiteral("logicalHeightMeters"), 15000.0},
            {QStringLiteral("logicalXAxis"), QStringLiteral("east")},
            {QStringLiteral("logicalYAxis"), QStringLiteral("north")},
        }},
    };
    QString error;
    ASSERT_TRUE(map.applyMetadata(currentMetadata, &error)) << error.toStdString();

    currentMetadata.insert(QStringLiteral("revision"), 1);
    currentMetadata.insert(QStringLiteral("projectAlignment"), QJsonObject{
        {QStringLiteral("originLat"), 0.0},
        {QStringLiteral("originLon"), 0.0},
        {QStringLiteral("logicalWidthMeters"), 1.0},
        {QStringLiteral("logicalHeightMeters"), 1.0},
        {QStringLiteral("logicalXAxis"), QStringLiteral("east")},
        {QStringLiteral("logicalYAxis"), QStringLiteral("north")},
    });

    EXPECT_FALSE(map.applyMetadata(currentMetadata, &error));
    EXPECT_EQ(error, QStringLiteral("地图修订版本过期"));
    EXPECT_EQ(map.metadataRevision(), 2);
    EXPECT_DOUBLE_EQ(map.widthMeters(), 20000.0);
    EXPECT_DOUBLE_EQ(map.heightMeters(), 15000.0);
}

TEST(GeoTest, MapProviderRejectsRepeatedMetadataRevisionWithoutMutation) {
    MapProvider map;
    QJsonObject metadata{
        {QStringLiteral("revision"), 2},
        {QStringLiteral("projection"), QStringLiteral("EPSG:3857")},
        {QStringLiteral("scheme"), QStringLiteral("xyz")},
        {QStringLiteral("tileSize"), 256},
        {QStringLiteral("minZoom"), 12},
        {QStringLiteral("maxZoom"), 12},
        {QStringLiteral("projectAlignment"), QJsonObject{
            {QStringLiteral("originLat"), 25.4},
            {QStringLiteral("originLon"), 119.3},
            {QStringLiteral("logicalWidthMeters"), 20000.0},
            {QStringLiteral("logicalHeightMeters"), 15000.0},
            {QStringLiteral("logicalXAxis"), QStringLiteral("east")},
            {QStringLiteral("logicalYAxis"), QStringLiteral("north")},
        }},
    };
    QString error;
    ASSERT_TRUE(map.applyMetadata(metadata, &error)) << error.toStdString();

    metadata.insert(QStringLiteral("projectAlignment"), QJsonObject{
        {QStringLiteral("originLat"), 0.0},
        {QStringLiteral("originLon"), 0.0},
        {QStringLiteral("logicalWidthMeters"), 1.0},
        {QStringLiteral("logicalHeightMeters"), 1.0},
        {QStringLiteral("logicalXAxis"), QStringLiteral("east")},
        {QStringLiteral("logicalYAxis"), QStringLiteral("north")},
    });

    EXPECT_FALSE(map.applyMetadata(metadata, &error));
    EXPECT_EQ(error, QStringLiteral("地图修订版本过期"));
    EXPECT_EQ(map.metadataRevision(), 2);
    EXPECT_DOUBLE_EQ(map.widthMeters(), 20000.0);
    EXPECT_DOUBLE_EQ(map.heightMeters(), 15000.0);
}

TEST(GeoTest, MapProviderRejectsInvalidCoordinateMetadataWithoutMutation) {
    MapProvider map;
    const QJsonObject invalidMetadata{
        {QStringLiteral("projection"), QStringLiteral("EPSG:4326")},
    };
    QString error;

    EXPECT_FALSE(map.applyMetadata(invalidMetadata, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_DOUBLE_EQ(map.widthMeters(), 20000.0);
    EXPECT_DOUBLE_EQ(map.heightMeters(), 15000.0);
}

TEST(GeoTest, MapProviderRejectsFractionalTileZoomWithoutMutation) {
    MapProvider map;
    const QJsonObject metadata{
        {QStringLiteral("revision"), 2},
        {QStringLiteral("projection"), QStringLiteral("EPSG:3857")},
        {QStringLiteral("scheme"), QStringLiteral("xyz")},
        {QStringLiteral("tileSize"), 256},
        {QStringLiteral("minZoom"), 12.5},
        {QStringLiteral("maxZoom"), 12},
        {QStringLiteral("projectAlignment"), QJsonObject{
            {QStringLiteral("originLat"), 25.4},
            {QStringLiteral("originLon"), 119.3},
            {QStringLiteral("logicalWidthMeters"), 1.0},
            {QStringLiteral("logicalHeightMeters"), 1.0},
            {QStringLiteral("logicalXAxis"), QStringLiteral("east")},
            {QStringLiteral("logicalYAxis"), QStringLiteral("north")},
        }},
    };
    QString error;

    EXPECT_FALSE(map.applyMetadata(metadata, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(map.tileZoomForView(), 12);
    EXPECT_DOUBLE_EQ(map.widthMeters(), 20000.0);
    EXPECT_DOUBLE_EQ(map.heightMeters(), 15000.0);
}

TEST(GeoTest, MapProviderBoundsInversePickingToDeclaredExtent) {
    MapProvider map;

    const GeoPos picked = map.fromPixel(1000.0, 500.0, QPointF(0.0, 500.0));

    EXPECT_DOUBLE_EQ(picked.x, 0.0);
    EXPECT_DOUBLE_EQ(picked.y, 0.0);
}

TEST(GeoTest, SharedScreenCoordinateConversionPreservesTileEdgeOrientation) {
    const GeoPos center{10000.0, 7500.0, 0.0};
    const QSizeF viewport{1200.0, 900.0};
    const QSizeF extent{20000.0, 15000.0};
    constexpr double pixelsPerMeter = 0.04;
    const GeoPos northWest{0.0, 15000.0, 0.0};

    const QPointF screen = MapCoordinates::logicalToScreen(northWest, center, viewport,
                                                            pixelsPerMeter);
    const GeoPos restored = MapCoordinates::screenToBoundedLogical(
        screen, center, viewport, pixelsPerMeter, extent);

    EXPECT_LT(screen.x(), viewport.width() * 0.5);
    EXPECT_LT(screen.y(), viewport.height() * 0.5);
    EXPECT_DOUBLE_EQ(restored.x, northWest.x);
    EXPECT_DOUBLE_EQ(restored.y, northWest.y);
}

TEST(GeoTest, BoundedPickingFallsBackForNonFiniteScreenInput) {
    const GeoPos center{10000.0, 7500.0, 0.0};
    const QSizeF viewport{1200.0, 900.0};
    const QSizeF extent{20000.0, 15000.0};
    const QPointF invalidScreen{std::numeric_limits<double>::quiet_NaN(), 450.0};

    const GeoPos picked = MapCoordinates::screenToBoundedLogical(
        invalidScreen, center, viewport, 0.04, extent);

    EXPECT_DOUBLE_EQ(picked.x, center.x);
    EXPECT_DOUBLE_EQ(picked.y, center.y);
}

TEST(GeoTest, MapProviderRejectsUnsupportedRasterFormatWithoutMutation) {
    MapProvider map;
    const QJsonObject metadata{
        {QStringLiteral("revision"), 1},
        {QStringLiteral("projection"), QStringLiteral("EPSG:3857")},
        {QStringLiteral("scheme"), QStringLiteral("xyz")},
        {QStringLiteral("tileSize"), 256},
        {QStringLiteral("format"), QStringLiteral("jpg")},
        {QStringLiteral("minZoom"), 12},
        {QStringLiteral("maxZoom"), 12},
        {QStringLiteral("projectAlignment"), QJsonObject{
            {QStringLiteral("originLat"), 25.4},
            {QStringLiteral("originLon"), 119.3},
            {QStringLiteral("logicalWidthMeters"), 20000.0},
            {QStringLiteral("logicalHeightMeters"), 15000.0},
            {QStringLiteral("logicalXAxis"), QStringLiteral("east")},
            {QStringLiteral("logicalYAxis"), QStringLiteral("north")},
        }},
    };
    QString error;

    EXPECT_FALSE(map.applyMetadata(metadata, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(map.metadataRevision(), 0);
    EXPECT_DOUBLE_EQ(map.widthMeters(), 20000.0);
    EXPECT_DOUBLE_EQ(map.heightMeters(), 15000.0);
}

TEST(GeoTest, MapProviderRejectsMalformedMetadataFileWithoutMutation) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    const QString metadataPath = temporary.filePath(QStringLiteral("metadata.json"));
    QFile file(metadataPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write("{ invalid json"), 14);
    file.close();

    MapProvider map;
    QString error;
    EXPECT_FALSE(map.loadMetadataFile(metadataPath, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_EQ(map.metadataRevision(), 0);
    EXPECT_DOUBLE_EQ(map.widthMeters(), 20000.0);
    EXPECT_DOUBLE_EQ(map.heightMeters(), 15000.0);
}
