#include "VmfCodec.h"

#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace gbr::vmf {

namespace {

struct Key {
    QString dfi;
    QString dui;

    friend bool operator==(const Key& left, const Key& right) {
        return left.dfi == right.dfi && left.dui == right.dui;
    }
};

uint qHash(const Key& key, uint seed = 0) {
    return qHash(key.dfi, seed) ^ (qHash(key.dui, seed << 1U));
}

struct ContentItem {
    QString name;
    int bits = -1;
    QSet<quint64> allowed;
    QVector<QPair<quint64, quint64>> ranges;

    bool accepts(quint64 value) const {
        if (allowed.isEmpty() && ranges.isEmpty()) return true;
        if (allowed.contains(value)) return true;
        for (const auto& range : ranges) {
            if (value >= range.first && value <= range.second) return true;
        }
        return false;
    }
};

struct SpecNode {
    QString tag;
    QString name;
    QString dfi;
    QString dui;
    int bits = 0;
    bool repeatable = false;
    bool selectable = false;
    int maxRepeat = -1;
    QVector<SpecNode> children;
};

struct RuleSet {
    XmlNode rules;
};

struct MessageSpec {
    QString name;
    QString number;
    SpecNode header;
    SpecNode body;
    RuleSet rules;
};

bool validateRuleDefinitions(const RuleSet& rules, const SpecNode& header,
                             const SpecNode& body, QList<Diagnostic>* diagnostics);

QString trim(const QString& value) {
    return value.trimmed();
}

bool isTrue(const QString& value) {
    const QString lower = value.trimmed().toLower();
    return lower == QLatin1String("true") || lower == QLatin1String("1");
}

bool isValueNode(const SpecNode& node) {
    return node.tag == QLatin1String("DataUnit") || node.tag == QLatin1String("GRI")
        || node.tag == QLatin1String("FRI") || node.tag == QLatin1String("GPI")
        || node.tag == QLatin1String("FPI")
        || (node.tag == QLatin1String("Field") && node.children.isEmpty());
}

bool parseUnsigned(const QString& text, quint64* value) {
    if (!value) return false;
    const QString input = text.trimmed();
    if (input.isEmpty()) return false;
    bool ok = false;
    quint64 parsed = 0;
    if (input.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)) {
        parsed = input.mid(2).toULongLong(&ok, 16);
    } else {
        parsed = input.toULongLong(&ok, 10);
    }
    if (!ok) return false;
    *value = parsed;
    return true;
}

void addDiagnostic(QList<Diagnostic>* diagnostics, Diagnostic::Severity severity,
                   const QString& code, const QString& path, const QString& message) {
    if (!diagnostics) return;
    diagnostics->append(Diagnostic{severity, code, path, message});
}

XmlNode parseXmlElement(QXmlStreamReader& reader, QList<Diagnostic>* diagnostics) {
    XmlNode node;
    const auto start = reader.name().toString();
    node.tag = start;
    const auto attributes = reader.attributes();
    for (const auto& attribute : attributes) {
        node.attributes.insert(attribute.name().toString(), attribute.value().toString());
    }

    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isCharacters() && !reader.isWhitespace()) {
            node.text += reader.text().toString();
        } else if (reader.isStartElement()) {
            node.children.append(parseXmlElement(reader, diagnostics));
        } else if (reader.isEndElement()) {
            break;
        }
    }
    if (reader.hasError()) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("XML_PARSE"),
                      start, reader.errorString());
    }
    node.text = node.text.trimmed();
    return node;
}

bool parseDocument(const QByteArray& data, XmlNode* root,
                   QList<Diagnostic>* diagnostics) {
    if (!root) return false;
    QXmlStreamReader reader(data);
    while (!reader.atEnd() && !reader.isStartElement()) reader.readNext();
    if (reader.atEnd() || !reader.isStartElement()) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("XML_ROOT"),
                      {}, QStringLiteral("XML 文档缺少根节点"));
        return false;
    }
    *root = parseXmlElement(reader, diagnostics);
    if (reader.hasError()) return false;
    return true;
}

void writeXmlNode(QXmlStreamWriter& writer, const XmlNode& node) {
    writer.writeStartElement(node.tag);
    QStringList keys = node.attributes.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString& key : keys) writer.writeAttribute(key, node.attributes.value(key));
    if (!node.text.isEmpty()) writer.writeCharacters(node.text);
    for (const XmlNode& child : node.children) writeXmlNode(writer, child);
    writer.writeEndElement();
}

const XmlNode* child(const XmlNode& parent, const QString& tag,
                     const QString& name = QString()) {
    for (const XmlNode& node : parent.children) {
        if (node.tag != tag) continue;
        if (!name.isEmpty() && node.attributes.value(QStringLiteral("name")) != name) continue;
        return &node;
    }
    return nullptr;
}

QVector<const XmlNode*> children(const XmlNode& parent, const QString& tag,
                                 const QString& name = QString()) {
    QVector<const XmlNode*> result;
    for (const XmlNode& node : parent.children) {
        if (node.tag != tag) continue;
        if (!name.isEmpty() && node.attributes.value(QStringLiteral("name")) != name) continue;
        result.append(&node);
    }
    return result;
}

QVector<XmlNode*> children(XmlNode& parent, const QString& tag,
                           const QString& name = QString()) {
    QVector<XmlNode*> result;
    for (XmlNode& node : parent.children) {
        if (node.tag != tag) continue;
        if (!name.isEmpty() && node.attributes.value(QStringLiteral("name")) != name) continue;
        result.append(&node);
    }
    return result;
}

bool parseBits(const QString& text, int* bits) {
    quint64 value = 0;
    if (!parseUnsigned(text, &value) || value == 0 || value > 64) return false;
    if (bits) *bits = static_cast<int>(value);
    return true;
}

bool parseSpecNode(const XmlNode& xml, SpecNode* output, QList<Diagnostic>* diagnostics,
                  const QString& path) {
    if (!output) return false;
    output->tag = xml.tag;
    output->name = xml.attributes.value(QStringLiteral("name"));
    output->dfi = xml.attributes.value(QStringLiteral("DFI"));
    output->dui = xml.attributes.value(QStringLiteral("DUI"));
    output->repeatable = isTrue(xml.attributes.value(QStringLiteral("repeatable")));
    output->selectable = isTrue(xml.attributes.value(QStringLiteral("selectable")));

    if (xml.attributes.contains(QStringLiteral("max"))) {
        quint64 maxRepeat = 0;
        if (!parseUnsigned(xml.attributes.value(QStringLiteral("max")), &maxRepeat)
            || maxRepeat == 0 || maxRepeat > static_cast<quint64>(std::numeric_limits<int>::max())) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_MAX"), path,
                          QStringLiteral("repeatable max 无效"));
            return false;
        }
        output->maxRepeat = static_cast<int>(maxRepeat);
    }

    if (xml.tag == QLatin1String("DataUnit") || xml.tag == QLatin1String("GRI")
        || xml.tag == QLatin1String("FRI") || xml.tag == QLatin1String("GPI")
        || xml.tag == QLatin1String("FPI")
        || (xml.tag == QLatin1String("Field") && xml.children.isEmpty())) {
        if (!parseBits(xml.attributes.value(QStringLiteral("bits")), &output->bits)) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_BITS"), path,
                          QStringLiteral("bits 必须是 1..64"));
            return false;
        }
    }
    for (const XmlNode& childNode : xml.children) {
        if (childNode.tag != QLatin1String("Group")
            && childNode.tag != QLatin1String("Field")
            && childNode.tag != QLatin1String("DataUnit")
            && childNode.tag != QLatin1String("GRI")
            && childNode.tag != QLatin1String("FRI")
            && childNode.tag != QLatin1String("GPI")
            && childNode.tag != QLatin1String("FPI")) continue;
        SpecNode parsed;
        if (!parseSpecNode(childNode, &parsed, diagnostics,
                           path + QLatin1Char('.') + childNode.tag)) return false;
        output->children.append(std::move(parsed));
    }
    return true;
}

void collectContent(const XmlNode& root, QHash<Key, ContentItem>* output,
                    QList<Diagnostic>* diagnostics) {
    for (const XmlNode* dfi : children(root, QStringLiteral("DFI"))) {
        const QString dfiNumber = dfi->attributes.value(QStringLiteral("num"));
        if (dfiNumber.isEmpty()) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_DFI"),
                          QStringLiteral("dic"), QStringLiteral("DFI 缺少 num"));
            continue;
        }
        for (const XmlNode* dui : children(*dfi, QStringLiteral("DUI"))) {
            const QString duiNumber = dui->attributes.value(QStringLiteral("num"));
            const QString name = dui->attributes.value(QStringLiteral("name"));
            if (duiNumber.isEmpty() || name.isEmpty()) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_DUI"),
                              dfiNumber, QStringLiteral("DUI 缺少 num/name"));
                continue;
            }
            const Key key{dfiNumber, duiNumber};
            if (output->contains(key)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_DUPLICATE"),
                              dfiNumber + QLatin1Char('/') + duiNumber,
                              QStringLiteral("DFI/DUI 重复"));
                continue;
            }
            ContentItem item;
            item.name = name;
            if (dui->attributes.contains(QStringLiteral("bits"))
                && !parseBits(dui->attributes.value(QStringLiteral("bits")), &item.bits)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_BITS"),
                              dfiNumber + QLatin1Char('/') + duiNumber,
                              QStringLiteral("content bits 无效"));
            }
            if (const XmlNode* constraints = child(*dui, QStringLiteral("Constraints"))) {
                if (const XmlNode* allow = child(*constraints, QStringLiteral("AllowList"))) {
                    for (const XmlNode* value : children(*allow, QStringLiteral("Value"))) {
                        quint64 parsed = 0;
                        if (!parseUnsigned(value->attributes.value(QStringLiteral("v")), &parsed)) {
                            addDiagnostic(diagnostics, Diagnostic::Severity::Error,
                                          QStringLiteral("DICT_ALLOW"),
                                          dfiNumber + QLatin1Char('/') + duiNumber,
                                          QStringLiteral("AllowList Value 无效"));
                        } else {
                            item.allowed.insert(parsed);
                        }
                    }
                    for (const XmlNode* range : children(*allow, QStringLiteral("Range"))) {
                        quint64 from = 0;
                        quint64 to = 0;
                        if (!parseUnsigned(range->attributes.value(QStringLiteral("from")), &from)
                            || !parseUnsigned(range->attributes.value(QStringLiteral("to")), &to)
                            || from > to) {
                            addDiagnostic(diagnostics, Diagnostic::Severity::Error,
                                          QStringLiteral("DICT_RANGE"),
                                          dfiNumber + QLatin1Char('/') + duiNumber,
                                          QStringLiteral("AllowList Range 无效"));
                        } else {
                            item.ranges.append({from, to});
                        }
                    }
                }
            }
            output->insert(key, item);
        }
    }
}

