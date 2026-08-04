#include <gtest/gtest.h>

#include "core/Geo.h"
#include "core/SimulationEngine.h"
#include "core/TileCacheLocator.h"
#include "core/TileImageProvider.h"
#include "view/MapTileRenderer.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

using namespace gbr;

namespace {

constexpr auto kTileId = "12/3406/1748";

QJsonObject metadata(const int revision = 1, const double originLat = 25.4,
                     const double originLon = 119.3, const double width = 20000.0,
                     const double height = 15000.0, const int zoom = 12) {
    return QJsonObject{
        {QStringLiteral("revision"), revision},
        {QStringLiteral("projection"), QStringLiteral("EPSG:3857")},
        {QStringLiteral("scheme"), QStringLiteral("xyz")},
        {QStringLiteral("format"), QStringLiteral("png")},
        {QStringLiteral("tileSize"), 256},
        {QStringLiteral("minZoom"), zoom},
        {QStringLiteral("maxZoom"), zoom},
        {QStringLiteral("projectAlignment"), QJsonObject{
            {QStringLiteral("originLat"), originLat},
            {QStringLiteral("originLon"), originLon},
            {QStringLiteral("logicalWidthMeters"), width},
            {QStringLiteral("logicalHeightMeters"), height},
            {QStringLiteral("logicalXAxis"), QStringLiteral("east")},
            {QStringLiteral("logicalYAxis"), QStringLiteral("north")},
        }},
    };
}

bool writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

bool createMapRoot(const QString& root) {
    return QDir().mkpath(root + QStringLiteral("/12/3406"))
        && writeFile(root + QStringLiteral("/metadata.json"),
                     QJsonDocument(metadata()).toJson(QJsonDocument::Compact))
        && writeFile(root + QStringLiteral("/tilejson.json"), QByteArrayLiteral("{}\n"));
}

QString tilePath(const QString& root) {
    return root + QStringLiteral("/") + QString::fromLatin1(kTileId)
        + QStringLiteral(".png");
}

class ScopedMapDirectoryEnvironment {
public:
    explicit ScopedMapDirectoryEnvironment(const QString& value)
        : m_wasSet(qEnvironmentVariableIsSet("WARGAME_MAP_DIR"))
        , m_previous(qgetenv("WARGAME_MAP_DIR")) {
        qputenv("WARGAME_MAP_DIR", value.toUtf8());
    }

    ~ScopedMapDirectoryEnvironment() {
        if (m_wasSet) {
            qputenv("WARGAME_MAP_DIR", m_previous);
        } else {
            qunsetenv("WARGAME_MAP_DIR");
        }
    }

private:
    bool m_wasSet;
    QByteArray m_previous;
};

QGuiApplication* ensureGuiApplication() {
    if (auto* application = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        return application;
    }
    static int argc = 1;
    static char applicationName[] = "wargame-tests";
    static char* argv[] = {applicationName, nullptr};
    qputenv("QT_QPA_PLATFORM", "offscreen");
    static QGuiApplication application(argc, argv);
    return &application;
}

QPointF eastTileEdgeWithinMap(const double originLat, const double originLon,
                              const double logicalWidth) {
    const QPointF origin = Mercator::latLonToMeters(originLat, originLon);
    int tileX = 0;
    int tileY = 0;
    Mercator::metersToTile(origin.x(), origin.y(), 12, tileX, tileY);
    const double tileWorld = Mercator::kTileSize * Mercator::kInitialRes
        / Mercator::safeZoomShift(12);
    const double eastEdge = (tileX + 1) * tileWorld - Mercator::kOriginShift - origin.x();
    EXPECT_GT(eastEdge, 0.0);
    EXPECT_LT(eastEdge, logicalWidth);
    return QPointF(eastEdge, 0.0);
}

void expectRendererTileEdgeAlignment(const QSizeF& viewport, const double zoom,
                                     const GeoPos& center) {
    constexpr double originLat = 25.4;
    constexpr double originLon = 119.3;
    constexpr double logicalWidth = 20000.0;
    constexpr double logicalHeight = 15000.0;
    MapTileRenderer renderer;
    renderer.setWidth(viewport.width());
    renderer.setHeight(viewport.height());
    renderer.setOriginLat(originLat);
    renderer.setOriginLon(originLon);
    renderer.setLogicalWidthMeters(logicalWidth);
    renderer.setLogicalHeightMeters(logicalHeight);
    renderer.setCenterX(center.x);
    renderer.setCenterY(center.y);
    renderer.setZoom(zoom);

    const QPointF edge = eastTileEdgeWithinMap(originLat, originLon, logicalWidth);
    const QPointF screen = renderer.simToScreen(edge.x(), edge.y());
    const QPointF expected{viewport.width() * 0.5 + (edge.x() - center.x) * zoom,
                           viewport.height() * 0.5 + center.y * zoom};

    EXPECT_NEAR(screen.x(), expected.x(), 1e-9);
    EXPECT_NEAR(screen.y(), expected.y(), 1e-9);
    const QPointF restored = renderer.screenToSim(screen.x(), screen.y());
    EXPECT_NEAR(restored.x(), edge.x(), 1e-8);
    EXPECT_NEAR(restored.y(), edge.y(), 1e-8);
}

} // namespace

