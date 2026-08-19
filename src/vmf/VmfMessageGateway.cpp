#include "VmfMessageGateway.h"
#include "VmfRuntimeState.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace {

quint64 stableNumber(const QString& value, quint64 mask) {
    quint64 hash = 1469598103934665603ULL;
    for (const unsigned char byte : value.toUtf8()) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash & mask;
}

gbr::vmf::XmlNode valueNode(const QString& tag, const QString& name,
                            const QString& dfi, const QString& dui, quint64 value) {
    gbr::vmf::XmlNode node;
    node.tag = tag;
    if (!name.isEmpty()) node.attributes.insert(QStringLiteral("name"), name);
    node.attributes.insert(QStringLiteral("DFI"), dfi);
    node.attributes.insert(QStringLiteral("DUI"), dui);
    node.text = QString::number(value);
    return node;
}

void appendValue(QVector<gbr::vmf::XmlNode>* children, const QString& tag,
                 const QString& name, const QString& dfi, const QString& dui,
                 quint64 value) {
    children->append(valueNode(tag, name, dfi, dui, value));
}

gbr::vmf::XmlNode indicator(const QString& tag, const QString& dfi,
                            const QString& dui, quint64 value) {
    return valueNode(tag, {}, dfi, dui, value);
}

void appendHeader(gbr::vmf::MessageXml* message, const gbr::Message& source) {
    message->header.tag = QStringLiteral("Header");
    message->body.tag = QStringLiteral("Body");
    appendValue(&message->header.children, QStringLiteral("Field"), QStringLiteral("version"),
                {}, {}, 1);
    // Codec::encode replaces this value with the final byte length after all
    // fields have been written.
    appendValue(&message->header.children, QStringLiteral("Field"), QStringLiteral("length"),
                {}, {}, 0);
    appendValue(&message->header.children, QStringLiteral("Field"), QStringLiteral("messageId"),
                {}, {}, stableNumber(source.id, 0xffff));
    appendValue(&message->header.children, QStringLiteral("Field"), QStringLiteral("originator"),
                {}, {}, stableNumber(source.sender, 0xffffff));
    appendValue(&message->header.children, QStringLiteral("Field"), QStringLiteral("destination"),
                {}, {}, stableNumber(source.receiver, 0xffffff));
}

QJsonValue firstValue(const QJsonObject& object, std::initializer_list<const char*> names) {
    for (const char* name : names) {
        const QString key = QString::fromLatin1(name);
        if (object.contains(key)) return object.value(key);
    }
    return {};
}

bool finiteNumber(const QJsonValue& value, double* output = nullptr) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number)) return false;
    if (output) *output = number;
    return true;
}

quint64 integerValue(const QJsonObject& object, std::initializer_list<const char*> names,
                     quint64 fallback, quint64 maximum) {
    double number = 0.0;
    if (!finiteNumber(firstValue(object, names), &number)
        || number < 0.0 || std::floor(number) != number) {
        return fallback;
    }
    return std::min(maximum, static_cast<quint64>(number));
}

QString tokenValue(const QJsonObject& object, std::initializer_list<const char*> names) {
    return firstValue(object, names).toString().trimmed().toLower();
}

quint64 enumValue(const QJsonObject& object, std::initializer_list<const char*> names,
                  quint64 fallback, quint64 maximum, const QHash<QString, quint64>& namesToValues) {
    const QJsonValue value = firstValue(object, names);
    double number = 0.0;
    if (finiteNumber(value, &number) && number >= 0.0 && std::floor(number) == number) {
        return std::min(maximum, static_cast<quint64>(number));
    }
    const quint64 mapped = namesToValues.value(value.toString().trimmed().toLower(), fallback);
    return std::min(maximum, mapped);
}

quint64 targetTypeCode(const QJsonObject& target) {
    static const QHash<QString, quint64> values{
        {QStringLiteral("aircraft"), 0}, {QStringLiteral("plane"), 0},
        {QStringLiteral("uav"), 0}, {QStringLiteral("reconuav"), 0},
        {QStringLiteral("attackuav"), 0}, {QStringLiteral("armored"), 1},
        {QStringLiteral("tank"), 1}, {QStringLiteral("vehicle"), 1},
        {QStringLiteral("groundvehicle"), 1}, {QStringLiteral("personnel"), 2},
        {QStringLiteral("infantry"), 2}, {QStringLiteral("groundscout"), 2},
        {QStringLiteral("commandpost"), 3}, {QStringLiteral("jammeruav"), 4},
        {QStringLiteral("unknown"), 7}};
    return enumValue(target, {"targetType", "kind", "type"}, 7, 7, values);
}

quint64 allegianceCode(const QJsonObject& target) {
    static const QHash<QString, quint64> values{
        {QStringLiteral("enemy"), 0}, {QStringLiteral("hostile"), 0},
        {QStringLiteral("foe"), 0}, {QStringLiteral("friendly"), 1},
        {QStringLiteral("friend"), 1}, {QStringLiteral("own"), 1},
        {QStringLiteral("neutral"), 2}, {QStringLiteral("unknown"), 3}};
    return enumValue(target, {"friendFoe", "allegiance", "iff", "targetSide"},
                     3, 3, values);
}

