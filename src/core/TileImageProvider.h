#pragma once

#include <QQuickImageProvider>
#include <QImage>
#include <QString>
#include <QMutex>

namespace gbr {

/// @brief Provides map tiles from local cache for QML Image sources.
/// @details Tile identifiers are "z/x/y" (e.g., "12/3421/2187").
/// Tiles are loaded from the resolved map root at {z}/{x}/{y}.png.
class TileImageProvider : public QQuickImageProvider {
public:
    TileImageProvider(const QString& cacheDir = QString());

    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override;

    void setCacheDirectory(const QString& dir);
    QString cacheDirectory() const;

private:
    QString m_cacheDir;
    QImage m_placeholderTile;
    mutable QMutex m_cacheDirMutex;
};

} // namespace gbr