void collectSpecData(const SpecNode& node, QHash<Key, QPair<QString, int>>* output,
                     QList<Diagnostic>* diagnostics, const QString& path) {
    if (node.tag == QLatin1String("DataUnit") || node.tag == QLatin1String("GRI")
        || node.tag == QLatin1String("FRI") || node.tag == QLatin1String("GPI")
        || node.tag == QLatin1String("FPI")) {
        if (node.dfi.isEmpty() || node.dui.isEmpty()) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_KEY"), path,
                          QStringLiteral("数据单元/指示位缺少 DFI/DUI"));
        } else {
            const Key key{node.dfi, node.dui};
            if (output->contains(key) && output->value(key) != qMakePair(node.name, node.bits)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_INCONSISTENT"),
                              path, QStringLiteral("同一 DFI/DUI 的名称或位宽不一致"));
            } else {
                output->insert(key, qMakePair(node.name, node.bits));
            }
        }
    }
    for (int i = 0; i < node.children.size(); ++i) {
        const SpecNode& childNode = node.children.at(i);
        collectSpecData(childNode, output, diagnostics,
                        path + QLatin1Char('.') + childNode.tag + QLatin1Char('[')
                            + QString::number(i) + QLatin1Char(']'));
    }
}

bool parseMessageDictionary(const XmlNode& root, MessageSpec* output,
                            QHash<Key, ContentItem>* content,
                            QList<Diagnostic>* diagnostics) {
    if (root.tag != QLatin1String("Message")) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_ROOT"), {},
                      QStringLiteral("消息字典根节点必须是 Message"));
        return false;
    }
    output->name = root.attributes.value(QStringLiteral("name"));
    output->number = root.attributes.value(QStringLiteral("number"));
    const XmlNode* header = child(root, QStringLiteral("Header"));
    const XmlNode* body = child(root, QStringLiteral("Body"));
    if (!header || !body || output->name.isEmpty()) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_SHAPE"), {},
                      QStringLiteral("消息字典缺少 name/Header/Body"));
        return false;
    }
    if (!parseSpecNode(*header, &output->header, diagnostics, QStringLiteral("Header"))
        || !parseSpecNode(*body, &output->body, diagnostics, QStringLiteral("Body"))) return false;
    if (const XmlNode* rules = child(root, QStringLiteral("Rules"))) output->rules.rules = *rules;

    // Rule paths are part of the dictionary contract.  Validate them while
    // loading the dictionary so a malformed rule cannot remain dormant until
    // a particular message happens to exercise it.
    if (!validateRuleDefinitions(output->rules, output->header, output->body, diagnostics)) {
        return false;
    }

    QHash<Key, QPair<QString, int>> specData;
    collectSpecData(output->header, &specData, diagnostics, QStringLiteral("Header"));
    collectSpecData(output->body, &specData, diagnostics, QStringLiteral("Body"));
    for (auto it = specData.cbegin(); it != specData.cend(); ++it) {
        if (!content->contains(it.key())) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_MISSING_CONTENT"),
                          it.key().dfi + QLatin1Char('/') + it.key().dui,
                          QStringLiteral("消息字典中的 DFI/DUI 不在内容字典中"));
            continue;
        }
        const ContentItem& item = content->value(it.key());
        if (item.bits != it.value().second) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_BITS_MISMATCH"),
                          it.key().dfi + QLatin1Char('/') + it.key().dui,
                          QStringLiteral("消息字典与内容字典位宽不一致"));
        }
        if (!it.value().first.isEmpty() && item.name != it.value().first) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_NAME_MISMATCH"),
                          it.key().dfi + QLatin1Char('/') + it.key().dui,
                          QStringLiteral("消息字典与内容字典名称不一致"));
        }
    }
    return true;
}

bool parseMessage(const QByteArray& data, MessageSpec* output,
                  QHash<Key, ContentItem>* content,
                  QList<Diagnostic>* diagnostics) {
    XmlNode root;
    if (!parseDocument(data, &root, diagnostics)) return false;
    return parseMessageDictionary(root, output, content, diagnostics);
}

bool parseContent(const QByteArray& data, QHash<Key, ContentItem>* output,
                  QList<Diagnostic>* diagnostics) {
    XmlNode root;
    if (!parseDocument(data, &root, diagnostics)) return false;
    if (root.tag != QLatin1String("dic")) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_ROOT"), {},
                      QStringLiteral("内容字典根节点必须是 dic"));
        return false;
    }
    collectContent(root, output, diagnostics);
    return true;
}

const XmlNode* findIndicator(const XmlNode& node, const QString& tag) {
    return child(node, tag);
}

QVector<const XmlNode*> findInstances(const XmlNode& node, const SpecNode& spec) {
    if (spec.tag == QLatin1String("DataUnit")) {
        QVector<const XmlNode*> result;
        for (const XmlNode& candidate : node.children) {
            if (candidate.tag != QLatin1String("DataUnit")) continue;
            if (!spec.name.isEmpty() && candidate.attributes.value(QStringLiteral("name")) == spec.name) {
                result.append(&candidate);
            } else if (candidate.attributes.value(QStringLiteral("DFI")) == spec.dfi
                       && candidate.attributes.value(QStringLiteral("DUI")) == spec.dui) {
                result.append(&candidate);
            }
        }
        return result;
    }
    return children(node, spec.tag, spec.name);
}

const XmlNode* findDataUnit(const XmlNode& node, const SpecNode& spec) {
    const auto matches = findInstances(node, spec);
    return matches.isEmpty() ? nullptr : matches.front();
}

bool readNodeValue(const XmlNode& node, quint64* value) {
    return parseUnsigned(node.text, value);
}

QString instancePath(const QString& parent, const SpecNode& spec, int index = -1) {
    QString result = parent + QLatin1Char('.') + spec.tag + QLatin1Char('(') + spec.name
        + QLatin1Char(')');
    if (index >= 0) result += QLatin1Char('[') + QString::number(index) + QLatin1Char(']');
    return result;
}

void validateValueNode(const SpecNode& spec, const XmlNode& actual, const QString& path,
                       const QHash<Key, ContentItem>& content,
                       QList<Diagnostic>* diagnostics) {
    quint64 value = 0;
    if (!readNodeValue(actual, &value)) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("VALUE_PARSE"), path,
                      QStringLiteral("节点值不是无符号整数"));
        return;
    }
    if (spec.bits < 64 && value > ((quint64{1} << spec.bits) - 1U)) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("VALUE_RANGE"), path,
                      QStringLiteral("值超出位宽范围"));
    }
    if (!spec.dfi.isEmpty() && !spec.dui.isEmpty()) {
        const auto item = content.constFind(Key{spec.dfi, spec.dui});
        if (item != content.cend() && !item->accepts(value)) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("ALLOW_LIST"), path,
                          QStringLiteral("值不在内容字典允许范围内"));
        }
        const QString actualName = actual.attributes.value(QStringLiteral("name"));
        if (!actualName.isEmpty() && actualName != spec.name) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("VALUE_NAME"), path,
                          QStringLiteral("DataUnit name 与字典不一致"));
        }
        if (actual.attributes.value(QStringLiteral("DFI")) != spec.dfi
            || actual.attributes.value(QStringLiteral("DUI")) != spec.dui) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("VALUE_KEY"), path,
                          QStringLiteral("DataUnit DFI/DUI 与字典不一致"));
        }
    }
}

void validateChildren(const SpecNode& parentSpec, const XmlNode& parentActual,
                      const QString& path, const QHash<Key, ContentItem>& content,
                      QList<Diagnostic>* diagnostics);

void validateContainer(const SpecNode& spec, const XmlNode& actual, const QString& path,
                       const QHash<Key, ContentItem>& content,
                       QList<Diagnostic>* diagnostics) {
    const QString indicatorTag = spec.tag == QLatin1String("Group") ? QStringLiteral("GPI")
        : QStringLiteral("FPI");
    if (spec.selectable) {
        const XmlNode* indicator = findIndicator(actual, indicatorTag);
        quint64 present = 0;
        if (!indicator || !readNodeValue(*indicator, &present) || present > 1) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error,
                          QStringLiteral("SELECTABLE_INDICATOR"), path,
                          QStringLiteral("可选结构缺少有效存在指示位"));
            return;
        }
        if (present == 0) {
            for (const XmlNode& childNode : actual.children) {
                if (childNode.tag != indicatorTag) {
                    addDiagnostic(diagnostics, Diagnostic::Severity::Error,
                                  QStringLiteral("SELECTABLE_CHILD"), path,
                                  QStringLiteral("存在位为 0 时不应包含子节点"));
                    break;
                }
            }
            return;
        }
    }
    validateChildren(spec, actual, path, content, diagnostics);
}

void validateRepeatIndicator(const SpecNode& spec, const XmlNode& actual,
                             const QString& path, int index, int count,
                             QList<Diagnostic>* diagnostics) {
    const QString tag = spec.tag == QLatin1String("Group") ? QStringLiteral("GRI")
        : QStringLiteral("FRI");
    const XmlNode* indicator = findIndicator(actual, tag);
    quint64 value = 0;
    if (!indicator || !readNodeValue(*indicator, &value) || value > 1) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, path + QLatin1Char('.') + tag,
                      QStringLiteral("REPEAT_INDICATOR"), QStringLiteral("重复指示位无效"));
    } else if (value != static_cast<quint64>(index + 1 < count)) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, path + QLatin1Char('.') + tag,
                      QStringLiteral("REPEAT_SEQUENCE"), QStringLiteral("重复指示位与实例顺序不一致"));
    }
}

