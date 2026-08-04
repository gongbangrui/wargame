#include <gtest/gtest.h>

#include "network/FastDdsTransport.h"

using namespace gbr;

TEST(FastDdsIntegrationTest, RequiresFastDdsSdkAndGenerator) {
    FastDdsTransport::Options options;
    options.roomId = QStringLiteral("room-a");
    options.seatId = QStringLiteral("red_a");
    options.transportMode = QStringLiteral("compatibility");
    FastDdsTransport transport(options);
    QString error;
    EXPECT_FALSE(transport.start(&error));
    EXPECT_FALSE(transport.running());
    EXPECT_TRUE(error.contains(QStringLiteral("安全认证与加密")));
}
