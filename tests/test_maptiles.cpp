#include <gtest/gtest.h>

#include "core/TileCacheLocator.h"
#include "core/TileImageProvider.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

using namespace gbr;

namespace {

constexpr auto kTileId = "12/3406/1748";

bool writeFile(const QString& path, const QByteArray& contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(contents) == contents.size();
}

bool createMapRoot(const QString& root) {
    return QDir().mkpath(root + QStringLiteral("/12/3406"))
        && writeFile(root + QStringLiteral("/metadata.json"), QByteArrayLiteral("{}\n"))
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