void validateChildren(const SpecNode& parentSpec, const XmlNode& parentActual,
                      const QString& path, const QHash<Key, ContentItem>& content,
                      QList<Diagnostic>* diagnostics) {
    QSet<const XmlNode*> consumed;
    for (const SpecNode& spec : parentSpec.children) {
        const auto matches = findInstances(parentActual, spec);
        if (spec.repeatable) {
            if (matches.isEmpty()) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, instancePath(path, spec),
                              QStringLiteral("MISSING_REPEAT"), QStringLiteral("缺少重复结构实例"));
                continue;
            }
            if (spec.maxRepeat > 0 && matches.size() > spec.maxRepeat) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, instancePath(path, spec),
                              QStringLiteral("REPEAT_MAX"), QStringLiteral("重复结构超过 max 限制"));
            }
            for (int i = 0; i < matches.size(); ++i) {
                const XmlNode* actual = matches.at(i);
                consumed.insert(actual);
                const QString currentPath = instancePath(path, spec, i);
                if (isValueNode(spec)) {
                    validateValueNode(spec, *actual, currentPath, content, diagnostics);
                } else {
                    validateRepeatIndicator(spec, *actual, currentPath, i, matches.size(), diagnostics);
                    validateContainer(spec, *actual, currentPath, content, diagnostics);
                }
            }
        } else {
            if (matches.isEmpty()) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, instancePath(path, spec),
                              QStringLiteral("MISSING_REQUIRED"), QStringLiteral("缺少必需节点"));
                continue;
            }
            if (matches.size() > 1) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, instancePath(path, spec),
                              QStringLiteral("DUPLICATE_REQUIRED"), QStringLiteral("非重复节点出现多次"));
            }
            const XmlNode* actual = matches.front();
            consumed.insert(actual);
            if (isValueNode(spec)) {
                validateValueNode(spec, *actual, instancePath(path, spec), content, diagnostics);
            } else {
                validateContainer(spec, *actual, instancePath(path, spec), content, diagnostics);
            }
        }
    }
    for (const XmlNode& actual : parentActual.children) {
        if (!consumed.contains(&actual)) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, path,
                          QStringLiteral("UNEXPECTED_NODE"),
                          QStringLiteral("消息包含字典未声明的节点: %1").arg(actual.tag));
        }
    }
}

struct ResolvedValue {
    quint64 value = 0;
};

QStringList splitPath(const QString& raw) {
    QStringList parts;
    QString current;
    int depth = 0;
    for (const QChar character : raw) {
        if (character == QLatin1Char('(')) ++depth;
        if (character == QLatin1Char(')')) --depth;
        if (character == QLatin1Char('.') && depth == 0) {
            if (!current.isEmpty()) parts.append(current);
            current.clear();
        } else {
            current += character;
        }
    }
    if (!current.isEmpty()) parts.append(current);
    return parts;
}

struct Segment {
    QChar kind;
    QString name;
    bool indexed = false;
    bool wildcard = false;
    int index = 0;
};

bool parseSegment(const QString& raw, Segment* output) {
    if (!output) return false;
    const int open = raw.indexOf(QLatin1Char('('));
    const int close = raw.indexOf(QLatin1Char(')'), open + 1);
    if (open <= 0 || close <= open + 1 || close != raw.lastIndexOf(QLatin1Char(')'))) return false;
    output->kind = raw.at(0);
    if (output->kind != QLatin1Char('G') && output->kind != QLatin1Char('F')
        && output->kind != QLatin1Char('D') && output->kind != QLatin1Char('I')) return false;
    output->name = raw.mid(open + 1, close - open - 1);
    if (output->name.trimmed().isEmpty()) return false;
    if (close + 1 == raw.size()) return true;
    if (output->kind != QLatin1Char('G') && output->kind != QLatin1Char('F')) return false;
    if (raw.at(close + 1) != QLatin1Char('[') || !raw.endsWith(QLatin1Char(']'))) return false;
    const QString index = raw.mid(close + 2, raw.size() - close - 3);
    if (index.isEmpty()) return false;
    output->indexed = true;
    if (index == QLatin1String("*")) {
        output->wildcard = true;
        return true;
    }
    bool ok = false;
    output->index = index.toInt(&ok);
    return ok;
}

QString segmentTag(const Segment& segment) {
    if (segment.kind == QLatin1Char('G')) return QStringLiteral("Group");
    if (segment.kind == QLatin1Char('F')) return QStringLiteral("Field");
    if (segment.kind == QLatin1Char('D')) return QStringLiteral("DataUnit");
    return segment.name;
}

bool isIndicatorName(const QString& name) {
    return name == QLatin1String("GRI") || name == QLatin1String("GPI")
        || name == QLatin1String("FRI") || name == QLatin1String("FPI");
}

QVector<const SpecNode*> matchingSpecChildren(const SpecNode& parent, const Segment& segment) {
    QVector<const SpecNode*> matches;
    const QString tag = segmentTag(segment);
    for (const SpecNode& candidate : parent.children) {
        if (segment.kind == QLatin1Char('D')) {
            // D(name) is a DataUnit lookup in the body.  The design also
            // permits the compact Header.Field(name) form for header scalars.
            const bool headerField = candidate.tag == QLatin1String("Field")
                && candidate.children.isEmpty();
            if ((!candidate.name.isEmpty() && candidate.name == segment.name)
                && (candidate.tag == QLatin1String("DataUnit") || headerField)) {
                matches.append(&candidate);
            }
            continue;
        } else if (candidate.name == segment.name || segment.kind == QLatin1Char('I')) {
            if (candidate.tag != tag) continue;
            matches.append(&candidate);
        }
    }
    return matches;
}

QString expandRuleAlias(const QString& rawPath, const QHash<QString, QString>& aliases,
                        QString* error) {
    if (!rawPath.startsWith(QLatin1Char('$'))) return rawPath;
    const int dot = rawPath.indexOf(QLatin1Char('.'));
    const QString alias = rawPath.mid(1, dot < 0 ? -1 : dot - 1);
    if (alias.isEmpty() || !aliases.contains(alias)) {
        if (error) *error = QStringLiteral("未知 alias: %1").arg(alias);
        return {};
    }
    const QString replacement = aliases.value(alias);
    if (replacement.startsWith(QLatin1Char('$'))) {
        if (error) *error = QStringLiteral("alias 不得引用另一个 alias: %1").arg(alias);
        return {};
    }
    return replacement + (dot < 0 ? QString() : rawPath.mid(dot));
}

bool validateRulePath(const QString& rawPath, const SpecNode& header, const SpecNode& body,
                      const QHash<QString, QString>& aliases, QString* error,
                      bool requireValue = true) {
    if (error) error->clear();
    QString path = expandRuleAlias(rawPath, aliases, error);
    if (path.isEmpty()) return false;

    const bool iteration = path.endsWith(QStringLiteral(".iteration"));
    if (iteration) path.chop(QStringLiteral(".iteration").size());
    const QStringList rawSegments = splitPath(path);
    if (rawSegments.isEmpty()) {
        if (error) *error = QStringLiteral("路径为空");
        return false;
    }
    if (rawSegments.first() != QLatin1String("H")
        && rawSegments.first() != QLatin1String("B")) {
        if (error) *error = QStringLiteral("路径必须以 H 或 B 开始");
        return false;
    }
    QVector<const SpecNode*> current;
    current.append(rawSegments.first() == QLatin1String("H") ? &header : &body);
    for (int i = 1; i < rawSegments.size(); ++i) {
        Segment segment;
        if (!parseSegment(rawSegments.at(i), &segment)) {
            if (error) *error = QStringLiteral("路径段无效: %1").arg(rawSegments.at(i));
            return false;
        }
        if (segment.kind == QLatin1Char('I') && !isIndicatorName(segment.name)) {
            if (error) *error = QStringLiteral("不支持的指示位: %1").arg(segment.name);
            return false;
        }

        QVector<const SpecNode*> next;
        for (const SpecNode* parent : current) {
            const auto matches = matchingSpecChildren(*parent, segment);
            if (matches.isEmpty()) {
                if (error) *error = QStringLiteral("字典路径未找到: %1").arg(rawPath);
                return false;
            }
            if (segment.kind == QLatin1Char('I') && matches.size() != 1) {
                if (error) *error = QStringLiteral("指示位路径不唯一: %1").arg(rawPath);
                return false;
            }
            if (segment.indexed) {
                if (segment.kind != QLatin1Char('G') && segment.kind != QLatin1Char('F')) {
                    if (error) *error = QStringLiteral("只有 Group/Field 允许实例下标: %1").arg(rawPath);
                    return false;
                }
                for (const SpecNode* match : matches) {
                    if (!match->repeatable) {
                        if (error) *error = QStringLiteral("非 repeatable 节点不得带下标: %1").arg(rawPath);
                        return false;
                    }
                    if (!segment.wildcard && match->maxRepeat > 0) {
                        const int index = segment.index >= 0
                            ? segment.index : match->maxRepeat + segment.index;
                        if (index < 0 || index >= match->maxRepeat) {
                            if (error) *error = QStringLiteral("路径下标超过字典 max: %1").arg(rawPath);
                            return false;
                        }
                    }
                }
                if (segment.wildcard) next += matches;
                else next.append(matches.first());
            } else {
                if (matches.size() > 1) {
                    if (error) *error = QStringLiteral("字典路径存在多个匹配，必须显式索引: %1")
                                             .arg(rawPath);
                    return false;
                }
                next.append(matches.first());
            }
        }
        current = next;
    }

    if (iteration) {
        if (current.isEmpty()) {
            if (error) *error = QStringLiteral("iteration 目标为空: %1").arg(rawPath);
            return false;
        }
        for (const SpecNode* node : current) {
            if ((node->tag != QLatin1String("Group") && node->tag != QLatin1String("Field"))
                || !node->repeatable) {
                if (error) *error = QStringLiteral("iteration 仅适用于 repeatable Group/Field: %1")
                                             .arg(rawPath);
                return false;
            }
            const QString indicator = node->tag == QLatin1String("Group")
                ? QStringLiteral("GRI") : QStringLiteral("FRI");
            bool found = false;
            for (const SpecNode& childNode : node->children) {
                if (childNode.tag == indicator) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (error) *error = QStringLiteral("repeatable 容器缺少 %1: %2")
                                             .arg(indicator, rawPath);
                return false;
            }
        }
    } else if (requireValue) {
        for (const SpecNode* node : current) {
            if (!isValueNode(*node)) {
                if (error) *error = QStringLiteral("Cmp path 必须指向数据值或指示位: %1").arg(rawPath);
                return false;
            }
        }
    }
    return true;
}

