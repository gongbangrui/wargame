#include "core/MessageBus.h"
#include "vmf/VmfMessageCatalog.h"
#include "vmf/VmfProfile.h"

#include <gtest/gtest.h>

namespace {

QString designPath(const QString& relative) {
    return QStringLiteral(WARGAME_SOURCE_DIR) + QLatin1Char('/') + relative;
}

} // namespace

TEST(VmfCatalog, LoadsDesignMetadataAndSeparatesCatalogIdFromDfiDui) {
    QList<gbr::vmf::Diagnostic> diagnostics;
    const auto catalog = gbr::vmf::VmfProfile::loadCatalogDesignV1(
        designPath(QStringLiteral("design/EncoderDecoder")), &diagnostics);
    ASSERT_TRUE(catalog)
        << gbr::vmf::Codec::diagnosticsToString(diagnostics).toStdString();
    const auto entry = catalog->entryFor(QStringLiteral("TargetReport"),
                                         QStringLiteral("Target Report"));
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->catalogId, QStringLiteral("47001"));
    EXPECT_EQ(entry->informationValue.level, QStringLiteral("high"));
    EXPECT_EQ(entry->informationValue.score, 90);
    EXPECT_FALSE(entry->informationValue.fields.isEmpty());
    EXPECT_NE(entry->catalogId, QStringLiteral("4200"));
}

TEST(VmfCatalog, ResolvesOneToManyAttackOrderMapping) {
    const auto catalog = gbr::vmf::VmfMessageCatalog::designV1();
    const QJsonObject route{{QStringLiteral("waypoints"), QJsonArray{
        QJsonObject{{QStringLiteral("x"), 1.0}, {QStringLiteral("y"), 2.0}}}}};
    const auto routed = catalog->entryFor(QStringLiteral("AttackOrder"),
                                          QStringLiteral("Land Route"), route);
    ASSERT_TRUE(routed.has_value());
    EXPECT_EQ(routed->payloadCondition, QStringLiteral("route"));
    const auto point = catalog->entryFor(QStringLiteral("AttackOrder"),
                                         QStringLiteral("NetworkMonitoring"),
                                         QJsonObject{{QStringLiteral("targetId"), "t"}});
    ASSERT_TRUE(point.has_value());
    EXPECT_EQ(point->payloadCondition, QStringLiteral("no-route"));
}

TEST(VmfCatalog, EnforcesRoleAndMappingContract) {
    const auto catalog = gbr::vmf::VmfMessageCatalog::designV1();
    QString error;
    EXPECT_TRUE(catalog->validate(QStringLiteral("GroundAttackConfirm"),
                                  QStringLiteral("Land Route"),
                                  QJsonObject{{QStringLiteral("waypoints"), QJsonArray{
                                      QJsonObject{{QStringLiteral("x"), 1.0},
                                                   {QStringLiteral("y"), 2.0}}}}},
                                  QStringLiteral("ground"), QStringLiteral("attack"), &error))
        << error.toStdString();
    EXPECT_FALSE(catalog->validate(QStringLiteral("GroundAttackConfirm"),
                                   QStringLiteral("Land Route"), QJsonObject{},
                                   QStringLiteral("commander"), QStringLiteral("attack"), &error));
    EXPECT_FALSE(catalog->validate(QStringLiteral("TargetReport"),
                                   QStringLiteral("Land Route"), QJsonObject{},
                                   QStringLiteral("recon"), QStringLiteral("commander"), &error));
    EXPECT_NE(error.indexOf(QStringLiteral("目录")), -1);
}

TEST(VmfCatalog, AllowsReconnaissanceDestructionConfirmation) {
    const auto catalog = gbr::vmf::VmfMessageCatalog::designV1();
    QString error;
    EXPECT_TRUE(catalog->validate(
        QStringLiteral("TargetDestroyed"), QStringLiteral("Target Report"),
        QJsonObject{{QStringLiteral("targetId"), QStringLiteral("blue_target")},
                    {QStringLiteral("attackerId"), QStringLiteral("red_attack")}},
        QStringLiteral("recon"), QStringLiteral("commander"), &error))
        << error.toStdString();
}