TEST(MapTilesTest, LocatorResolvesExplicitMapRoot) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(createMapRoot(temporary.path()));

    EXPECT_TRUE(TileCacheLocator::isUsableMapDirectory(temporary.path()));
    EXPECT_EQ(TileCacheLocator::resolve(temporary.path()),
              QDir(temporary.path()).canonicalPath());
}

TEST(MapTilesTest, EnvironmentOverrideIsAuthoritative) {
    QTemporaryDir validMap;
    QTemporaryDir invalidMap;
    ASSERT_TRUE(validMap.isValid());
    ASSERT_TRUE(invalidMap.isValid());
    ASSERT_TRUE(createMapRoot(validMap.path()));

    {
        ScopedMapDirectoryEnvironment environment(validMap.path());
        EXPECT_EQ(TileCacheLocator::resolve(), QDir(validMap.path()).canonicalPath());
    }
    {
        ScopedMapDirectoryEnvironment environment(invalidMap.path());
        EXPECT_TRUE(TileCacheLocator::resolve().isEmpty());
    }
}

TEST(MapTilesTest, ProviderTreatsCorruptPngAsMissing) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(createMapRoot(temporary.path()));
    ASSERT_TRUE(writeFile(tilePath(temporary.path()), QByteArrayLiteral("not a png")));

    TileImageProvider provider(temporary.path());
    QSize corruptSize;
    QSize missingSize;
    const QImage corrupt = provider.requestImage(QString::fromLatin1(kTileId),
                                                  &corruptSize, {});
    const QImage missing = provider.requestImage(QStringLiteral("12/3406/1749"),
                                                  &missingSize, {});

    EXPECT_EQ(corruptSize, QSize(256, 256));
    EXPECT_EQ(missingSize, QSize(256, 256));
    EXPECT_EQ(corrupt, missing);
}

TEST(MapTilesTest, ProviderSwitchesResolvedCacheDirectory) {
    QTemporaryDir redMap;
    QTemporaryDir blueMap;
    ASSERT_TRUE(redMap.isValid());
    ASSERT_TRUE(blueMap.isValid());
    ASSERT_TRUE(createMapRoot(redMap.path()));
    ASSERT_TRUE(createMapRoot(blueMap.path()));

    QImage redTile(16, 16, QImage::Format_RGB32);
    redTile.fill(QColor(220, 20, 40));
    ASSERT_TRUE(redTile.save(tilePath(redMap.path()), "PNG"));
    QImage blueTile(16, 16, QImage::Format_RGB32);
    blueTile.fill(QColor(20, 60, 220));
    ASSERT_TRUE(blueTile.save(tilePath(blueMap.path()), "PNG"));

    TileImageProvider provider(redMap.path());
    EXPECT_EQ(provider.cacheDirectory(), QDir(redMap.path()).canonicalPath());
    const QImage loadedRed = provider.requestImage(QString::fromLatin1(kTileId),
                                                    nullptr, {});

    provider.setCacheDirectory(blueMap.path());
    EXPECT_EQ(provider.cacheDirectory(), QDir(blueMap.path()).canonicalPath());
    const QImage loadedBlue = provider.requestImage(QString::fromLatin1(kTileId),
                                                     nullptr, {});

    ASSERT_FALSE(loadedRed.isNull());
    ASSERT_FALSE(loadedBlue.isNull());
    EXPECT_EQ(loadedRed.pixelColor(0, 0), QColor(220, 20, 40));
    EXPECT_EQ(loadedBlue.pixelColor(0, 0), QColor(20, 60, 220));
}

TEST(MapTilesTest, SimulationEngineLoadsStagedRuntimeMetadata) {
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(createMapRoot(temporary.path()));
    ASSERT_TRUE(writeFile(temporary.filePath(QStringLiteral("metadata.json")),
                          QJsonDocument(metadata(73, 11.5, 22.5, 1234.0, 5678.0, 7))
                              .toJson(QJsonDocument::Compact)));

    ScopedMapDirectoryEnvironment environment(temporary.path());
    SimulationEngine engine;
    const QJsonObject mapInfo = engine.mapInfo();

    EXPECT_EQ(mapInfo.value(QStringLiteral("mapRevision")).toInt(), 73);
    EXPECT_DOUBLE_EQ(mapInfo.value(QStringLiteral("widthMeters")).toDouble(), 1234.0);
    EXPECT_DOUBLE_EQ(mapInfo.value(QStringLiteral("heightMeters")).toDouble(), 5678.0);
    EXPECT_DOUBLE_EQ(mapInfo.value(QStringLiteral("originLat")).toDouble(), 11.5);
    EXPECT_DOUBLE_EQ(mapInfo.value(QStringLiteral("originLon")).toDouble(), 22.5);
    EXPECT_EQ(mapInfo.value(QStringLiteral("tileZoom")).toInt(), 7);
}

TEST(MapTilesTest, RendererAlignsOriginTileEdgeAtWideViewport) {
    ensureGuiApplication();
    expectRendererTileEdgeAlignment(QSizeF{1280.0, 720.0}, 0.04,
                                    GeoPos{10000.0, 7500.0, 0.0});
}

TEST(MapTilesTest, RendererAlignsOriginTileEdgeAtTallViewport) {
    ensureGuiApplication();
    expectRendererTileEdgeAlignment(QSizeF{800.0, 1200.0}, 0.07,
                                    GeoPos{9000.0, 6000.0, 0.0});
}
