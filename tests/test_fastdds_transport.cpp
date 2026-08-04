#include <gtest/gtest.h>

#include "network/FastDdsTransport.h"

using namespace gbr;

TEST(FastDdsTransportTest, RequiresRoomAndSeatBeforeStarting) {
    FastDdsTransport transport;
    QString error;
    EXPECT_FALSE(transport.start(&error));
    EXPECT_FALSE(error.isEmpty());
    FastDdsTransport::Options options;
    options.roomId = QStringLiteral("main");
    options.seatId = QStringLiteral("red_commander");
    FastDdsTransport ready(options, nullptr);
    EXPECT_FALSE(ready.start(&error));
    EXPECT_FALSE(ready.isLocal());
    EXPECT_TRUE(ready.backendName().contains(QStringLiteral("Fast DDS disabled")));
}

TEST(FastDdsTransportTest, RejectsCompatibilityModeUntilSecurityIsDeployed) {
    FastDdsTransport::Options options;
    options.roomId = QStringLiteral("main");
    options.seatId = QStringLiteral("red_recon_1");
    options.transportMode = QStringLiteral("compatibility");
    FastDdsTransport transport(options, nullptr);
    QString error;
    EXPECT_FALSE(transport.start(&error));
    EXPECT_FALSE(transport.running());
    EXPECT_TRUE(error.contains(QStringLiteral("安全认证与加密")));
}

TEST(FastDdsTransportTest, DdsModeUsesAdapterWhenAvailableOrReportsMissingDependency) {
    FastDdsTransport::Options options;
    options.roomId = QStringLiteral("main");
    options.seatId = QStringLiteral("red_recon_1");
    options.transportMode = QStringLiteral("dds");
    FastDdsTransport transport(options, nullptr);
    QString error;

#ifdef WARGAME_HAS_FASTDDS
    ASSERT_TRUE(transport.start(&error)) << error.toStdString();
    EXPECT_TRUE(transport.running());
    EXPECT_TRUE(transport.backendName().contains(QStringLiteral("Fast DDS")));
    transport.stop();
#else
    EXPECT_FALSE(transport.start(&error));
    EXPECT_FALSE(transport.running());
    EXPECT_TRUE(error.contains(QStringLiteral("缺少 SDK 或 fastddsgen")));
#endif
}
