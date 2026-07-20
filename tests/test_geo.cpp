#include <gtest/gtest.h>
#include "core/Geo.h"
#include "core/MapProvider.h"

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