QVector<const XmlNode*> resolvePath(const XmlNode& header, const XmlNode& body,
                                    const QString& rawPath, const QHash<QString, QString>& aliases,
                                    QString* error, bool allowFinalMultiple = false) {
    QString path = rawPath;
    if (path.startsWith(QLatin1Char('$'))) {
        const int dot = path.indexOf(QLatin1Char('.'));
        const QString alias = path.mid(1, dot < 0 ? -1 : dot - 1);
        if (!aliases.contains(alias)) {
            if (error) *error = QStringLiteral("未知 alias: %1").arg(alias);
            return {};
        }
        path = aliases.value(alias) + (dot < 0 ? QString() : path.mid(dot));
    }
    const QStringList rawSegments = splitPath(path);
    if (rawSegments.isEmpty()) {
        if (error) *error = QStringLiteral("路径为空");
        return {};
    }
    QVector<const XmlNode*> current;
    if (rawSegments.first() == QLatin1String("H")) current.append(&header);
    else if (rawSegments.first() == QLatin1String("B")) current.append(&body);
    else {
        if (error) *error = QStringLiteral("路径必须以 H 或 B 开始");
        return {};
    }
    for (int i = 1; i < rawSegments.size(); ++i) {
        Segment segment;
        if (!parseSegment(rawSegments.at(i), &segment)) {
            if (error) *error = QStringLiteral("路径段无效: %1").arg(rawSegments.at(i));
            return {};
        }
        QVector<const XmlNode*> next;
        for (const XmlNode* parent : current) {
            QString tag;
            if (segment.kind == QLatin1Char('G')) tag = QStringLiteral("Group");
            else if (segment.kind == QLatin1Char('F')) tag = QStringLiteral("Field");
            else if (segment.kind == QLatin1Char('D')) tag = QStringLiteral("DataUnit");
            else if (segment.kind == QLatin1Char('I')) tag = segment.name;
            else {
                if (error) *error = QStringLiteral("路径段类型不支持");
                return {};
            }
            QVector<const XmlNode*> matches;
            if (segment.kind == QLatin1Char('D')) {
                for (const XmlNode& node : parent->children) {
                    const bool headerField = parent->tag == QLatin1String("Header")
                        && node.tag == QLatin1String("Field") && node.children.isEmpty();
                    if ((node.tag == tag || headerField)
                        && node.attributes.value(QStringLiteral("name")) == segment.name) {
                        matches.append(&node);
                    }
                }
            } else if (segment.kind == QLatin1Char('I')) {
                for (const XmlNode& node : parent->children) {
                    if (node.tag == tag) matches.append(&node);
                }
            } else {
                matches = children(*parent, tag, segment.name);
            }
            if (segment.indexed) {
                if (segment.wildcard) {
                    next += matches;
                } else {
                    int index = segment.index;
                    if (index < 0) index += static_cast<int>(matches.size());
                    if (index >= 0 && index < matches.size()) next.append(matches.at(index));
                    else if (error) *error = QStringLiteral("路径索引越界: %1").arg(rawPath);
                }
            } else if (matches.size() == 1) {
                next.append(matches.front());
            } else if (allowFinalMultiple && i == rawSegments.size() - 1) {
                next += matches;
            } else if (matches.isEmpty()) {
                if (error) *error = QStringLiteral("路径未找到: %1").arg(rawPath);
            } else if (error) {
                *error = QStringLiteral("路径存在多个匹配，必须显式索引: %1").arg(rawPath);
            }
        }
        if (next.isEmpty() && error && error->isEmpty()) *error = QStringLiteral("路径未找到: %1").arg(rawPath);
        current = next;
        if (current.isEmpty()) return {};
    }
    return current;
}

bool evaluateExpression(const XmlNode& expression, const XmlNode& header, const XmlNode& body,
                        const QHash<QString, QString>& aliases, bool* result, QString* error) {
    if (!result) return false;
    if (expression.tag == QLatin1String("All") || expression.tag == QLatin1String("AND")) {
        if (expression.children.isEmpty()) { *result = false; return true; }
        *result = true;
        for (const XmlNode& childNode : expression.children) {
            bool childResult = false;
            if (!evaluateExpression(childNode, header, body, aliases, &childResult, error)) return false;
            *result = *result && childResult;
        }
        return true;
    }
    if (expression.tag == QLatin1String("Any") || expression.tag == QLatin1String("OR")) {
        *result = false;
        for (const XmlNode& childNode : expression.children) {
            bool childResult = false;
            if (!evaluateExpression(childNode, header, body, aliases, &childResult, error)) return false;
            *result = *result || childResult;
        }
        return true;
    }
    if (expression.tag == QLatin1String("Not")) {
        if (expression.children.size() != 1) {
            if (error) *error = QStringLiteral("Not 必须只有一个子表达式");
            return false;
        }
        if (!evaluateExpression(expression.children.first(), header, body, aliases, result, error)) return false;
        *result = !*result;
        return true;
    }
    if (expression.tag != QLatin1String("Cmp")) {
        if (error) *error = QStringLiteral("未知表达式节点: %1").arg(expression.tag);
        return false;
    }
    const QString path = expression.attributes.value(QStringLiteral("path"));
    const QString op = expression.attributes.value(QStringLiteral("op"));
    const QString valueText = expression.attributes.value(QStringLiteral("value"));
    const QString targetPath = expression.attributes.value(QStringLiteral("targetPath"));
    const bool hasValue = expression.attributes.contains(QStringLiteral("value"));
    const bool hasTargetPath = expression.attributes.contains(QStringLiteral("targetPath"));
    if (path.trimmed().isEmpty() || op.trimmed().isEmpty() || hasValue == hasTargetPath) {
        if (error) *error = QStringLiteral("Cmp 缺少 path/op 或 value/targetPath");
        return false;
    }
    if (hasTargetPath && (op == QLatin1String("in") || op == QLatin1String("not_in"))) {
        if (error) *error = QStringLiteral("targetPath 不支持 in/not_in");
        return false;
    }
    const bool leftIsIteration = path.endsWith(QStringLiteral(".iteration"));
    const QString leftPath = leftIsIteration
        ? path.left(path.size() - QStringLiteral(".iteration").size()) : path;
    QString resolveError;
    const auto leftNodes = resolvePath(header, body, leftPath, aliases, &resolveError,
                                       leftIsIteration);
    if (leftNodes.isEmpty()) {
        if (error) *error = resolveError;
        return false;
    }
    QVector<quint64> leftValues;
    if (leftIsIteration) {
        leftValues.append(static_cast<quint64>(leftNodes.size()));
    } else {
        for (const XmlNode* node : leftNodes) {
            quint64 value = 0;
            if (!readNodeValue(*node, &value)) {
                if (error) *error = QStringLiteral("Cmp 左值不是整数: %1").arg(path);
                return false;
            }
            leftValues.append(value);
        }
    }
    quint64 right = 0;
    if (hasTargetPath) {
        const bool rightIsIteration = targetPath.endsWith(QStringLiteral(".iteration"));
        const QString rightPath = rightIsIteration
            ? targetPath.left(targetPath.size() - QStringLiteral(".iteration").size()) : targetPath;
        const auto rightNodes = resolvePath(header, body, rightPath, aliases, &resolveError,
                                            rightIsIteration);
        if (rightIsIteration) {
            right = static_cast<quint64>(rightNodes.size());
        }
        else if (rightNodes.size() != 1 || !readNodeValue(*rightNodes.first(), &right)) {
            if (error) *error = QStringLiteral("Cmp targetPath 无法解析: %1").arg(targetPath);
            return false;
        }
    }
    QSet<quint64> allowed;
    if (hasValue && (op == QLatin1String("in") || op == QLatin1String("not_in"))) {
        for (const QString& token : valueText.split(QLatin1Char(','), Qt::KeepEmptyParts)) {
            quint64 parsed = 0;
            if (token.trimmed().isEmpty() || !parseUnsigned(token, &parsed)) {
                if (error) *error = QStringLiteral("Cmp value 无效");
                return false;
            }
            allowed.insert(parsed);
        }
    } else if (hasValue && !parseUnsigned(valueText, &right)) {
        if (error) *error = QStringLiteral("Cmp value 无效");
        return false;
    }
    auto compare = [&](quint64 left) {
        if (op == QLatin1String("eq")) return left == right;
        if (op == QLatin1String("ne")) return left != right;
        if (op == QLatin1String("gt")) return left > right;
        if (op == QLatin1String("ge")) return left >= right;
        if (op == QLatin1String("lt")) return left < right;
        if (op == QLatin1String("le")) return left <= right;
        if (op == QLatin1String("in")) return allowed.contains(left);
        if (op == QLatin1String("not_in")) return !allowed.contains(left);
        return false;
    };
    if (op != QLatin1String("eq") && op != QLatin1String("ne") && op != QLatin1String("gt")
        && op != QLatin1String("ge") && op != QLatin1String("lt") && op != QLatin1String("le")
        && op != QLatin1String("in") && op != QLatin1String("not_in")) {
        if (error) *error = QStringLiteral("Cmp 操作符不支持: %1").arg(op);
        return false;
    }
    *result = std::all_of(leftValues.cbegin(), leftValues.cend(), compare);
    return true;
}

