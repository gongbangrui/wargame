#include "VmfProfile.h"

#include <QDir>
#include <QFileInfo>

namespace gbr::vmf {

namespace {

QString profileDirectory(const QString& rootDirectory) {
    const QFileInfo direct(QDir(rootDirectory).filePath(QStringLiteral("msgStruct/msg0_1.xml")));
    if (direct.exists()) return QDir(rootDirectory).absolutePath();
    const QFileInfo nested(QDir(rootDirectory).filePath(QStringLiteral("EncoderDecoder/msgStruct/msg0_1.xml")));
    if (nested.exists()) return QDir(rootDirectory).filePath(QStringLiteral("EncoderDecoder"));
    return {};
}

} // namespace

QStringList VmfProfile::designV1Messages() {
    return {QStringLiteral("NetworkMonitoring"), QStringLiteral("Land Route"),
            QStringLiteral("Target Report")};
}

std::shared_ptr<const DictionarySet> VmfProfile::load(const QString& profileId,
                                                       const QString& rootDirectory,
                                                       QList<Diagnostic>* diagnostics) {
    if (profileId == QLatin1String(DesignV1)) return loadDesignV1(rootDirectory, diagnostics);
    if (diagnostics) diagnostics->append({Diagnostic::Severity::Error,
                                          QStringLiteral("PROFILE_UNKNOWN"), profileId,
                                          QStringLiteral("未知 VMF profile")});
    return {};
}

std::shared_ptr<const DictionarySet> VmfProfile::loadDesignV1(
    const QString& rootDirectory, QList<Diagnostic>* diagnostics) {
    const QString directory = profileDirectory(rootDirectory);
    if (directory.isEmpty()) {
        if (diagnostics) diagnostics->append({Diagnostic::Severity::Error,
                                              QStringLiteral("PROFILE_PATH"), rootDirectory,
                                              QStringLiteral("找不到 vmf-design-v1 字典目录")});
        return {};
    }
    const QDir base(directory);
    const QString content = base.filePath(QStringLiteral("dic_content.xml"));
    const QStringList messages{
        base.filePath(QStringLiteral("msgStruct/msg0_1.xml")),
        base.filePath(QStringLiteral("msgStruct/msg4_2.xml")),
        base.filePath(QStringLiteral("msgStruct/msg_target_report.xml"))};
    return DictionarySet::fromFiles(messages, content, diagnostics);
}

std::shared_ptr<const VmfMessageCatalog> VmfProfile::loadCatalog(
    const QString& profileId, const QString& rootDirectory,
    QList<Diagnostic>* diagnostics) {
    if (profileId == QLatin1String(DesignV1)) {
        return loadCatalogDesignV1(rootDirectory, diagnostics);
    }
    if (diagnostics) diagnostics->append({Diagnostic::Severity::Error,
                                          QStringLiteral("PROFILE_UNKNOWN"), profileId,
                                          QStringLiteral("未知 VMF profile")});
    return {};
}

std::shared_ptr<const VmfMessageCatalog> VmfProfile::loadCatalogDesignV1(
    const QString& rootDirectory, QList<Diagnostic>* diagnostics) {
    return VmfMessageCatalog::loadDesignV1(rootDirectory, diagnostics);
}

} // namespace gbr::vmf
