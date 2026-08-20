#include <gtest/gtest.h>

#include "core/Geo.h"
#include "core/MapProvider.h"
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
#include <QPainter>
#include <QTemporaryDir>

using namespace gbr;

namespace {

constexpr auto kTileId = "12/3406/1748";

QJsonObject metadata(const int revision = 1, const double originLat = 25.4,
                     const double originLon = 119.3, const double width = 20000.0,
                     const double height = 15000.0, const int minZoom = 12,
                     const int maxZoom = 12) {
    return QJsonObject{
        {QStringLiteral("revision"), revision},
        {QStringLiteral("projection"), QStringLiteral("EPSG:3857")},
        {QStringLiteral("scheme"), QStringLiteral("xyz")},
        {QStringLiteral("format"), QStringLiteral("png")},
        {QStringLiteral("tileSize"), 256},
        {QStringLiteral("minZoom"), minZoom},
        {QStringLiteral("maxZoom"), maxZoom},
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
    // Headless tests must not inherit a desktop theme or input-method plugin
    // that can synchronously contact an unavailable DBus session.
    qunsetenv("QT_QPA_PLATFORMTHEME");
    qunsetenv("QT_IM_MODULE");
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
                          QJsonDocument(metadata(73, 11.5, 22.5, 1234.0, 5678.0, 7, 9))
                              .toJson(QJsonDocument::Compact)));

    ScopedMapDirectoryEnvironment environment(temporary.path());
    SimulationEngine engine;
    const QJsonObject mapInfo = engine.mapInfo();

    EXPECT_EQ(mapInfo.value(QStringLiteral("mapRevision")).toInt(), 73);
    EXPECT_DOUBLE_EQ(mapInfo.value(QStringLiteral("widthMeters")).toDouble(), 1234.0);
    EXPECT_DOUBLE_EQ(mapInfo.value(QStringLiteral("heightMeters")).toDouble(), 5678.0);
    EXPECT_DOUBLE_EQ(mapInfo.value(QStringLiteral("originLat")).toDouble(), 11.5);
    EXPECT_DOUBLE_EQ(mapInfo.value(QStringLiteral("originLon")).toDouble(), 22.5);
    EXPECT_EQ(mapInfo.value(QStringLiteral("tileZoom")).toInt(), 9);
    EXPECT_EQ(mapInfo.value(QStringLiteral("tileMinZoom")).toInt(), 7);
    EXPECT_EQ(mapInfo.value(QStringLiteral("tileMaxZoom")).toInt(), 9);
    EXPECT_GT(mapInfo.value(QStringLiteral("tilePixelsPerMeterAtZoom0")).toDouble(), 0.0);
}

TEST(MapTilesTest, StagedMapMetadataLoadsWithRevisionAndZoomRange) {
    ensureGuiApplication();
    const QString metadataPath = QDir(QCoreApplication::applicationDirPath())
                                     .filePath(QStringLiteral("map/metadata.json"));
    ASSERT_TRUE(QFile::exists(metadataPath)) << qPrintable(metadataPath);

    MapProvider provider;
    QString error;
    ASSERT_TRUE(provider.loadMetadataFile(metadataPath, &error)) << qPrintable(error);
    EXPECT_EQ(provider.metadataRevision(), 1);
    EXPECT_EQ(provider.minTileZoom(), 12);
    EXPECT_EQ(provider.maxTileZoom(), 14);
}

TEST(MapTilesTest, RendererPaintsStagedRuntimeMap) {
    ensureGuiApplication();
    const QString mapRoot = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("map"));
    ASSERT_TRUE(TileCacheLocator::isUsableMapDirectory(mapRoot));

    MapProvider provider;
    QString error;
    ASSERT_TRUE(provider.loadMetadataFile(
        QDir(mapRoot).filePath(QStringLiteral("metadata.json")), &error))
        << qPrintable(error);
    const QJsonObject info = provider.describe();

    MapTileRenderer renderer;
    renderer.setWidth(640);
    renderer.setHeight(480);
    renderer.setTileCacheDir(mapRoot);
    renderer.setOriginLat(info.value(QStringLiteral("originLat")).toDouble());
    renderer.setOriginLon(info.value(QStringLiteral("originLon")).toDouble());
    renderer.setLogicalWidthMeters(info.value(QStringLiteral("widthMeters")).toDouble());
    renderer.setLogicalHeightMeters(info.value(QStringLiteral("heightMeters")).toDouble());
    renderer.setMinTileZoom(info.value(QStringLiteral("tileMinZoom")).toInt());
    renderer.setMaxTileZoom(info.value(QStringLiteral("tileMaxZoom")).toInt());
    renderer.setTileZoom(info.value(QStringLiteral("tileMinZoom")).toInt());
    renderer.setCenterX(info.value(QStringLiteral("widthMeters")).toDouble() / 2.0);
    renderer.setCenterY(info.value(QStringLiteral("heightMeters")).toDouble() / 2.0);
    renderer.setZoom(0.04);

    QImage rendered(640, 480, QImage::Format_RGB32);
    rendered.fill(Qt::black);
    QPainter painter(&rendered);
    renderer.paint(&painter);
    painter.end();

    const QColor blankBackground(8, 11, 20);
    int sampled = 0;
    int mapPixels = 0;
    for (int y = 4; y < rendered.height(); y += 8) {
        for (int x = 4; x < rendered.width(); x += 8) {
            ++sampled;
            if (rendered.pixelColor(x, y) != blankBackground) ++mapPixels;
        }
    }
    EXPECT_GT(mapPixels, sampled / 2);
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