QHash<QString, QString> loadAliases(const XmlNode& rules) {
    QHash<QString, QString> aliases;
    const XmlNode* aliasRoot = child(rules, QStringLiteral("Aliases"));
    if (!aliasRoot) return aliases;
    for (const XmlNode* alias : children(*aliasRoot, QStringLiteral("Alias"))) {
        const QString name = alias->attributes.value(QStringLiteral("name"));
        const QString path = alias->attributes.value(QStringLiteral("path"));
        if (!name.isEmpty() && !path.isEmpty()) aliases.insert(name, path);
    }
    return aliases;
}

bool supportedCompareOperator(const QString& op) {
    return op == QLatin1String("eq") || op == QLatin1String("ne")
        || op == QLatin1String("gt") || op == QLatin1String("ge")
        || op == QLatin1String("lt") || op == QLatin1String("le")
        || op == QLatin1String("in") || op == QLatin1String("not_in");
}

bool validateExpressionDefinition(const XmlNode& expression, const SpecNode& header,
                                  const SpecNode& body, const QHash<QString, QString>& aliases,
                                  QList<Diagnostic>* diagnostics, const QString& path) {
    bool valid = true;
    const auto report = [&](const QString& code, const QString& message) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, code, path, message);
        valid = false;
    };
    if (expression.tag == QLatin1String("All") || expression.tag == QLatin1String("AND")
        || expression.tag == QLatin1String("Any") || expression.tag == QLatin1String("OR")) {
        if (expression.children.isEmpty()) {
            report(QStringLiteral("RULE_SHAPE"), QStringLiteral("逻辑节点至少需要一个子表达式"));
        }
        for (int index = 0; index < expression.children.size(); ++index) {
            if (!validateExpressionDefinition(expression.children.at(index), header, body, aliases,
                                              diagnostics,
                                              path + QLatin1Char('.') + expression.children.at(index).tag
                                                  + QLatin1Char('[') + QString::number(index) + QLatin1Char(']'))) {
                valid = false;
            }
        }
        return valid;
    }
    if (expression.tag == QLatin1String("Not")) {
        if (expression.children.size() != 1) {
            report(QStringLiteral("RULE_SHAPE"), QStringLiteral("Not 必须只有一个子表达式"));
            return false;
        }
        return validateExpressionDefinition(expression.children.first(), header, body, aliases,
                                            diagnostics, path + QStringLiteral(".Not"));
    }
    if (expression.tag != QLatin1String("Cmp")) {
        report(QStringLiteral("RULE_SHAPE"), QStringLiteral("未知表达式节点: %1").arg(expression.tag));
        return false;
    }
    if (!expression.children.isEmpty()) {
        report(QStringLiteral("RULE_SHAPE"), QStringLiteral("Cmp 不得包含子节点"));
    }
    const QString rulePath = expression.attributes.value(QStringLiteral("path"));
    const QString op = expression.attributes.value(QStringLiteral("op"));
    const bool hasValue = expression.attributes.contains(QStringLiteral("value"));
    const bool hasTargetPath = expression.attributes.contains(QStringLiteral("targetPath"));
    if (rulePath.trimmed().isEmpty() || op.trimmed().isEmpty() || hasValue == hasTargetPath) {
        report(QStringLiteral("RULE_SHAPE"),
               QStringLiteral("Cmp 必须提供 path/op，并且必须且只能提供 value 或 targetPath"));
        return false;
    }
    if (!supportedCompareOperator(op)) {
        report(QStringLiteral("RULE_OPERATOR"), QStringLiteral("Cmp 操作符不支持: %1").arg(op));
    }
    if (hasTargetPath && (op == QLatin1String("in") || op == QLatin1String("not_in"))) {
        report(QStringLiteral("RULE_OPERATOR"), QStringLiteral("targetPath 不支持 in/not_in"));
    }
    QString pathError;
    if (!validateRulePath(rulePath, header, body, aliases, &pathError)) {
        report(QStringLiteral("RULE_PATH"), pathError);
    }
    if (hasTargetPath) {
        const QString targetPath = expression.attributes.value(QStringLiteral("targetPath"));
        if (targetPath.trimmed().isEmpty()) {
            report(QStringLiteral("RULE_SHAPE"), QStringLiteral("targetPath 不得为空"));
        } else if (!validateRulePath(targetPath, header, body, aliases, &pathError)) {
            report(QStringLiteral("RULE_PATH"), pathError);
        }
    } else {
        const QString value = expression.attributes.value(QStringLiteral("value"));
        if (value.trimmed().isEmpty()) {
            report(QStringLiteral("RULE_SHAPE"), QStringLiteral("value 不得为空"));
        } else if (op == QLatin1String("in") || op == QLatin1String("not_in")) {
            const QStringList tokens = value.split(QLatin1Char(','), Qt::KeepEmptyParts);
            if (tokens.isEmpty()) {
                report(QStringLiteral("RULE_VALUE"), QStringLiteral("Cmp value 列表为空"));
            }
            for (const QString& token : tokens) {
                quint64 parsed = 0;
                if (token.trimmed().isEmpty() || !parseUnsigned(token, &parsed)) {
                    report(QStringLiteral("RULE_VALUE"), QStringLiteral("Cmp value 列表包含无效整数"));
                    break;
                }
            }
        } else {
            quint64 parsed = 0;
            if (!parseUnsigned(value, &parsed)) {
                report(QStringLiteral("RULE_VALUE"), QStringLiteral("Cmp value 不是无符号整数"));
            }
        }
    }
    return valid;
}

bool validateRuleDefinitions(const RuleSet& rules, const SpecNode& header,
                             const SpecNode& body, QList<Diagnostic>* diagnostics) {
    if (rules.rules.tag.isEmpty()) return true;
    bool valid = true;
    const XmlNode& root = rules.rules;
    const QSet<QString> knownTopLevel{
        QStringLiteral("Aliases"), QStringLiteral("CaseRules"), QStringLiteral("ConditionRules")};
    for (const XmlNode& node : root.children) {
        if (!knownTopLevel.contains(node.tag)) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_SHAPE"),
                          QStringLiteral("Rules"), QStringLiteral("未知 Rules 子节点: %1").arg(node.tag));
            valid = false;
        }
    }

    QHash<QString, QString> aliases;
    if (const XmlNode* aliasRoot = child(root, QStringLiteral("Aliases"))) {
        for (const XmlNode* alias : children(*aliasRoot, QStringLiteral("Alias"))) {
            const QString name = alias->attributes.value(QStringLiteral("name")).trimmed();
            const QString path = alias->attributes.value(QStringLiteral("path")).trimmed();
            if (name.isEmpty() || path.isEmpty() || name.startsWith(QLatin1Char('$'))) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_ALIAS"),
                              QStringLiteral("Rules.Aliases"), QStringLiteral("Alias 缺少有效 name/path"));
                valid = false;
                continue;
            }
            if (aliases.contains(name)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_ALIAS"), name,
                              QStringLiteral("Alias 名称重复"));
                valid = false;
                continue;
            }
            aliases.insert(name, path);
            QString pathError;
            if (!validateRulePath(path, header, body, {}, &pathError, false)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_PATH"), name,
                              pathError);
                valid = false;
            }
        }
    }

    QSet<QString> caseIds;
    if (const XmlNode* caseRules = child(root, QStringLiteral("CaseRules"))) {
        for (const XmlNode* caseNode : children(*caseRules, QStringLiteral("Case"))) {
            const QString id = caseNode->attributes.value(QStringLiteral("id")).trimmed();
            if (id.isEmpty() || caseIds.contains(id)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_CASE"), id,
                              QStringLiteral("Case id 缺失或重复"));
                valid = false;
            } else {
                caseIds.insert(id);
            }
            if (caseNode->children.size() != 1) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_SHAPE"), id,
                              QStringLiteral("Case 必须恰好包含一个表达式"));
                valid = false;
                continue;
            }
            if (!validateExpressionDefinition(caseNode->children.first(), header, body, aliases,
                                              diagnostics, QStringLiteral("Case.") + id)) {
                valid = false;
            }
        }
    }

    QSet<QString> conditionIds;
    if (const XmlNode* conditionRules = child(root, QStringLiteral("ConditionRules"))) {
        for (const XmlNode* condition : children(*conditionRules, QStringLiteral("Condition"))) {
            const QString id = condition->attributes.value(QStringLiteral("id")).trimmed();
            const QString caseRef = condition->attributes.value(QStringLiteral("caseRef")).trimmed();
            if (id.isEmpty() || conditionIds.contains(id)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_CONDITION"), id,
                              QStringLiteral("Condition id 缺失或重复"));
                valid = false;
            } else {
                conditionIds.insert(id);
            }
            if (!caseRef.isEmpty() && !caseIds.contains(caseRef)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_CASE_REF"), id,
                              QStringLiteral("Condition 引用了不存在的 Case: %1").arg(caseRef));
                valid = false;
            }
            const XmlNode* ifNode = child(*condition, QStringLiteral("If"));
            const XmlNode* thenNode = child(*condition, QStringLiteral("Then"));
            if (!ifNode || !thenNode || ifNode->children.size() != 1
                || thenNode->children.size() != 1) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_SHAPE"), id,
                              QStringLiteral("Condition 必须包含恰好一个 If 和一个 Then 表达式"));
                valid = false;
                continue;
            }
            const bool ifValid = validateExpressionDefinition(
                ifNode->children.first(), header, body, aliases, diagnostics,
                QStringLiteral("Condition.") + id + QStringLiteral(".If"));
            const bool thenValid = validateExpressionDefinition(
                thenNode->children.first(), header, body, aliases, diagnostics,
                QStringLiteral("Condition.") + id + QStringLiteral(".Then"));
            if (!ifValid || !thenValid) {
                valid = false;
            }
        }
    }
    return valid;
}

