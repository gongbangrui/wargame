#include "src/vmf/VmfCodec.h"
#include "src/vmf/VmfProfile.h"

#include <gtest/gtest.h>

#include <QFile>

namespace {

QString designPath(const QString& relative) {
    return QStringLiteral(WARGAME_SOURCE_DIR) + QLatin1Char('/') + relative;
}

gbr::vmf::MessageXml readMessage(const QString& path) {
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    gbr::vmf::MessageXml message;
    QList<gbr::vmf::Diagnostic> diagnostics;
    EXPECT_TRUE(gbr::vmf::MessageXml::parse(file.readAll(), &message, &diagnostics))
        << gbr::vmf::Codec::diagnosticsToString(diagnostics).toStdString();
    return message;
}

} // namespace

TEST(VmfCodec, DesignV1NetworkMonitoringRoundTrip) {
    QList<gbr::vmf::Diagnostic> diagnostics;
    const auto dictionaries = gbr::vmf::VmfProfile::loadDesignV1(
        designPath(QStringLiteral("design/EncoderDecoder")), &diagnostics);
    ASSERT_TRUE(dictionaries)
        << gbr::vmf::Codec::diagnosticsToString(diagnostics).toStdString();

    const auto message = readMessage(designPath(QStringLiteral("design/EncoderDecoder/msg_pass.xml")));
    gbr::vmf::Codec codec(dictionaries);
    const auto validation = codec.validate(message);
    ASSERT_TRUE(validation.valid) << validation.summary().toStdString();

    gbr::vmf::EncodedMessage encoded;
    ASSERT_TRUE(codec.encode(message.message, message, &encoded, &diagnostics));
    EXPECT_EQ(encoded.bitLength, 502);
    EXPECT_EQ(encoded.bytes.size(), 63);

    gbr::vmf::DecodedMessage decoded;
    ASSERT_TRUE(codec.decode(message.message, encoded.bytes, encoded.bitLength,
                             &decoded, &diagnostics));
    EXPECT_EQ(decoded.bitLength, encoded.bitLength);
    EXPECT_EQ(decoded.xml.header.firstChild(QStringLiteral("Field"), QStringLiteral("length"))->text,
              QStringLiteral("63"));
}

TEST(VmfCodec, AcceptsNameOmittedDataUnitsAndRejectsWrongName) {
    QList<gbr::vmf::Diagnostic> diagnostics;
    const auto dictionaries = gbr::vmf::VmfProfile::loadDesignV1(
        designPath(QStringLiteral("design/EncoderDecoder")), &diagnostics);
    ASSERT_TRUE(dictionaries);
    gbr::vmf::Codec codec(dictionaries);

    const auto positive = readMessage(designPath(QStringLiteral("design/EncoderDecoder/pos_msg.xml")));
    EXPECT_TRUE(codec.validate(positive).valid);

    const auto negative = readMessage(designPath(QStringLiteral("design/EncoderDecoder/msg_fail.xml")));
    const auto validation = codec.validate(negative);
    EXPECT_FALSE(validation.valid);
    EXPECT_NE(validation.summary().indexOf(QStringLiteral("VALUE_NAME")), -1);
}

TEST(VmfCodec, ProfileExposesDesignMessages) {
    QList<gbr::vmf::Diagnostic> diagnostics;
    const auto dictionaries = gbr::vmf::VmfProfile::loadDesignV1(
        designPath(QStringLiteral("design/EncoderDecoder")), &diagnostics);
    ASSERT_TRUE(dictionaries);
    const QStringList names = dictionaries->messageNames();
    EXPECT_TRUE(names.contains(QStringLiteral("NetworkMonitoring")));
    EXPECT_TRUE(names.contains(QStringLiteral("Land Route")));
    EXPECT_TRUE(names.contains(QStringLiteral("Target Report")));
}

TEST(VmfCodec, RuleDefinitionsRejectInvalidPathsAndOperators) {
    const QByteArray content = R"xml(
<dic>
  <DFI num="1">
    <DUI num="1" name="Value" bits="8"/>
  </DFI>
</dic>)xml";
    const QByteArray dictionary = R"xml(
<Message name="RuleTest" number="9.9">
  <Header>
    <Field name="version" bits="8"/>
  </Header>
  <Body>
    <DataUnit DFI="1" DUI="1" name="Value" bits="8"/>
  </Body>
  <Rules>
    <ConditionRules>
      <Condition id="bad">
        <If><Cmp path="B.G(Missing)" op="eq" value="1"/></If>
        <Then><Cmp path="B.D(Value)" op="in" targetPath="B.D(Value)"/></Then>
      </Condition>
    </ConditionRules>
  </Rules>
</Message>)xml";

    QList<gbr::vmf::Diagnostic> diagnostics;
    const auto dictionaries = gbr::vmf::DictionarySet::fromXml(dictionary, content, &diagnostics);
    EXPECT_FALSE(dictionaries);
    const QString summary = gbr::vmf::Codec::diagnosticsToString(diagnostics);
    EXPECT_NE(summary.indexOf(QStringLiteral("RULE_PATH")), -1);
    EXPECT_NE(summary.indexOf(QStringLiteral("RULE_OPERATOR")), -1);
}

TEST(VmfCodec, RuleDefinitionsSupportHeaderDataUnitShorthand) {
    const QByteArray content = R"xml(
<dic>
  <DFI num="1">
    <DUI num="1" name="Value" bits="8"/>
  </DFI>
</dic>)xml";
    const QByteArray dictionary = R"xml(
<Message name="RuleTest" number="9.9">
  <Header>
    <Field name="version" bits="8"/>
  </Header>
  <Body>
    <DataUnit DFI="1" DUI="1" name="Value" bits="8"/>
  </Body>
  <Rules>
    <ConditionRules>
      <Condition id="header-version">
        <If><Cmp path="H.D(version)" op="eq" value="1"/></If>
        <Then><Cmp path="B.D(Value)" op="eq" value="7"/></Then>
      </Condition>
    </ConditionRules>
  </Rules>
</Message>)xml";

    QList<gbr::vmf::Diagnostic> diagnostics;
    const auto dictionaries = gbr::vmf::DictionarySet::fromXml(dictionary, content, &diagnostics);
    ASSERT_TRUE(dictionaries)
        << gbr::vmf::Codec::diagnosticsToString(diagnostics).toStdString();
    gbr::vmf::Codec codec(dictionaries);
    gbr::vmf::MessageXml message;
    ASSERT_TRUE(gbr::vmf::MessageXml::parse(
        QByteArrayLiteral("<MessageContent message=\"RuleTest\"><Header><Field name=\"version\">1</Field></Header>"
                          "<Body><DataUnit DFI=\"1\" DUI=\"1\" name=\"Value\">7</DataUnit></Body></MessageContent>"),
        &message, &diagnostics));
    EXPECT_TRUE(codec.validate(message).valid);
}
