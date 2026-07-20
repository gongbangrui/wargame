#pragma once

#include <QString>

namespace gbr {

/// Resolves the single runtime root used by all local GIS tile consumers.
class TileCacheLocator final {
public:
    /// Resolution order is explicitDir, WARGAME_MAP_DIR, then appDir/map.
    /// An explicitly selected but invalid directory does not silently fall
    /// through, making deployment/configuration errors visible.
    static QString resolve(const QString& explicitDir = QString());
    static bool isUsableMapDirectory(const QString& path);

private:
    static QString normalizedUsableDirectory(const QString& path);
};

} // namespace gbr