void validateRules(const RuleSet& rules, const MessageXml& message,
                   QList<Diagnostic>* diagnostics) {
    if (rules.rules.tag.isEmpty()) return;
    const QHash<QString, QString> aliases = loadAliases(rules.rules);
    QSet<QString> matchedCases;
    const QString messageCase = message.caseId.trimmed();
    // A missing case is explicitly allowed by the design document.  In that
    // form CaseRules are informational and must not reject an otherwise valid
    // message; only an explicitly supplied case is checked against them.
    const XmlNode* cases = messageCase.isEmpty()
        ? nullptr : child(rules.rules, QStringLiteral("CaseRules"));
    if (cases) {
        for (const XmlNode* caseNode : children(*cases, QStringLiteral("Case"))) {
            const QString id = caseNode->attributes.value(QStringLiteral("id")).trimmed();
            if (caseNode->children.isEmpty()) continue;
            bool matched = false;
            QString error;
            if (!evaluateExpression(caseNode->children.first(), message.header, message.body,
                                    aliases, &matched, &error)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_EVAL"), id, error);
            } else if (matched) {
                matchedCases.insert(id);
            }
        }
        if (!messageCase.isEmpty() && !matchedCases.contains(messageCase)) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("CASE_MISMATCH"),
                          messageCase, QStringLiteral("消息 case 未匹配字典 CaseRules"));
        }
    }
    if (const XmlNode* conditions = child(rules.rules, QStringLiteral("ConditionRules"))) {
        for (const XmlNode* condition : children(*conditions, QStringLiteral("Condition"))) {
            const QString id = condition->attributes.value(QStringLiteral("id")).trimmed();
            const QString caseRef = condition->attributes.value(QStringLiteral("caseRef")).trimmed();
            if (!caseRef.isEmpty() && !matchedCases.contains(caseRef)) continue;
            const XmlNode* ifNode = child(*condition, QStringLiteral("If"));
            const XmlNode* thenNode = child(*condition, QStringLiteral("Then"));
            if (!ifNode || !thenNode || ifNode->children.isEmpty() || thenNode->children.isEmpty()) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_SHAPE"), id,
                              QStringLiteral("Condition 缺少 If/Then 表达式"));
                continue;
            }
            bool ifValue = false;
            bool thenValue = false;
            QString error;
            if (!evaluateExpression(ifNode->children.first(), message.header, message.body,
                                    aliases, &ifValue, &error)
                || !evaluateExpression(thenNode->children.first(), message.header, message.body,
                                       aliases, &thenValue, &error)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("RULE_EVAL"), id, error);
            } else if (ifValue && !thenValue) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("CONDITION_FAILED"), id,
                              QStringLiteral("Condition 的 Then 不满足"));
            }
        }
    }
}

void populateCanonicalNames(const SpecNode& spec, XmlNode* actual) {
    if (!actual) return;
    for (const SpecNode& childSpec : spec.children) {
        for (XmlNode& candidate : actual->children) {
            if (candidate.tag != childSpec.tag) continue;
            if (!childSpec.name.isEmpty()
                && childSpec.tag != QLatin1String("DataUnit")
                && candidate.attributes.value(QStringLiteral("name")) != childSpec.name) continue;
            if (childSpec.tag == QLatin1String("DataUnit")
                && !childSpec.name.isEmpty()
                && !candidate.attributes.value(QStringLiteral("name")).isEmpty()
                && candidate.attributes.value(QStringLiteral("name")) != childSpec.name) continue;
            if (childSpec.tag == QLatin1String("DataUnit")
                && (!childSpec.dfi.isEmpty()
                    && candidate.attributes.value(QStringLiteral("DFI")) != childSpec.dfi)) continue;
            if (childSpec.tag == QLatin1String("DataUnit")
                && (!childSpec.dui.isEmpty()
                    && candidate.attributes.value(QStringLiteral("DUI")) != childSpec.dui)) continue;
            if (candidate.tag == QLatin1String("DataUnit")
                && candidate.attributes.value(QStringLiteral("name")).isEmpty()) {
                candidate.attributes.insert(QStringLiteral("name"), childSpec.name);
            }
            if (!isValueNode(childSpec)) populateCanonicalNames(childSpec, &candidate);
        }
    }
}

class BitWriter {
public:
    bool write(quint64 value, int bits, QList<Diagnostic>* diagnostics, const QString& path) {
        if (bits <= 0 || bits > 64 || (bits < 64 && value > ((quint64{1} << bits) - 1U))) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("ENCODE_RANGE"), path,
                          QStringLiteral("值超出位宽"));
            return false;
        }
        for (int index = bits - 1; index >= 0; --index) {
            const int position = m_bitLength++;
            if (position / 8 >= m_bytes.size()) m_bytes.append(char(0));
            if ((value >> index) & 1U) {
                m_bytes[position / 8] = char(static_cast<unsigned char>(m_bytes.at(position / 8))
                                              | static_cast<unsigned char>(1U << (7 - position % 8)));
            }
        }
        return true;
    }

    bool set(int offset, quint64 value, int bits, QList<Diagnostic>* diagnostics,
             const QString& path) {
        if (offset < 0 || offset + bits > m_bitLength || bits <= 0 || bits > 64
            || (bits < 64 && value > ((quint64{1} << bits) - 1U))) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("ENCODE_SET"), path,
                          QStringLiteral("长度字段回填失败"));
            return false;
        }
        for (int index = 0; index < bits; ++index) {
            const int position = offset + index;
            auto byte = static_cast<unsigned char>(m_bytes.at(position / 8));
            const unsigned char mask = static_cast<unsigned char>(1U << (7 - position % 8));
            if ((value >> (bits - index - 1)) & 1U) byte |= mask;
            else byte &= static_cast<unsigned char>(~mask);
            m_bytes[position / 8] = static_cast<char>(byte);
        }
        return true;
    }

    QByteArray bytes() const { return m_bytes; }
    int bitLength() const { return m_bitLength; }

private:
    QByteArray m_bytes;
    int m_bitLength = 0;
};

class BitReader {
public:
    BitReader(const QByteArray& bytes, int bitLength)
        : m_bytes(bytes), m_bitLength(bitLength > 0 ? bitLength : bytes.size() * 8) {}

    bool read(int bits, quint64* value, QList<Diagnostic>* diagnostics, const QString& path) {
        if (!value || bits <= 0 || bits > 64 || m_offset + bits > m_bitLength) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DECODE_RANGE"), path,
                          QStringLiteral("bit 流长度不足"));
            return false;
        }
        quint64 result = 0;
        for (int index = 0; index < bits; ++index) {
            const int position = m_offset++;
            const unsigned char byte = static_cast<unsigned char>(m_bytes.at(position / 8));
            result = (result << 1U) | ((byte >> (7 - position % 8)) & 1U);
        }
        *value = result;
        return true;
    }

    int offset() const { return m_offset; }
    int bitLength() const { return m_bitLength; }

    bool remainingBitsAreZero() const {
        for (int position = m_offset; position < m_bitLength; ++position) {
            const unsigned char byte = static_cast<unsigned char>(m_bytes.at(position / 8));
            if ((byte >> (7 - position % 8)) & 1U) return false;
        }
        return true;
    }

private:
    const QByteArray& m_bytes;
    int m_bitLength = 0;
    int m_offset = 0;
};

const XmlNode* matchingActual(const XmlNode& parent, const SpecNode& spec, int index = 0) {
    const auto matches = findInstances(parent, spec);
    return index >= 0 && index < matches.size() ? matches.at(index) : nullptr;
}

bool encodeNodes(const SpecNode& spec, const XmlNode& actual, const QString& path,
                 BitWriter* writer, QVector<FieldLayout>* fields,
                 QList<Diagnostic>* diagnostics) {
    for (const SpecNode& childSpec : spec.children) {
        const auto matches = findInstances(actual, childSpec);
        const int count = childSpec.repeatable
            ? static_cast<int>(matches.size())
            : std::min(1, static_cast<int>(matches.size()));
        for (int index = 0; index < count; ++index) {
            const XmlNode* node = matches.at(index);
            const QString currentPath = instancePath(path, childSpec,
                                                      childSpec.repeatable ? index : -1);
            if (isValueNode(childSpec)) {
                quint64 value = 0;
                if (!readNodeValue(*node, &value)) {
                    addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("VALUE_PARSE"),
                                  currentPath, QStringLiteral("节点值不是整数"));
                    return false;
                }
                const int offset = writer->bitLength();
                if (!writer->write(value, childSpec.bits, diagnostics, currentPath)) return false;
                if (fields) fields->append(FieldLayout{currentPath, childSpec.tag, childSpec.name,
                                                        childSpec.dfi, childSpec.dui, value, offset,
                                                        childSpec.bits});
            } else if (!encodeNodes(childSpec, *node, currentPath, writer, fields, diagnostics)) {
                return false;
            }
        }
    }
    return true;
}

