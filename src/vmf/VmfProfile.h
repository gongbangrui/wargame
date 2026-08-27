#pragma once

#include "VmfCodec.h"
#include "VmfMessageCatalog.h"

#include <memory>

namespace gbr::vmf {

// Loads the repository's VMF design subset without making callers depend on
// the layout of the design/ directory.  The profile is intentionally explicit
// about its identifier so a running scenario cannot silently switch encoding
// rules when a dictionary is upgraded.
class VmfProfile final {
public:
    static constexpr const char* DesignV1 = "vmf-design-v1";
    static constexpr const char* DemoV2 = "vmf-demo-v2";

    static std::shared_ptr<const DictionarySet> load(const QString& profileId,
                                                     const QString& rootDirectory,
                                                     QList<Diagnostic>* diagnostics = nullptr);
    static std::shared_ptr<const DictionarySet> loadDesignV1(
        const QString& rootDirectory, QList<Diagnostic>* diagnostics = nullptr);
    static std::shared_ptr<const VmfMessageCatalog> loadCatalog(
        const QString& profileId, const QString& rootDirectory,
        QList<Diagnostic>* diagnostics = nullptr);
    static std::shared_ptr<const VmfMessageCatalog> loadCatalogDesignV1(
        const QString& rootDirectory, QList<Diagnostic>* diagnostics = nullptr);

    static QStringList designV1Messages();
};

} // namespace gbr::vmf