TEST(MapTilesTest, RendererEmitsZoomChangedOnlyForActualChanges) {
    ensureGuiApplication();
    MapTileRenderer renderer;
    int signalCount = 0;
    QObject::connect(&renderer, &MapTileRenderer::zoomChanged, &renderer,
                     [&signalCount]() { ++signalCount; });

    renderer.setZoom(2.0);
    renderer.setZoom(2.0);

    EXPECT_EQ(signalCount, 1);
}

TEST(MapTilesTest, RendererKeepsZoomRangeValidWhenMinimumIsSetFirst) {
    ensureGuiApplication();
    MapTileRenderer renderer;

    renderer.setMinTileZoom(14);

    EXPECT_EQ(renderer.minTileZoom(), 14);
    EXPECT_EQ(renderer.maxTileZoom(), 14);
    EXPECT_EQ(renderer.tileZoom(), 14);
}

TEST(MapTilesTest, RendererCropsParentTileQuadrantWhenDetailTileIsMissing) {
    ensureGuiApplication();
    QTemporaryDir temporary;
    ASSERT_TRUE(temporary.isValid());
    ASSERT_TRUE(createMapRoot(temporary.path()));
    ASSERT_TRUE(writeFile(temporary.filePath(QStringLiteral("metadata.json")),
                          QJsonDocument(metadata(1, 25.4, 119.3, 20000.0, 15000.0, 12, 14))
                              .toJson(QJsonDocument::Compact)));

    QImage parent(256, 256, QImage::Format_RGB32);
    QPainter parentPainter(&parent);
    parentPainter.fillRect(parent.rect(), Qt::black);
    parentPainter.fillRect(QRect(0, 0, 64, 64), QColor(220, 40, 40));
    parentPainter.fillRect(QRect(64, 0, 64, 64), QColor(40, 210, 80));
    parentPainter.fillRect(QRect(128, 0, 64, 64), QColor(40, 100, 220));
    parentPainter.fillRect(QRect(192, 0, 64, 64), QColor(230, 190, 40));
    parentPainter.end();
    ASSERT_TRUE(parent.save(tilePath(temporary.path()), "PNG"));

    constexpr int sourceZoom = 12;
    constexpr int detailZoom = 14;
    constexpr int childX = 0;
    constexpr int childY = 0;
    const int sourceX = 3406;
    const int sourceY = 1748;
    const int detailX = sourceX * (1 << (detailZoom - sourceZoom)) + childX;
    const int detailY = sourceY * (1 << (detailZoom - sourceZoom)) + childY;
    const QPointF origin = Mercator::latLonToMeters(25.4, 119.3);
    const double tileWorld = Mercator::kTileSize * Mercator::kInitialRes
        / Mercator::safeZoomShift(detailZoom);
    const double tileTopX = detailX * tileWorld - Mercator::kOriginShift;
    const double tileTopY = Mercator::kOriginShift - detailY * tileWorld;
    const double tileLeft = tileTopX - origin.x();
    const double tileTop = tileTopY - origin.y();

    MapTileRenderer renderer;
    renderer.setWidth(256);
    renderer.setHeight(256);
    renderer.setOriginLat(25.4);
    renderer.setOriginLon(119.3);
    renderer.setLogicalWidthMeters(20000.0);
    renderer.setLogicalHeightMeters(15000.0);
    renderer.setMinTileZoom(sourceZoom);
    renderer.setMaxTileZoom(detailZoom);
    renderer.setTileZoom(detailZoom);
    renderer.setZoom(256.0 / tileWorld);
    renderer.setCenterX(tileLeft + tileWorld * 0.5);
    renderer.setCenterY(tileTop - tileWorld * 0.5);
    renderer.setTileCacheDir(temporary.path());

    QImage rendered(256, 256, QImage::Format_RGB32);
    rendered.fill(Qt::black);
    QPainter painter(&rendered);
    renderer.paint(&painter);
    painter.end();

    EXPECT_EQ(rendered.pixelColor(128, 128), QColor(220, 40, 40));
}