bool decodeNodes(const SpecNode& spec, XmlNode* actual, const QString& path,
                 BitReader* reader, QVector<FieldLayout>* fields,
                 QList<Diagnostic>* diagnostics) {
    if (!actual) return false;
    for (const SpecNode& childSpec : spec.children) {
        const int count = childSpec.repeatable ? 1 : 1;
        for (int index = 0; index < count; ++index) {
            const QString currentPath = instancePath(path, childSpec,
                                                      childSpec.repeatable ? index : -1);
            XmlNode node;
            node.tag = childSpec.tag;
            if (!childSpec.name.isEmpty()) node.attributes.insert(QStringLiteral("name"), childSpec.name);
            if (!childSpec.dfi.isEmpty()) node.attributes.insert(QStringLiteral("DFI"), childSpec.dfi);
            if (!childSpec.dui.isEmpty()) node.attributes.insert(QStringLiteral("DUI"), childSpec.dui);
            if (isValueNode(childSpec)) {
                quint64 value = 0;
                const int offset = reader->offset();
                if (!reader->read(childSpec.bits, &value, diagnostics, currentPath)) return false;
                node.text = QString::number(value);
                actual->children.append(node);
                if (fields) fields->append(FieldLayout{currentPath, childSpec.tag, childSpec.name,
                                                        childSpec.dfi, childSpec.dui, value, offset,
                                                        childSpec.bits});
                if (childSpec.repeatable) {
                    const QString indicatorTag = childSpec.tag == QLatin1String("Group")
                        ? QStringLiteral("GRI") : QStringLiteral("FRI");
                    if (childSpec.tag == QLatin1String("DataUnit")) {
                        Q_UNUSED(indicatorTag);
                    }
                }
            } else {
                if (!decodeNodes(childSpec, &node, currentPath, reader, fields, diagnostics)) return false;
                actual->children.append(node);
                if (childSpec.repeatable) {
                    const QString indicatorTag = childSpec.tag == QLatin1String("Group")
                        ? QStringLiteral("GRI") : QStringLiteral("FRI");
                    const XmlNode* indicator = child(node, indicatorTag);
                    quint64 repeat = 0;
                    if (!indicator || !parseUnsigned(indicator->text, &repeat)) return false;
                    if (repeat == 0) break;
                    if (repeat != 1) {
                        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DECODE_INDICATOR"),
                                      currentPath, QStringLiteral("重复指示位不是 0/1"));
                        return false;
                    }
                    // The dictionary's repeat indicator belongs to the group instance.
                    // Decode the following instance by rewinding the loop through a small
                    // recursive helper below.
                }
            }
        }
    }
    return true;
}

// Decoding repeatable containers needs a separate routine because the GRI/FRI
// bit is part of each instance and determines whether another instance follows.
// Presence indicators gate all following members of the same container.  This
// is important for a zero GPI/FPI: the absent branch contributes only its
// indicator bit and must not consume bits for the omitted payload.
bool decodeContainer(const SpecNode& spec, XmlNode* parent, const QString& path,
                     BitReader* reader, QVector<FieldLayout>* fields,
                     QList<Diagnostic>* diagnostics) {
    if (!parent) return false;

    const QString presenceTag = spec.tag == QLatin1String("Group")
        ? QStringLiteral("GPI") : QStringLiteral("FPI");
    bool present = true;
    bool presenceSeen = !spec.selectable;
    if (spec.selectable) {
        if (spec.children.isEmpty() || spec.children.first().tag != presenceTag) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_SELECTABLE_ORDER"),
                          path, QStringLiteral("可选容器的 GPI/FPI 必须是第一个子节点"));
            return false;
        }
    }

    auto decodeValue = [&](const SpecNode& childSpec, const QString& childPath) {
        XmlNode node;
        node.tag = childSpec.tag;
        if (!childSpec.name.isEmpty()) node.attributes.insert(QStringLiteral("name"), childSpec.name);
        if (!childSpec.dfi.isEmpty()) node.attributes.insert(QStringLiteral("DFI"), childSpec.dfi);
        if (!childSpec.dui.isEmpty()) node.attributes.insert(QStringLiteral("DUI"), childSpec.dui);
        const int offset = reader->offset();
        quint64 value = 0;
        if (!reader->read(childSpec.bits, &value, diagnostics, childPath)) return false;
        node.text = QString::number(value);
        parent->children.append(std::move(node));
        if (fields) fields->append(FieldLayout{childPath, childSpec.tag, childSpec.name,
                                                childSpec.dfi, childSpec.dui, value, offset,
                                                childSpec.bits});
        return true;
    };

    for (int childIndex = 0; childIndex < spec.children.size(); ++childIndex) {
        const SpecNode& childSpec = spec.children.at(childIndex);
        const QString childPath = instancePath(path, childSpec);

        if (spec.selectable && !presenceSeen) {
            if (childSpec.tag != presenceTag) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error,
                              QStringLiteral("DICT_SELECTABLE_ORDER"), path,
                              QStringLiteral("可选容器缺少 GPI/FPI"));
                return false;
            }
            presenceSeen = true;
            if (!decodeValue(childSpec, childPath)) return false;
            const XmlNode* indicator = child(*parent, presenceTag);
            quint64 value = 0;
            present = indicator && readNodeValue(*indicator, &value) && value == 1;
            if (!indicator || (value != 0 && value != 1)) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error,
                              QStringLiteral("DECODE_INDICATOR"), childPath,
                              QStringLiteral("存在指示位不是 0/1"));
                return false;
            }
            if (!present) return true;
            continue;
        }
        if (spec.selectable && !present) continue;

        if (isValueNode(childSpec)) {
            if (!decodeValue(childSpec, childPath)) return false;
            continue;
        }

        if (childSpec.repeatable) {
            if (childSpec.tag != QLatin1String("Group")
                && childSpec.tag != QLatin1String("Field")) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_REPEAT"),
                              childPath, QStringLiteral("只有 Group/Field 支持 repeatable"));
                return false;
            }
            const QString repeatTag = childSpec.tag == QLatin1String("Group")
                ? QStringLiteral("GRI") : QStringLiteral("FRI");
            const int maxCount = childSpec.maxRepeat > 0 ? childSpec.maxRepeat : 1024;
            bool terminated = false;
            for (int index = 0; index < maxCount; ++index) {
                XmlNode node;
                node.tag = childSpec.tag;
                if (!childSpec.name.isEmpty()) {
                    node.attributes.insert(QStringLiteral("name"), childSpec.name);
                }
                const QString instance = instancePath(path, childSpec, index);
                if (!decodeContainer(childSpec, &node, instance, reader, fields, diagnostics)) return false;
                const XmlNode* indicator = child(node, repeatTag);
                quint64 repeat = 0;
                if (!indicator) {
                    // A zero presence indicator suppresses the remaining
                    // members, including a repeat indicator, and ends this
                    // repeatable sequence.
                    const XmlNode* optional = child(node, presenceTag);
                    quint64 optionalValue = 1;
                    if (childSpec.selectable && optional && readNodeValue(*optional, &optionalValue)
                        && optionalValue == 0) {
                        parent->children.append(std::move(node));
                        terminated = true;
                        break;
                    }
                    addDiagnostic(diagnostics, Diagnostic::Severity::Error,
                                  QStringLiteral("DECODE_INDICATOR"), instance,
                                  QStringLiteral("重复容器缺少 GRI/FRI"));
                    return false;
                }
                if (!readNodeValue(*indicator, &repeat) || repeat > 1) {
                    addDiagnostic(diagnostics, Diagnostic::Severity::Error,
                                  QStringLiteral("DECODE_INDICATOR"), instance,
                                  QStringLiteral("重复指示位不是 0/1"));
                    return false;
                }
                parent->children.append(std::move(node));
                if (repeat == 0) {
                    terminated = true;
                    break;
                }
            }
            if (!terminated) {
                addDiagnostic(diagnostics, Diagnostic::Severity::Error,
                              QStringLiteral("DECODE_REPEAT_MAX"), childPath,
                              QStringLiteral("重复容器超过最大可解码实例数"));
                return false;
            }
            continue;
        }

        XmlNode node;
        node.tag = childSpec.tag;
        if (!childSpec.name.isEmpty()) node.attributes.insert(QStringLiteral("name"), childSpec.name);
        if (!decodeContainer(childSpec, &node, childPath, reader, fields, diagnostics)) return false;
        parent->children.append(std::move(node));
    }
    return true;
}

bool decodeRoot(const SpecNode& headerSpec, const SpecNode& bodySpec,
                MessageXml* output, BitReader* reader, QVector<FieldLayout>* fields,
                QList<Diagnostic>* diagnostics) {
    output->header.tag = QStringLiteral("Header");
    output->body.tag = QStringLiteral("Body");
    if (!decodeContainer(headerSpec, &output->header, QStringLiteral("Header"), reader,
                         fields, diagnostics)) return false;
    if (!decodeContainer(bodySpec, &output->body, QStringLiteral("Body"), reader,
                         fields, diagnostics)) return false;
    return true;
}

} // namespace

struct DictionarySet::Private {
    QHash<Key, ContentItem> content;
    QHash<QString, MessageSpec> messages;
    bool valid = false;
};

const MessageSpec* findMessage(const DictionarySet::Private& data, const QString& name) {
    const auto it = data.messages.constFind(name);
    return it == data.messages.cend() ? nullptr : &it.value();
}

const XmlNode* XmlNode::firstChild(const QString& childTag, const QString& name) const {
    return ::gbr::vmf::child(*this, childTag, name);
}

QVector<const XmlNode*> XmlNode::childrenNamed(const QString& childTag, const QString& name) const {
    return ::gbr::vmf::children(*this, childTag, name);
}

XmlNode* XmlNode::firstChild(const QString& childTag, const QString& name) {
    for (XmlNode& node : children) {
        if (node.tag == childTag && (name.isEmpty()
                                     || node.attributes.value(QStringLiteral("name")) == name)) return &node;
    }
    return nullptr;
}

bool MessageXml::parse(const QByteArray& data, MessageXml* output,
                       QList<Diagnostic>* diagnostics) {
    if (!output) return false;
    XmlNode root;
    if (!parseDocument(data, &root, diagnostics)) return false;
    if (root.tag != QLatin1String("MessageContent")) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("MESSAGE_ROOT"), {},
                      QStringLiteral("消息根节点必须是 MessageContent"));
        return false;
    }
    const XmlNode* header = child(root, QStringLiteral("Header"));
    const XmlNode* body = child(root, QStringLiteral("Body"));
    if (!header || !body) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("MESSAGE_SHAPE"), {},
                      QStringLiteral("消息缺少 Header 或 Body"));
        return false;
    }
    output->message = root.attributes.value(QStringLiteral("message"));
    output->caseId = root.attributes.value(QStringLiteral("case"));
    output->header = *header;
    output->body = *body;
    return true;
}

QByteArray MessageXml::serialize(bool indent) const {
    QByteArray output;
    QXmlStreamWriter writer(&output);
    writer.setAutoFormatting(indent);
    writer.setAutoFormattingIndent(2);
    writer.writeStartElement(QStringLiteral("MessageContent"));
    writer.writeAttribute(QStringLiteral("message"), message);
    if (!caseId.isEmpty()) writer.writeAttribute(QStringLiteral("case"), caseId);
    writeXmlNode(writer, header);
    writeXmlNode(writer, body);
    writer.writeEndElement();
    return output;
}