quint64 statusCode(const QJsonObject& target, gbr::Message::Type type) {
    static const QHash<QString, quint64> values{
        {QStringLiteral("intact"), 0}, {QStringLiteral("healthy"), 0},
        {QStringLiteral("operational"), 0}, {QStringLiteral("alive"), 0},
        {QStringLiteral("damaged"), 1}, {QStringLiteral("hit"), 1},
        {QStringLiteral("destroyed"), 2}, {QStringLiteral("dead"), 2},
        {QStringLiteral("unknown"), 3}};
    if (target.contains(QStringLiteral("alive")) && !target.value(QStringLiteral("alive")).toBool()) {
        return 2;
    }
    if (type == gbr::Message::Type::TargetDestroyed) return 2;
    if (type == gbr::Message::Type::EngagementReport
        && target.value(QStringLiteral("outcome")).toString() == QLatin1String("hit")) {
        return 1;
    }
    return enumValue(target, {"targetStatus", "status", "outcome"}, 3, 3, values);
}

quint64 quantize(double value, double minimum, double maximum, int bits) {
    if (!std::isfinite(value) || !std::isfinite(minimum) || !std::isfinite(maximum)
        || maximum <= minimum) return 0;
    const double normalized = std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0);
    const quint64 maximumCode = (quint64{1} << bits) - 1U;
    return static_cast<quint64>(std::llround(normalized * static_cast<double>(maximumCode)));
}

std::optional<gbr::GeoCoord> resolvedGeoCoordinate(
    const QJsonObject& point,
    const gbr::vmf::VmfMessageGateway::CoordinateResolver& resolver) {
    double x = 0.0;
    double y = 0.0;
    if (!finiteNumber(point.value(QStringLiteral("x")), &x)
        || !finiteNumber(point.value(QStringLiteral("y")), &y)
        || !resolver) {
        return std::nullopt;
    }
    const std::optional<gbr::GeoCoord> coordinate = resolver(x, y);
    if (!coordinate.has_value() || !std::isfinite(coordinate->lat)
        || !std::isfinite(coordinate->lon) || coordinate->lat < -90.0
        || coordinate->lat > 90.0 || coordinate->lon < -180.0
        || coordinate->lon > 180.0) {
        return std::nullopt;
    }
    return coordinate;
}

quint64 coordinateCode(const QJsonObject& point, bool latitude,
                       const gbr::vmf::VmfMessageGateway::CoordinateResolver& resolver) {
    const QJsonValue geographic = firstValue(point, latitude ?
                                              std::initializer_list<const char*>{"latitude", "lat"} :
                                              std::initializer_list<const char*>{"longitude", "lon"});
    double degrees = 0.0;
    if (finiteNumber(geographic, &degrees)) {
        return quantize(degrees, latitude ? -90.0 : -180.0,
                        latitude ? 90.0 : 180.0, latitude ? 22 : 23);
    }
    if (const std::optional<gbr::GeoCoord> coordinate = resolvedGeoCoordinate(point, resolver);
        coordinate.has_value()) {
        return quantize(latitude ? coordinate->lat : coordinate->lon,
                        latitude ? -90.0 : -180.0,
                        latitude ? 90.0 : 180.0, latitude ? 22 : 23);
    }
    // The simulator uses a bounded local metre grid.  If no valid GIS
    // alignment is available, preserve the logical point in the documented
    // envelope and expose the fallback source in the domain payload.
    const QJsonValue local = firstValue(point, latitude ?
                                        std::initializer_list<const char*>{"y", "northing"} :
                                        std::initializer_list<const char*>{"x", "easting"});
    finiteNumber(local, &degrees);
    return quantize(degrees, 0.0, 1'000'000.0, latitude ? 22 : 23);
}

bool hasMotion(const QJsonObject& target) {
    return target.contains(QStringLiteral("headingDeg"))
        || target.contains(QStringLiteral("heading"))
        || target.contains(QStringLiteral("headingRad"));
}

quint64 directionCode(const QJsonObject& target) {
    double degrees = 0.0;
    if (finiteNumber(target.value(QStringLiteral("headingDeg")), &degrees)
        || finiteNumber(target.value(QStringLiteral("heading")), &degrees)) {
        // already expressed in degrees
    } else if (finiteNumber(target.value(QStringLiteral("headingRad")), &degrees)) {
        degrees = degrees * 180.0 / M_PI;
    } else {
        return 0;
    }
    degrees = std::fmod(degrees, 360.0);
    if (degrees < 0.0) degrees += 360.0;
    return quantize(degrees, 0.0, 360.0, 9);
}

void appendObservationTime(gbr::vmf::XmlNode* parent, const QDateTime& input) {
    gbr::vmf::XmlNode time;
    time.tag = QStringLiteral("Group");
    time.attributes.insert(QStringLiteral("name"), QStringLiteral("ObservationTime"));
    time.children.append(indicator(QStringLiteral("GPI"), QStringLiteral("4014"),
                                   QStringLiteral("001"), 1));
    const QDateTime timestamp = input.isValid() ? input.toUTC() : QDateTime::currentDateTimeUtc();
    appendValue(&time.children, QStringLiteral("DataUnit"), QStringLiteral("Month"),
                QStringLiteral("4099"), QStringLiteral("001"), timestamp.date().month());
    appendValue(&time.children, QStringLiteral("DataUnit"), QStringLiteral("DayOfMonth"),
                QStringLiteral("4019"), QStringLiteral("001"), timestamp.date().day());
    appendValue(&time.children, QStringLiteral("DataUnit"), QStringLiteral("Hour"),
                QStringLiteral("792"), QStringLiteral("001"), timestamp.time().hour());
    appendValue(&time.children, QStringLiteral("DataUnit"), QStringLiteral("Minute"),
                QStringLiteral("797"), QStringLiteral("004"), timestamp.time().minute());
    appendValue(&time.children, QStringLiteral("DataUnit"), QStringLiteral("DataCollectionSecond"),
                QStringLiteral("380"), QStringLiteral("417"), timestamp.time().second());
    parent->children.append(std::move(time));
}

bool targetReportXml(
    const gbr::Message& source, gbr::vmf::MessageXml* output,
    const gbr::vmf::VmfMessageGateway::CoordinateResolver& coordinateResolver) {
    gbr::vmf::MessageXml message;
    message.message = QStringLiteral("Target Report");
    appendHeader(&message, source);
    QVector<QJsonObject> targetObjects;
    const QJsonArray array = source.payload.value(QStringLiteral("targets")).toArray();
    if (!array.isEmpty()) {
        for (const QJsonValue& value : array) {
            if (value.isObject()) targetObjects.append(value.toObject());
            if (targetObjects.size() == 8) break;
        }
    }
    if (targetObjects.isEmpty()) targetObjects.append(source.payload);
    QVector<gbr::vmf::XmlNode> groups;
    int index = 0;
    for (const QJsonObject& target : targetObjects) {
        gbr::vmf::XmlNode group;
        group.tag = QStringLiteral("Group");
        group.attributes.insert(QStringLiteral("name"), QStringLiteral("TargetReport"));
        group.children.append(indicator(QStringLiteral("GRI"), QStringLiteral("4045"),
                                        QStringLiteral("001"), index + 1 < targetObjects.size()));
        appendValue(&group.children, QStringLiteral("DataUnit"), QStringLiteral("TargetType"),
                    QStringLiteral("4200"), QStringLiteral("001"), targetTypeCode(target));
        appendValue(&group.children, QStringLiteral("DataUnit"), QStringLiteral("TargetQuantity"),
                    QStringLiteral("4201"), QStringLiteral("001"),
                    integerValue(target, {"targetCount", "count", "quantity"}, 1, 255));
        appendValue(&group.children, QStringLiteral("DataUnit"), QStringLiteral("IdentificationFriendOrFoe"),
                    QStringLiteral("4202"), QStringLiteral("001"), allegianceCode(target));
        appendValue(&group.children, QStringLiteral("DataUnit"), QStringLiteral("Latitude00051"),
                    QStringLiteral("281"), QStringLiteral("014"),
                    coordinateCode(target, true, coordinateResolver));
        appendValue(&group.children, QStringLiteral("DataUnit"), QStringLiteral("Longtitude00051"),
                    QStringLiteral("282"), QStringLiteral("014"),
                    coordinateCode(target, false, coordinateResolver));
        appendValue(&group.children, QStringLiteral("DataUnit"), QStringLiteral("TargetStatus"),
                    QStringLiteral("4203"), QStringLiteral("001"), statusCode(target, source.type));
        gbr::vmf::XmlNode motion;
        motion.tag = QStringLiteral("Field");
        motion.attributes.insert(QStringLiteral("name"), QStringLiteral("Motion"));
        motion.children.append(indicator(QStringLiteral("FPI"), QStringLiteral("4014"),
                                         QStringLiteral("002"), hasMotion(target)));
        if (hasMotion(target)) {
            appendValue(&motion.children, QStringLiteral("DataUnit"), QStringLiteral("Direction"),
                        QStringLiteral("4204"), QStringLiteral("001"), directionCode(target));
        }
        group.children.append(std::move(motion));
        appendObservationTime(&group, source.timestamp);
        groups.append(std::move(group));
        ++index;
    }
    message.body.children = std::move(groups);
    *output = std::move(message);
    return true;
}

QJsonArray routePoints(const QJsonObject& payload) {
    QJsonArray points = payload.value(QStringLiteral("waypoints")).toArray();
    if (!points.isEmpty()) return points;
    if (payload.contains(QStringLiteral("x")) && payload.contains(QStringLiteral("y"))) {
        points.append(QJsonObject{{QStringLiteral("x"), payload.value(QStringLiteral("x"))},
                                  {QStringLiteral("y"), payload.value(QStringLiteral("y"))}});
    } else if (payload.contains(QStringLiteral("homeX"))
               && payload.contains(QStringLiteral("homeY"))) {
        points.append(QJsonObject{{QStringLiteral("x"), payload.value(QStringLiteral("homeX"))},
                                  {QStringLiteral("y"), payload.value(QStringLiteral("homeY"))}});
    }
    return points;
}

bool validRoutePoint(const QJsonValue& value) {
    if (!value.isObject()) return false;
    const QJsonObject point = value.toObject();
    double coordinate = 0.0;
    const bool hasGeo = finiteNumber(firstValue(point, {"latitude", "lat"}), &coordinate)
        && finiteNumber(firstValue(point, {"longitude", "lon"}), &coordinate);
    const bool hasLocal = finiteNumber(point.value(QStringLiteral("x")), &coordinate)
        && finiteNumber(point.value(QStringLiteral("y")), &coordinate);
    return hasGeo || hasLocal;
}

QString pointCoordinateSource(
    const QJsonObject& point,
    const gbr::vmf::VmfMessageGateway::CoordinateResolver& coordinateResolver) {
    double latitude = 0.0;
    double longitude = 0.0;
    if (finiteNumber(firstValue(point, {"latitude", "lat"}), &latitude)
        && finiteNumber(firstValue(point, {"longitude", "lon"}), &longitude)
        && latitude >= -90.0 && latitude <= 90.0
        && longitude >= -180.0 && longitude <= 180.0) {
        return QStringLiteral("geo");
    }
    double x = 0.0;
    double y = 0.0;
    if (finiteNumber(point.value(QStringLiteral("x")), &x)
        && finiteNumber(point.value(QStringLiteral("y")), &y)) {
        return resolvedGeoCoordinate(point, coordinateResolver).has_value()
            ? QStringLiteral("map-gis") : QStringLiteral("logical-grid-fallback");
    }
    return QStringLiteral("none");
}