QString ValidationReport::summary() const {
    if (valid) return QStringLiteral("Validation PASS");
    return Codec::diagnosticsToString(diagnostics);
}

DictionarySet::DictionarySet() : d(std::make_unique<Private>()) {}
DictionarySet::~DictionarySet() = default;
DictionarySet::DictionarySet(DictionarySet&&) noexcept = default;
DictionarySet& DictionarySet::operator=(DictionarySet&&) noexcept = default;

std::shared_ptr<DictionarySet> DictionarySet::fromFiles(const QString& messageDictionaryPath,
                                                        const QString& contentDictionaryPath,
                                                        QList<Diagnostic>* diagnostics) {
    return fromFiles(QStringList{messageDictionaryPath}, contentDictionaryPath, diagnostics);
}

std::shared_ptr<DictionarySet> DictionarySet::fromFiles(const QStringList& messageDictionaryPaths,
                                                        const QString& contentDictionaryPath,
                                                        QList<Diagnostic>* diagnostics) {
    if (messageDictionaryPaths.isEmpty()) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_IO"), {},
                      QStringLiteral("未提供 VMF 消息字典"));
        return {};
    }
    QList<QByteArray> messageDictionaries;
    messageDictionaries.reserve(messageDictionaryPaths.size());
    for (const QString& path : messageDictionaryPaths) {
        QFile messageFile(path);
        if (!messageFile.open(QIODevice::ReadOnly)) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_IO"), path,
                          QStringLiteral("无法读取 VMF 消息字典文件"));
            return {};
        }
        messageDictionaries.append(messageFile.readAll());
    }
    QFile contentFile(contentDictionaryPath);
    if (!contentFile.open(QIODevice::ReadOnly)) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_IO"), {},
                      QStringLiteral("无法读取 VMF 字典文件"));
        return {};
    }
    return fromXmlSet(messageDictionaries, contentFile.readAll(), diagnostics);
}

std::shared_ptr<DictionarySet> DictionarySet::fromXml(const QByteArray& messageDictionary,
                                                      const QByteArray& contentDictionary,
                                                      QList<Diagnostic>* diagnostics) {
    return fromXmlSet(QList<QByteArray>{messageDictionary}, contentDictionary, diagnostics);
}

std::shared_ptr<DictionarySet> DictionarySet::fromXmlSet(const QList<QByteArray>& messageDictionaries,
                                                         const QByteArray& contentDictionary,
                                                         QList<Diagnostic>* diagnostics) {
    auto result = std::make_shared<DictionarySet>();
    if (!parseContent(contentDictionary, &result->d->content, diagnostics)) return {};
    if (messageDictionaries.isEmpty()) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_EMPTY"), {},
                      QStringLiteral("消息字典集合为空"));
        return {};
    }
    for (const QByteArray& messageDictionary : messageDictionaries) {
        MessageSpec message;
        if (!parseMessage(messageDictionary, &message, &result->d->content, diagnostics)) return {};
        if (result->d->messages.contains(message.name)) {
            addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("DICT_DUPLICATE_MESSAGE"),
                          message.name, QStringLiteral("消息名称重复"));
            return {};
        }
        result->d->messages.insert(message.name, std::move(message));
    }
    bool hasError = false;
    if (diagnostics) {
        hasError = std::any_of(diagnostics->cbegin(), diagnostics->cend(),
                               [](const Diagnostic& diagnostic) {
                                   return diagnostic.severity == Diagnostic::Severity::Error;
                               });
    }
    result->d->valid = !hasError;
    return result->d->valid ? result : std::shared_ptr<DictionarySet>{};
}

bool DictionarySet::isValid() const { return d && d->valid; }
QStringList DictionarySet::messageNames() const { return d ? d->messages.keys() : QStringList{}; }
bool DictionarySet::hasMessage(const QString& name) const { return d && d->messages.contains(name); }

Codec::Codec(std::shared_ptr<const DictionarySet> dictionaries)
    : m_dictionaries(std::move(dictionaries)) {}

ValidationReport Codec::validate(const QString& messageName, const MessageXml& message) const {
    ValidationReport report;
    if (!m_dictionaries || !m_dictionaries->d) {
        report.diagnostics.append({Diagnostic::Severity::Error, QStringLiteral("NO_DICT"), {},
                                   QStringLiteral("VMF 字典未加载")});
        return report;
    }
    const MessageSpec* spec = findMessage(*m_dictionaries->d, messageName);
    if (!spec) {
        report.diagnostics.append({Diagnostic::Severity::Error, QStringLiteral("UNKNOWN_MESSAGE"), {},
                                   QStringLiteral("未找到消息字典: %1").arg(messageName)});
        return report;
    }
    if (!message.message.isEmpty() && message.message != spec->name) {
        report.diagnostics.append({Diagnostic::Severity::Error, QStringLiteral("MESSAGE_NAME"), {},
                                   QStringLiteral("消息名称与字典不一致")});
    }
    validateChildren(spec->header, message.header, QStringLiteral("Header"), m_dictionaries->d->content,
                     &report.diagnostics);
    validateChildren(spec->body, message.body, QStringLiteral("Body"), m_dictionaries->d->content,
                     &report.diagnostics);
    MessageXml rulesMessage = message;
    populateCanonicalNames(spec->header, &rulesMessage.header);
    populateCanonicalNames(spec->body, &rulesMessage.body);
    validateRules(spec->rules, rulesMessage, &report.diagnostics);
    report.valid = std::none_of(report.diagnostics.cbegin(), report.diagnostics.cend(),
                                [](const Diagnostic& diagnostic) {
                                    return diagnostic.severity == Diagnostic::Severity::Error;
                                });
    return report;
}

ValidationReport Codec::validate(const MessageXml& message) const {
    return validate(message.message, message);
}

bool Codec::encode(const QString& messageName, const MessageXml& message,
                   EncodedMessage* output, QList<Diagnostic>* diagnostics) const {
    if (!output) return false;
    const ValidationReport report = validate(messageName, message);
    if (!report.valid) {
        if (diagnostics) *diagnostics = report.diagnostics;
        return false;
    }
    const MessageSpec* spec = findMessage(*m_dictionaries->d, messageName);
    BitWriter writer;
    QVector<FieldLayout> fields;
    if (!encodeNodes(spec->header, message.header, QStringLiteral("Header"), &writer, &fields, diagnostics)
        || !encodeNodes(spec->body, message.body, QStringLiteral("Body"), &writer, &fields, diagnostics)) {
        return false;
    }
    for (const FieldLayout& field : fields) {
        if (field.name != QLatin1String("length")) continue;
        const quint64 byteLength = static_cast<quint64>((writer.bitLength() + 7) / 8);
        if (!writer.set(field.bitOffset, byteLength, field.bits, diagnostics, field.path)) return false;
        break;
    }
    output->message = messageName;
    output->bytes = writer.bytes();
    output->bitLength = writer.bitLength();
    output->canonicalXml = message;
    output->canonicalXml.message = messageName;
    output->fields = std::move(fields);
    return true;
}

bool Codec::decode(const QString& messageName, const QByteArray& bytes, int bitLength,
                   DecodedMessage* output, QList<Diagnostic>* diagnostics) const {
    if (!output) return false;
    const MessageSpec* spec = findMessage(*m_dictionaries->d, messageName);
    if (!spec) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("UNKNOWN_MESSAGE"), {},
                      QStringLiteral("未找到消息字典: %1").arg(messageName));
        return false;
    }
    const int actualBits = bitLength > 0 ? bitLength : bytes.size() * 8;
    if (actualBits <= 0 || actualBits > bytes.size() * 8) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("BIT_LENGTH"), {},
                      QStringLiteral("bitLength 无效"));
        return false;
    }
    BitReader reader(bytes, actualBits);
    MessageXml message;
    message.message = messageName;
    QVector<FieldLayout> fields;
    if (!decodeRoot(spec->header, spec->body, &message, &reader, &fields, diagnostics)) return false;
    if (!reader.remainingBitsAreZero()) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("PADDING"), {},
                      QStringLiteral("bit 流尾部包含非零填充"));
        return false;
    }
    quint64 declaredBytes = 0;
    for (const FieldLayout& field : fields) {
        if (field.name == QLatin1String("length")) {
            declaredBytes = field.value;
            break;
        }
    }
    if (declaredBytes != static_cast<quint64>((actualBits + 7) / 8)) {
        addDiagnostic(diagnostics, Diagnostic::Severity::Error, QStringLiteral("LENGTH"), {},
                      QStringLiteral("Header length 与实际字节数不一致"));
        return false;
    }
    const ValidationReport report = validate(messageName, message);
    if (!report.valid) {
        if (diagnostics) *diagnostics += report.diagnostics;
        return false;
    }
    output->message = messageName;
    output->xml = std::move(message);
    output->fields = std::move(fields);
    output->bitLength = reader.offset();
    return true;
}

QString Codec::diagnosticsToString(const QList<Diagnostic>& diagnostics) {
    QStringList lines;
    for (const Diagnostic& diagnostic : diagnostics) {
        lines.append(QStringLiteral("%1: %2 [%3] %4")
                         .arg(diagnostic.severity == Diagnostic::Severity::Error ? QStringLiteral("error")
                                                                                   : QStringLiteral("warning"),
                              diagnostic.code, diagnostic.path, diagnostic.message));
    }
    return lines.join(QLatin1Char('\n'));
}

QString diagnosticsToJson(const QList<Diagnostic>& diagnostics) {
    QStringList lines;
    for (const Diagnostic& diagnostic : diagnostics) {
        lines.append(QStringLiteral("%1|%2|%3|%4")
                         .arg(diagnostic.severity == Diagnostic::Severity::Error ? QStringLiteral("error")
                                                                                   : QStringLiteral("warning"),
                              diagnostic.code, diagnostic.path, diagnostic.message));
    }
    return lines.join(QLatin1Char('\n'));
}

} // namespace gbr::vmf