QString messageCoordinateSource(
    const gbr::Message& source,
    const gbr::vmf::VmfMessageGateway::CoordinateResolver& coordinateResolver) {
    QVector<QJsonObject> points;
    if (source.type == gbr::Message::Type::TargetReport
        || source.type == gbr::Message::Type::TargetDetect
        || source.type == gbr::Message::Type::TargetTrack
        || source.type == gbr::Message::Type::PositionReport
        || source.type == gbr::Message::Type::TargetDestroyed
        || source.type == gbr::Message::Type::EngagementReport
        || source.type == gbr::Message::Type::SharedDetect) {
        const QJsonArray targets = source.payload.value(QStringLiteral("targets")).toArray();
        for (const QJsonValue& value : targets) {
            if (value.isObject()) points.append(value.toObject());
        }
        if (points.isEmpty()) points.append(source.payload);
    } else {
        const QJsonArray route = routePoints(source.payload);
        for (const QJsonValue& value : route) {
            if (value.isObject()) points.append(value.toObject());
        }
        const QJsonArray critical = source.payload.value(QStringLiteral("criticalPoints")).toArray();
        for (const QJsonValue& value : critical) {
            if (value.isObject()) points.append(value.toObject());
        }
    }
    QString sourceName;
    for (const QJsonObject& point : points) {
        const QString current = pointCoordinateSource(point, coordinateResolver);
        if (current == QLatin1String("none")) continue;
        if (sourceName.isEmpty()) sourceName = current;
        else if (sourceName != current) return QStringLiteral("mixed");
    }
    return sourceName.isEmpty() ? QStringLiteral("none") : sourceName;
}

bool landRouteXml(
    const gbr::Message& source, gbr::vmf::MessageXml* output,
    const gbr::vmf::VmfMessageGateway::CoordinateResolver& coordinateResolver) {
    const QJsonArray rawPoints = routePoints(source.payload);
    QJsonArray points;
    for (const QJsonValue& point : rawPoints) {
        if (validRoutePoint(point)) points.append(point);
        if (points.size() == 32) break;
    }
    if (points.isEmpty()) return false;

    gbr::vmf::MessageXml message;
    message.message = QStringLiteral("Land Route");
    appendHeader(&message, source);
    gbr::vmf::XmlNode route;
    route.tag = QStringLiteral("Group");
    route.attributes.insert(QStringLiteral("name"), QStringLiteral("MultipleRoute"));
    route.children.append(indicator(QStringLiteral("GRI"), QStringLiteral("4045"),
                                    QStringLiteral("001"), 0));
    for (int i = 0; i < points.size(); ++i) {
        const QJsonObject point = points.at(i).toObject();
        gbr::vmf::XmlNode extremity;
        extremity.tag = QStringLiteral("Group");
        extremity.attributes.insert(QStringLiteral("name"), QStringLiteral("RouteExtremeties"));
        extremity.children.append(indicator(QStringLiteral("GRI"), QStringLiteral("4045"),
                                            QStringLiteral("001"), i + 1 < points.size()));
        appendValue(&extremity.children, QStringLiteral("DataUnit"), QStringLiteral("Latitude00051"),
                    QStringLiteral("281"), QStringLiteral("014"),
                    coordinateCode(point, true, coordinateResolver));
        appendValue(&extremity.children, QStringLiteral("DataUnit"), QStringLiteral("Longtitude00051"),
                    QStringLiteral("282"), QStringLiteral("014"),
                    coordinateCode(point, false, coordinateResolver));
        route.children.append(std::move(extremity));
    }
    gbr::vmf::XmlNode reportTime;
    reportTime.tag = QStringLiteral("Group");
    reportTime.attributes.insert(QStringLiteral("name"), QStringLiteral("ReportTime"));
    reportTime.children.append(indicator(QStringLiteral("GPI"), QStringLiteral("4014"),
                                         QStringLiteral("001"), 1));
    const QDateTime timestamp = source.timestamp.isValid()
        ? source.timestamp.toUTC() : QDateTime::currentDateTimeUtc();
    appendValue(&reportTime.children, QStringLiteral("DataUnit"), QStringLiteral("Month"),
                QStringLiteral("4099"), QStringLiteral("001"), timestamp.date().month());
    appendValue(&reportTime.children, QStringLiteral("DataUnit"), QStringLiteral("DayOfMonth"),
                QStringLiteral("4019"), QStringLiteral("001"), timestamp.date().day());
    appendValue(&reportTime.children, QStringLiteral("DataUnit"), QStringLiteral("Hour"),
                QStringLiteral("792"), QStringLiteral("001"), timestamp.time().hour());
    appendValue(&reportTime.children, QStringLiteral("DataUnit"), QStringLiteral("Minute"),
                QStringLiteral("797"), QStringLiteral("004"), timestamp.time().minute());
    route.children.append(std::move(reportTime));

    const QJsonArray critical = source.payload.value(QStringLiteral("criticalPoints")).toArray();
    int validCritical = 0;
    for (const QJsonValue& value : critical) if (validRoutePoint(value)) ++validCritical;
    {
        const int emittedCritical = std::min(validCritical, 32);
        gbr::vmf::XmlNode routeData;
        routeData.tag = QStringLiteral("Group");
        routeData.attributes.insert(QStringLiteral("name"), QStringLiteral("RouteData"));
        // RouteData is a selectable container in msg4_2.xml.  The container
        // itself must be present even when GPI=0; omitting it shifts the
        // dictionary walk and is not equivalent to an absent critical-point
        // list.
        routeData.children.append(indicator(QStringLiteral("GPI"), QStringLiteral("4014"),
                                             QStringLiteral("001"), validCritical > 0));
        int current = 0;
        for (const QJsonValue& value : critical) {
            if (!validRoutePoint(value) || current == 32) continue;
            const QJsonObject point = value.toObject();
            gbr::vmf::XmlNode item;
            item.tag = QStringLiteral("Group");
            item.attributes.insert(QStringLiteral("name"), QStringLiteral("CriticalPoints"));
            item.children.append(indicator(QStringLiteral("GRI"), QStringLiteral("4045"),
                                           QStringLiteral("001"), current + 1 < emittedCritical));
            appendValue(&item.children, QStringLiteral("DataUnit"), QStringLiteral("Latitude00051"),
                        QStringLiteral("281"), QStringLiteral("014"),
                        coordinateCode(point, true, coordinateResolver));
            appendValue(&item.children, QStringLiteral("DataUnit"), QStringLiteral("Longtitude00051"),
                        QStringLiteral("282"), QStringLiteral("014"),
                        coordinateCode(point, false, coordinateResolver));
            routeData.children.append(std::move(item));
            ++current;
        }
        route.children.append(std::move(routeData));
    }
    message.body.children.append(std::move(route));
    *output = std::move(message);
    return true;
}

} // namespace

namespace gbr::vmf {

VmfMessageGateway::VmfMessageGateway(MessageBus* bus,
                                     std::shared_ptr<const DictionarySet> dictionaries,
                                     std::shared_ptr<const VmfMessageCatalog> catalog,
                                     QObject* parent)
    : QObject(parent), m_bus(bus), m_dictionaries(std::move(dictionaries)),
      m_catalog(catalog ? std::move(catalog) : VmfMessageCatalog::designV1()) {}

QString VmfMessageGateway::messageNameForType(Message::Type type,
                                               const QJsonObject& payload) {
    switch (type) {
    case Message::Type::PositionReport:
    case Message::Type::TargetDetect:
    case Message::Type::TargetReport:
    case Message::Type::TargetTrack:
    case Message::Type::TargetDestroyed:
    case Message::Type::EngagementReport:
    case Message::Type::SharedDetect:
        return QStringLiteral("Target Report");
    case Message::Type::StrikePlan:
    case Message::Type::FlightPlan:
    case Message::Type::GroundAttackConfirm:
    case Message::Type::Guidance:
    case Message::Type::Withdraw:
    case Message::Type::WithdrawOrder:
    case Message::Type::Pursue:
        return QStringLiteral("Land Route");
    case Message::Type::Ack:
        return QStringLiteral("NetworkMonitoring");
    case Message::Type::AttackOrder:
        // A point attack is a route with one extremity; a target-only order
        // remains on the generic network-monitoring carrier.
        if (!routePoints(payload).isEmpty()) return QStringLiteral("Land Route");
        return QStringLiteral("NetworkMonitoring");
    default:
        return QStringLiteral("NetworkMonitoring");
    }
}

bool VmfMessageGateway::isMessageNameCompatible(const Message& message) {
    if (message.wireFormat != Message::WireFormat::VmfDesignV1
        || message.vmfMessage.isEmpty()) return false;
    return message.vmfMessage == messageNameForType(message.type, message.payload);
}

QJsonObject VmfMessageGateway::traceSummary(const Message& message,
                                            const EncodedMessage& encoded) {
    QJsonObject result;
    result.insert(QStringLiteral("messageId"), message.id);
    result.insert(QStringLiteral("traceId"), message.traceId);
    result.insert(QStringLiteral("correlationId"), message.correlationId);
    result.insert(QStringLiteral("sender"), message.sender);
    result.insert(QStringLiteral("receiver"), message.receiver);
    result.insert(QStringLiteral("vmfMessage"), encoded.message);
    result.insert(QStringLiteral("wireFormat"), Message::wireFormatName(message.wireFormat));
    result.insert(QStringLiteral("bitLength"), encoded.bitLength);
    result.insert(QStringLiteral("byteLength"), encoded.bytes.size());
    result.insert(QStringLiteral("fieldCount"), encoded.fields.size());
    result.insert(QStringLiteral("validated"), true);
    result.insert(QStringLiteral("retryCount"), message.retryCount);
    if (message.payload.contains(QStringLiteral("vmfCatalogId"))) {
        result.insert(QStringLiteral("catalogId"),
                     message.payload.value(QStringLiteral("vmfCatalogId")));
    }
    if (message.payload.contains(QStringLiteral("vmfTrigger"))) {
        result.insert(QStringLiteral("trigger"),
                     message.payload.value(QStringLiteral("vmfTrigger")));
    }
    if (message.payload.contains(QStringLiteral("vmfInformationValue"))) {
        result.insert(QStringLiteral("informationValue"),
                     message.payload.value(QStringLiteral("vmfInformationValue")));
    }
    return result;
}

bool VmfMessageGateway::domainMessageXml(
    const Message& message, MessageXml* xml, QString* messageName,
    const CoordinateResolver& coordinateResolver, QString* error) {
    if (!xml || !messageName) {
        if (error) *error = QStringLiteral("VMF domain 映射输出参数为空");
        return false;
    }
    const QString selectedMessage = messageNameForType(message.type, message.payload);
    if (selectedMessage == QLatin1String("Target Report")) {
        if (!targetReportXml(message, xml, coordinateResolver)) {
            if (error) *error = QStringLiteral("目标报告 VMF 字段映射失败");
            return false;
        }
        *messageName = selectedMessage;
        return true;
    }
    if (selectedMessage == QLatin1String("Land Route")) {
        if (!landRouteXml(message, xml, coordinateResolver)) {
            if (error) *error = QStringLiteral("航路 VMF 字段缺少有效航点");
            return false;
        }
        *messageName = selectedMessage;
        return true;
    }

    MessageXml mapped;
    mapped.message = QStringLiteral("NetworkMonitoring");
    mapped.header.tag = QStringLiteral("Header");
    mapped.body.tag = QStringLiteral("Body");
    // NetworkMonitoring is the common design-profile carrier.  The domain
    // payload remains in the local Message object; only bounded numeric
    // identifiers are placed on the bit wire.
    appendValue(&mapped.header.children, QStringLiteral("Field"), QStringLiteral("version"),
                {}, {}, 1);
    appendValue(&mapped.header.children, QStringLiteral("Field"), QStringLiteral("length"),
                {}, {}, 0);
    appendValue(&mapped.header.children, QStringLiteral("Field"), QStringLiteral("messageId"),
                {}, {}, stableNumber(message.id, 0xffff));
    appendValue(&mapped.header.children, QStringLiteral("Field"), QStringLiteral("originator"),
                {}, {}, stableNumber(message.sender, 0xffffff));
    appendValue(&mapped.header.children, QStringLiteral("Field"), QStringLiteral("destination"),
                {}, {}, stableNumber(message.receiver, 0xffffff));

    const quint64 typeCode = static_cast<quint64>(message.type) & 0x3U;
    // ProtocolType's design dictionary intentionally reserves value 3.  The
    // domain enum is larger than the two-bit VMF message-type field, so map
    // the compact protocol discriminator into the next valid design value
    // instead of allowing a command such as Halt (enum value 19) to fail
    // encoding solely because 19 & 0x7 == 3.
    const quint64 protocolType = (static_cast<quint64>(message.type) & 0x7U) == 3U
        ? 4U : (static_cast<quint64>(message.type) & 0x7U);
    appendValue(&mapped.body.children, QStringLiteral("DataUnit"), QStringLiteral("NumberOfSubNetworks"),
                QStringLiteral("4029"), QStringLiteral("125"), 1);
    appendValue(&mapped.body.children, QStringLiteral("DataUnit"), QStringLiteral("NetworkMonitoringMessageType"),
                QStringLiteral("4093"), QStringLiteral("042"), typeCode);
    appendValue(&mapped.body.children, QStringLiteral("DataUnit"), QStringLiteral("URN"),
                QStringLiteral("4004"), QStringLiteral("012"), stableNumber(message.sender, 0xffffff));

    XmlNode info;
    info.tag = QStringLiteral("Group");
    info.attributes.insert(QStringLiteral("name"), QStringLiteral("NetworkInfo"));
    info.children.append(indicator(QStringLiteral("GRI"), QStringLiteral("4045"),
                                   QStringLiteral("001"), 0));
    appendValue(&info.children, QStringLiteral("DataUnit"), QStringLiteral("NetworkIdNumber"),
                QStringLiteral("4085"), QStringLiteral("059"),
                static_cast<quint64>(message.type) & 0xffffU);
    XmlNode time;
    time.tag = QStringLiteral("Group");
    time.attributes.insert(QStringLiteral("name"), QStringLiteral("DataCollectionTime"));
    time.children.append(indicator(QStringLiteral("GRI"), QStringLiteral("4045"),
                                   QStringLiteral("001"), 0));
    const QDateTime timestamp = message.timestamp.isValid()
        ? message.timestamp.toUTC() : QDateTime::currentDateTimeUtc();
    appendValue(&time.children, QStringLiteral("DataUnit"), QStringLiteral("DataCollectionDay"),
                QStringLiteral("4019"), QStringLiteral("044"), timestamp.date().day() & 0x1f);
    appendValue(&time.children, QStringLiteral("DataUnit"), QStringLiteral("DataCollectionHour"),
                QStringLiteral("792"), QStringLiteral("452"), timestamp.time().hour() & 0x1f);
    appendValue(&time.children, QStringLiteral("DataUnit"), QStringLiteral("DataCollectionMinutes"),
                QStringLiteral("797"), QStringLiteral("449"), timestamp.time().minute() & 0x3f);
    appendValue(&time.children, QStringLiteral("DataUnit"), QStringLiteral("DataCollectionSecond"),
                QStringLiteral("380"), QStringLiteral("417"), timestamp.time().second() & 0x3f);
    info.children.append(std::move(time));

    XmlNode characteristics;
    characteristics.tag = QStringLiteral("Group");
    characteristics.attributes.insert(QStringLiteral("name"), QStringLiteral("NetworkCharacteristics"));
    characteristics.children.append(indicator(QStringLiteral("GPI"), QStringLiteral("4014"),
                                               QStringLiteral("001"), 1));
    appendValue(&characteristics.children, QStringLiteral("DataUnit"), QStringLiteral("DataMeasurementIndicator"),
                QStringLiteral("4093"), QStringLiteral("044"), 1);
    XmlNode networkType;
    networkType.tag = QStringLiteral("Group");
    networkType.attributes.insert(QStringLiteral("name"), QStringLiteral("NetworkType"));
    networkType.children.append(indicator(QStringLiteral("GRI"), QStringLiteral("4045"),
                                           QStringLiteral("001"), 0));
    appendValue(&networkType.children, QStringLiteral("DataUnit"), QStringLiteral("ProtocolType"),
                QStringLiteral("4093"), QStringLiteral("045"), protocolType);
    XmlNode traffic;
    traffic.tag = QStringLiteral("Group");
    traffic.attributes.insert(QStringLiteral("name"), QStringLiteral("TrafficStatus"));
    traffic.children.append(indicator(QStringLiteral("GRI"), QStringLiteral("4045"),
                                      QStringLiteral("001"), 0));
    appendValue(&traffic.children, QStringLiteral("DataUnit"), QStringLiteral("TrafficType"),
                QStringLiteral("4093"), QStringLiteral("046"), typeCode & 0x3U);
    appendValue(&traffic.children, QStringLiteral("DataUnit"), QStringLiteral("CurrentNetworkLoad"),
                QStringLiteral("4082"), QStringLiteral("012"), message.payload.size() & 0xfU);
    appendValue(&traffic.children, QStringLiteral("DataUnit"), QStringLiteral("AverageNetworkLoad"),
                QStringLiteral("4082"), QStringLiteral("013"), message.retryCount & 0xfU);
    networkType.children.append(std::move(traffic));
    characteristics.children.append(std::move(networkType));
    XmlNode serverId;
    serverId.tag = QStringLiteral("Field");
    serverId.attributes.insert(QStringLiteral("name"), QStringLiteral("ServerId"));
    serverId.children.append(indicator(QStringLiteral("FPI"), QStringLiteral("4014"),
                                       QStringLiteral("002"), 1));
    appendValue(&serverId.children, QStringLiteral("DataUnit"), QStringLiteral("URN"),
                QStringLiteral("4004"), QStringLiteral("012"),
                stableNumber(message.receiver, 0xffffff));
    characteristics.children.append(std::move(serverId));
    appendValue(&characteristics.children, QStringLiteral("DataUnit"), QStringLiteral("NetworkStatus"),
                QStringLiteral("4082"), QStringLiteral("014"), 1);
    XmlNode serverUsage;
    serverUsage.tag = QStringLiteral("Group");
    serverUsage.attributes.insert(QStringLiteral("name"), QStringLiteral("ServerNetworkUsage"));
    serverUsage.children.append(indicator(QStringLiteral("GPI"), QStringLiteral("4014"),
                                          QStringLiteral("001"), 0));
    characteristics.children.append(std::move(serverUsage));
    info.children.append(std::move(characteristics));
    mapped.body.children.append(std::move(info));
    *xml = std::move(mapped);
    *messageName = selectedMessage;
    return true;
}

bool VmfMessageGateway::prepareDomainMessage(const Message& input, Message* output,
                                             QString* error) const {
    if (error) error->clear();
    if (!output || !m_dictionaries) {
        if (error) *error = QStringLiteral("VMF 网关未初始化");
        return false;
    }
    MessageXml xml;
    QString messageName;
    if (!domainMessageXml(input, &xml, &messageName, m_coordinateResolver, error)) return false;
    if (!m_catalog || !m_catalog->validate(Message::typeName(input.type), messageName,
                                           input.payload, {}, {}, error)) {
        if (error && error->isEmpty()) {
            *error = QStringLiteral("VMF 消息目录没有匹配领域映射");
        }
        return false;
    }
    const auto catalogEntry = m_catalog->entryFor(Message::typeName(input.type), messageName,
                                                   input.payload);
    if (!catalogEntry.has_value()) {
        if (error) *error = QStringLiteral("VMF 消息目录条目缺失");
        return false;
    }
    Codec codec(m_dictionaries);
    EncodedMessage encoded;
    QList<Diagnostic> diagnostics;
    if (!codec.encode(messageName, xml, &encoded, &diagnostics)) {
        if (error) *error = Codec::diagnosticsToString(diagnostics);
        return false;
    }
    DecodedMessage decoded;
    if (!codec.decode(messageName, encoded.bytes, encoded.bitLength, &decoded, &diagnostics)) {
        if (error) *error = Codec::diagnosticsToString(diagnostics);
        return false;
    }
    Message mapped = input;
    mapped.wireFormat = Message::WireFormat::VmfDesignV1;
    mapped.vmfMessage = messageName;
    mapped.wireBytes = encoded.bytes;
    mapped.wireBitLength = encoded.bitLength;
    mapped.requiresAck = catalogEntry->requiresAck;
    mapped.automaticAck = catalogEntry->automaticAck && m_automaticAckEnabled;
    mapped.payload.insert(QStringLiteral("vmfValidated"), true);
    mapped.payload.insert(QStringLiteral("vmfFieldCount"), encoded.fields.size());
    mapped.payload.insert(QStringLiteral("vmfCoordinateSource"),
                          messageCoordinateSource(input, m_coordinateResolver));
    mapped.payload.insert(QStringLiteral("vmfCatalogId"), catalogEntry->catalogId);
    mapped.payload.insert(QStringLiteral("vmfTrigger"), catalogEntry->trigger);
    mapped.payload.insert(QStringLiteral("vmfInformationValue"),
                          catalogEntry->informationValue.toJson());
    *output = std::move(mapped);
    return true;
}

void VmfMessageGateway::rememberTrace(const QJsonObject& trace) {
    m_recentTraces.append(trace);
    while (m_recentTraces.size() > 200) m_recentTraces.removeFirst();
}

bool VmfMessageGateway::restoreTraceSummaries(const QJsonArray& traces, QString* error) {
    if (error) error->clear();
    RuntimeState state;
    state.traceSummaries = traces;
    QString validationError;
    if (!state.validate(&validationError)) {
        if (error) *error = validationError;
        return false;
    }
    m_recentTraces = traces;
    return true;
}

bool VmfMessageGateway::send(const QString& messageName, const MessageXml& xml,
                             Message message, EncodedMessage* encodedOutput,
                             QString* error) {
    if (error) error->clear();
    if (!m_bus || !m_dictionaries) {
        if (error) *error = QStringLiteral("VMF 网关未初始化");
        emit diagnostic(QStringLiteral("NO_GATEWAY"), {}, QStringLiteral("VMF 网关未初始化"));
        return false;
    }

    Codec codec(m_dictionaries);
    EncodedMessage encoded;
    QList<Diagnostic> diagnostics;
    if (!codec.encode(messageName, xml, &encoded, &diagnostics)) {
        const QString detail = Codec::diagnosticsToString(diagnostics);
        if (error) *error = detail;
        for (const Diagnostic& item : diagnostics) emit diagnostic(item.code, item.path, item.message);
        return false;
    }
    DecodedMessage decoded;
    if (!codec.decode(messageName, encoded.bytes, encoded.bitLength, &decoded, &diagnostics)) {
        const QString detail = Codec::diagnosticsToString(diagnostics);
        if (error) *error = detail;
        for (const Diagnostic& item : diagnostics) emit diagnostic(item.code, item.path, item.message);
        return false;
    }

    message.wireFormat = Message::WireFormat::VmfDesignV1;
    message.vmfMessage = messageName;
    message.wireBytes = encoded.bytes;
    message.wireBitLength = encoded.bitLength;
    if (m_catalog) {
        const auto entry = m_catalog->entryFor(Message::typeName(message.type), messageName,
                                               message.payload);
        if (!entry.has_value()) {
            if (error) *error = QStringLiteral("VMF 消息目录没有匹配领域映射");
            return false;
        }
        message.requiresAck = entry->requiresAck;
        message.automaticAck = entry->automaticAck && m_automaticAckEnabled;
        message.payload.insert(QStringLiteral("vmfCatalogId"), entry->catalogId);
        message.payload.insert(QStringLiteral("vmfTrigger"), entry->trigger);
        message.payload.insert(QStringLiteral("vmfInformationValue"),
                               entry->informationValue.toJson());
    }
    if (message.traceId.isEmpty()) {
        message.traceId = QStringLiteral("vmf-%1").arg(++m_traceSequence);
    }
    message.payload.insert(QStringLiteral("vmfValidated"), true);
    message.payload.insert(QStringLiteral("vmfFieldCount"), encoded.fields.size());
    const QJsonObject trace = traceSummary(message, encoded);
    rememberTrace(trace);
    emit vmfDelivered(trace);
    if (encodedOutput) *encodedOutput = encoded;
    m_bus->send(message);
    return true;
}

bool VmfMessageGateway::decode(const Message& message, DecodedMessage* decoded,
                               QList<Diagnostic>* diagnostics) const {
    if (!decoded || !m_dictionaries) {
        if (diagnostics) diagnostics->append({Diagnostic::Severity::Error,
                                              QStringLiteral("NO_GATEWAY"), {},
                                              QStringLiteral("VMF 网关未初始化")});
        return false;
    }
    if (message.wireFormat != Message::WireFormat::VmfDesignV1
        || message.vmfMessage.isEmpty() || message.wireBytes.isEmpty()) {
        if (diagnostics) diagnostics->append({Diagnostic::Severity::Error,
                                              QStringLiteral("WIRE_FORMAT"), {},
                                              QStringLiteral("消息不是完整的 VMF 设计格式")});
        return false;
    }
    if (!isMessageNameCompatible(message)) {
        if (diagnostics) diagnostics->append({Diagnostic::Severity::Error,
                                              QStringLiteral("MESSAGE_NAME_MISMATCH"), {},
                                              QStringLiteral("VMF 消息名与领域消息类型不匹配")});
        return false;
    }
    if (!validateCatalog(message, {}, {}, nullptr)) {
        if (diagnostics) diagnostics->append({Diagnostic::Severity::Error,
                                              QStringLiteral("CATALOG_MISMATCH"), {},
                                              QStringLiteral("消息未通过 VMF 消息目录语义校验")});
        return false;
    }
    Codec codec(m_dictionaries);
    return codec.decode(message.vmfMessage, message.wireBytes, message.wireBitLength,
                        decoded, diagnostics);
}

bool VmfMessageGateway::validateCatalog(const Message& message, const QString& senderRole,
                                        const QString& receiverRole, QString* error) const {
    if (error) error->clear();
    if (!m_catalog) {
        if (error) *error = QStringLiteral("VMF 消息目录未加载");
        return false;
    }
    return m_catalog->validate(Message::typeName(message.type), message.vmfMessage,
                               message.payload, senderRole, receiverRole, error);
}

QJsonObject VmfMessageGateway::catalogSummary(const Message& message) const {
    if (!m_catalog) return {};
    return m_catalog->summaryFor(Message::typeName(message.type), message.vmfMessage,
                                 message.payload);
}

} // namespace gbr::vmf
