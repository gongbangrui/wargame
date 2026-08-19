#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <tinyxml2.h>

using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;

namespace {

struct DfiDuiKey {
  std::string dfi;
  std::string dui;

  bool operator==(const DfiDuiKey& other) const { return dfi == other.dfi && dui == other.dui; }
};

struct DfiDuiKeyHash {
  size_t operator()(const DfiDuiKey& k) const {
    return std::hash<std::string>{}(k.dfi) ^ (std::hash<std::string>{}(k.dui) << 1U);
  }
};

enum class IndexMode { kNone, kExact, kAll };

struct Segment {
  char kind = '\0';
  std::string name;
  IndexMode index_mode = IndexMode::kNone;
  int index = 0;
};

std::string Trim(std::string s) {
  auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c) { return !is_space(c); }));
  s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c) { return !is_space(c); }).base(), s.end());
  return s;
}

bool IsTrue(const char* value) {
  if (!value) return false;
  std::string s(value);
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s == "true" || s == "1";
}

uint64_t ParseUInt(const char* text, const std::string& ctx) {
  if (!text) throw std::runtime_error("Missing numeric text at " + ctx);
  std::string s = Trim(text);
  if (s.empty()) throw std::runtime_error("Empty numeric text at " + ctx);
  size_t idx = 0;
  const unsigned long long v = std::stoull(s, &idx, 0);
  if (idx != s.size()) throw std::runtime_error("Invalid numeric text at " + ctx + ": " + s);
  return static_cast<uint64_t>(v);
}

int ParseBits(const XMLElement* elem, const std::string& ctx) {
  const char* bits = elem->Attribute("bits");
  if (!bits) throw std::runtime_error("Dictionary config error: missing bits at " + ctx);
  const uint64_t v = ParseUInt(bits, ctx + ".bits");
  if (v == 0 || v > 64) throw std::runtime_error("Dictionary config error: invalid bits at " + ctx + ": " + bits);
  return static_cast<int>(v);
}

std::vector<std::string> SplitPath(const std::string& s) {
  std::vector<std::string> parts;
  std::string cur;
  int depth = 0;
  for (char ch : s) {
    if (ch == '(') depth++;
    if (ch == ')') depth--;
    if (ch == '.' && depth == 0) {
      parts.push_back(cur);
      cur.clear();
      continue;
    }
    cur.push_back(ch);
  }
  if (!cur.empty()) parts.push_back(cur);
  return parts;
}

Segment ParseSegment(const std::string& seg, const std::string& path) {
  if (seg.size() < 4) throw std::runtime_error("Invalid path segment in " + path + ": " + seg);
  Segment s;
  s.kind = seg[0];
  if (s.kind != 'G' && s.kind != 'F' && s.kind != 'D' && s.kind != 'I') {
    throw std::runtime_error("Unsupported segment kind in " + path + ": " + seg);
  }
  if (seg[1] != '(') throw std::runtime_error("Invalid segment syntax in " + path + ": " + seg);
  const auto close = seg.find(')');
  if (close == std::string::npos || close <= 2) {
    throw std::runtime_error("Invalid segment syntax in " + path + ": " + seg);
  }
  s.name = seg.substr(2, close - 2);
  if (close + 1 == seg.size()) return s;
  if (seg[close + 1] != '[' || seg.back() != ']') {
    throw std::runtime_error("Invalid index syntax in " + path + ": " + seg);
  }
  const std::string idx = seg.substr(close + 2, seg.size() - close - 3);
  if (idx.empty()) throw std::runtime_error("Empty index in " + path + ": " + seg);
  if (idx == "*") {
    s.index_mode = IndexMode::kAll;
    return s;
  }
  s.index = std::stoi(idx);
  s.index_mode = IndexMode::kExact;
  return s;
}

std::vector<const XMLElement*> FindNamedChildren(const XMLElement* parent, const char* tag, const std::string& name) {
  std::vector<const XMLElement*> out;
  for (const XMLElement* c = parent->FirstChildElement(tag); c; c = c->NextSiblingElement(tag)) {
    const char* n = c->Attribute("name");
    if (n && name == n) out.push_back(c);
  }
  return out;
}

const XMLElement* FindUniqueNamedDictChild(const XMLElement* parent, const char* tag, const std::string& name,
                                           const std::string& path) {
  const XMLElement* found = nullptr;
  for (const XMLElement* c = parent->FirstChildElement(tag); c; c = c->NextSiblingElement(tag)) {
    const char* n = c->Attribute("name");
    if (!n || name != n) continue;
    if (found) throw std::runtime_error("Dictionary config error: duplicate " + std::string(tag) + "(" + name +
                                        ") under " + path);
    found = c;
  }
  if (!found) {
    throw std::runtime_error("Dictionary config error: missing " + std::string(tag) + "(" + name + ") under " +
                             path);
  }
  return found;
}

std::string RequireAttr(const XMLElement* elem, const char* attr, const std::string& ctx) {
  const char* value = elem->Attribute(attr);
  if (!value) throw std::runtime_error("Missing attribute " + std::string(attr) + " at " + ctx);
  return value;
}

const XMLElement* FindIndicatorChild(const XMLElement* parent, const std::string& tag) {
  for (const XMLElement* c = parent->FirstChildElement(); c; c = c->NextSiblingElement()) {
    if (tag == c->Name()) return c;
  }
  return nullptr;
}

std::vector<const XMLElement*> FindDataUnitChildrenByKey(const XMLElement* parent, const DfiDuiKey& key) {
  std::vector<const XMLElement*> out;
  for (const XMLElement* c = parent->FirstChildElement("DataUnit"); c; c = c->NextSiblingElement("DataUnit")) {
    const char* dfi = c->Attribute("DFI");
    const char* dui = c->Attribute("DUI");
    if (dfi && dui && key.dfi == dfi && key.dui == dui) out.push_back(c);
  }
  return out;
}

size_t ResolveIndex(int index, size_t size, const std::string& path) {
  const long long resolved = index >= 0 ? static_cast<long long>(index) : static_cast<long long>(size) + index;
  if (resolved < 0 || resolved >= static_cast<long long>(size)) {
    throw std::runtime_error("Path index out of range: " + path);
  }
  return static_cast<size_t>(resolved);
}

template <typename T>
std::vector<T> SelectMatches(const std::vector<T>& matches, const Segment& seg, const std::string& path,
                             bool repeatable) {
  if (matches.empty()) {
    throw std::runtime_error("Path not found: " + path + ", segment " + seg.name);
  }
  if (seg.index_mode == IndexMode::kAll) {
    if (!repeatable) throw std::runtime_error("Wildcard index only allowed on repeatable path: " + path);
    return matches;
  }
  if (seg.index_mode == IndexMode::kExact) {
    if (!repeatable) throw std::runtime_error("Indexed path only allowed on repeatable path: " + path);
    return {matches[ResolveIndex(seg.index, matches.size(), path)]};
  }
  if (matches.size() != 1) {
    throw std::runtime_error("Ambiguous path (missing index): " + path);
  }
  return {matches.front()};
}

struct AllowConstraint {
  std::vector<uint64_t> values;
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
};

struct ContentItem {
  std::string name;
  int bits = -1;
  AllowConstraint allow;
};

class ContentDictionary {
 public:
  explicit ContentDictionary(const XMLElement* content_root) { Build(content_root); }

  const ContentItem* Find(const DfiDuiKey& key) const {
    auto it = items_.find(key);
    if (it == items_.end()) return nullptr;
    return &it->second;
  }

 private:
  void Build(const XMLElement* content_root) {
    if (!content_root) throw std::runtime_error("Dictionary config error: dic_content root is null");
    for (const XMLElement* dfi = content_root->FirstChildElement("DFI"); dfi; dfi = dfi->NextSiblingElement("DFI")) {
      const char* dfi_num = dfi->Attribute("num");
      if (!dfi_num) throw std::runtime_error("Dictionary config error: DFI missing num");
      for (const XMLElement* dui = dfi->FirstChildElement("DUI"); dui; dui = dui->NextSiblingElement("DUI")) {
        const char* dui_num = dui->Attribute("num");
        const char* dui_name = dui->Attribute("name");
        if (!dui_num || !dui_name) {
          throw std::runtime_error("Dictionary config error: DUI missing num/name under DFI " + std::string(dfi_num));
        }
        const DfiDuiKey key{dfi_num, dui_num};
        if (items_.count(key) > 0) {
          throw std::runtime_error("Dictionary config error: duplicate DFI/DUI in dic_content: DFI=" + key.dfi +
                                   " DUI=" + key.dui);
        }
        ContentItem item;
        item.name = dui_name;
        const char* bits = dui->Attribute("bits");
        if (bits) item.bits = ParseBits(dui, "dic_content.DFI(" + key.dfi + ").DUI(" + key.dui + ")");
        ParseAllowConstraint(dui, key, &item.allow);
        items_.emplace(key, std::move(item));
      }
    }
  }

  void ParseAllowConstraint(const XMLElement* dui, const DfiDuiKey& key, AllowConstraint* out) {
    const XMLElement* constraints = dui->FirstChildElement("Constraints");
    if (!constraints) return;
    const XMLElement* allow = constraints->FirstChildElement("AllowList");
    if (!allow) return;
    for (const XMLElement* node = allow->FirstChildElement(); node; node = node->NextSiblingElement()) {
      const std::string n = node->Name();
      if (n == "Value") {
        const char* v = node->Attribute("v");
        if (!v) {
          throw std::runtime_error("Dictionary config error: AllowList.Value missing v at DFI=" + key.dfi +
                                   " DUI=" + key.dui);
        }
        out->values.push_back(ParseUInt(v, "dic_content.AllowList.Value"));
      } else if (n == "Range") {
        const char* from = node->Attribute("from");
        const char* to = node->Attribute("to");
        if (!from || !to) {
          throw std::runtime_error("Dictionary config error: AllowList.Range missing from/to at DFI=" + key.dfi +
                                   " DUI=" + key.dui);
        }
        const uint64_t a = ParseUInt(from, "dic_content.AllowList.Range.from");
        const uint64_t b = ParseUInt(to, "dic_content.AllowList.Range.to");
        if (a > b) {
          throw std::runtime_error("Dictionary config error: AllowList.Range from>to at DFI=" + key.dfi +
                                   " DUI=" + key.dui);
        }
        out->ranges.push_back({a, b});
      } else {
        throw std::runtime_error("Dictionary config error: unsupported AllowList node " + n + " at DFI=" + key.dfi +
                                 " DUI=" + key.dui);
      }
    }
  }

  std::unordered_map<DfiDuiKey, ContentItem, DfiDuiKeyHash> items_;
};

struct MessageDataUnitDef {
  DfiDuiKey key;
  std::string name;
  int bits = 0;
};

class MessageDictionary {
 public:
  explicit MessageDictionary(const XMLElement* dict_msg) { Build(dict_msg); }

  const MessageDataUnitDef* FindDataUnit(const DfiDuiKey& key) const {
    auto it = data_units_.find(key);
    if (it == data_units_.end()) return nullptr;
    return &it->second;
  }

 private:
  void Build(const XMLElement* dict_msg) {
    const XMLElement* body = dict_msg ? dict_msg->FirstChildElement("Body") : nullptr;
    if (!body) throw std::runtime_error("Dictionary config error: dic_msg missing Body");
    Walk(body, "Body");
  }

  void Walk(const XMLElement* dict_parent, const std::string& path) {
    for (const XMLElement* c = dict_parent->FirstChildElement(); c; c = c->NextSiblingElement()) {
      const std::string tag = c->Name();
      if (tag == "DataUnit") {
        MessageDataUnitDef def;
        def.key.dfi = RequireAttr(c, "DFI", path + ".DataUnit");
        def.key.dui = RequireAttr(c, "DUI", path + ".DataUnit");
        def.name = RequireAttr(c, "name", path + ".DataUnit");
        def.bits = ParseBits(c, path + ".DataUnit(" + def.name + ")");
        auto it = data_units_.find(def.key);
        if (it == data_units_.end()) {
          data_units_.emplace(def.key, std::move(def));
        } else if (it->second.name != def.name || it->second.bits != def.bits) {
          throw std::runtime_error("Dictionary config error: DFI/DUI maps to multiple DataUnit definitions: DFI=" +
                                   def.key.dfi + " DUI=" + def.key.dui);
        }
      } else if (tag == "Group" || tag == "Field") {
        const char* name = c->Attribute("name");
        if (!name) throw std::runtime_error("Dictionary config error: " + tag + " missing name at " + path);
        Walk(c, path + "." + tag + "(" + name + ")");
      }
    }
  }

  std::unordered_map<DfiDuiKey, MessageDataUnitDef, DfiDuiKeyHash> data_units_;
};

class DictionaryConsistencyValidator {
 public:
  DictionaryConsistencyValidator(const XMLElement* dict_msg, const ContentDictionary* content_dict)
      : dict_msg_(dict_msg), content_dict_(content_dict) {}

  void ValidateOrThrow() const {
    std::unordered_map<DfiDuiKey, int, DfiDuiKeyHash> seen_bits;
    const XMLElement* header = dict_msg_ ? dict_msg_->FirstChildElement("Header") : nullptr;
    const XMLElement* body = dict_msg_ ? dict_msg_->FirstChildElement("Body") : nullptr;
    if (!header || !body) throw std::runtime_error("Dictionary config error: dic_msg missing Header/Body");
    Walk(header, "Header", &seen_bits);
    Walk(body, "Body", &seen_bits);
  }

 private:
  void Walk(const XMLElement* dict_parent, const std::string& path,
            std::unordered_map<DfiDuiKey, int, DfiDuiKeyHash>* seen_bits) const {
    for (const XMLElement* c = dict_parent->FirstChildElement(); c; c = c->NextSiblingElement()) {
      const std::string tag = c->Name();
      if (tag == "DataUnit" || tag == "GRI" || tag == "GPI" || tag == "FRI" || tag == "FPI") {
        const DfiDuiKey key{RequireAttr(c, "DFI", path + "." + tag), RequireAttr(c, "DUI", path + "." + tag)};
        const int bits = ParseBits(c, path + "." + tag);
        auto it_seen = seen_bits->find(key);
        if (it_seen == seen_bits->end()) {
          seen_bits->emplace(key, bits);
        } else if (it_seen->second != bits) {
          throw std::runtime_error("Dictionary config error: inconsistent bits in dic_msg for DFI=" + key.dfi +
                                   " DUI=" + key.dui);
        }
        const ContentItem* content_item = content_dict_->Find(key);
        if (!content_item) {
          throw std::runtime_error("Dictionary config error: missing DFI/DUI in dic_content: DFI=" + key.dfi +
                                   " DUI=" + key.dui);
        }
        if (content_item->bits < 0) {
          throw std::runtime_error("Dictionary config error: missing bits in dic_content for DFI=" + key.dfi +
                                   " DUI=" + key.dui);
        }
        if (content_item->bits != bits) {
          throw std::runtime_error("Dictionary config error: bits mismatch for DFI=" + key.dfi + " DUI=" + key.dui +
                                   " (dic_msg=" + std::to_string(bits) +
                                   ", dic_content=" + std::to_string(content_item->bits) + ")");
        }
        if (tag == "DataUnit") {
          const char* name = c->Attribute("name");
          if (!name) throw std::runtime_error("Dictionary config error: DataUnit missing name at " + path);
          if (content_item->name != name) {
            throw std::runtime_error("Dictionary config error: DataUnit name mismatch for DFI=" + key.dfi +
                                     " DUI=" + key.dui + " (dic_msg=" + name +
                                     ", dic_content=" + content_item->name + ")");
          }
        }
      } else if (tag == "Group" || tag == "Field") {
        const char* name = c->Attribute("name");
        if (!name) throw std::runtime_error("Dictionary config error: " + tag + " missing name at " + path);
        Walk(c, path + "." + tag + "(" + name + ")", seen_bits);
      }
    }
  }

  const XMLElement* dict_msg_;
  const ContentDictionary* content_dict_;
};

class MessageResolver {
 public:
  struct NodeRef {
    const XMLElement* dict = nullptr;
    const XMLElement* msg = nullptr;
  };

  MessageResolver(const XMLElement* dict_root, const XMLElement* msg_root, std::unordered_map<std::string, std::string> aliases)
      : dict_root_(dict_root), msg_root_(msg_root), aliases_(std::move(aliases)) {}

  std::vector<uint64_t> ResolveValues(const std::string& raw_path) const {
    const std::string expanded = ExpandAlias(raw_path, 0);
    bool repeat_suffix = false;
    std::string path = expanded;
    if (path.size() > 10 && path.substr(path.size() - 10) == ".iteration") {
      repeat_suffix = true;
      path.resize(path.size() - 10);
    }

    auto parts = SplitPath(path);
    if (parts.empty()) throw std::runtime_error("Empty path");

    std::vector<NodeRef> nodes;
    size_t idx = 0;
    if (parts[0] == "H") {
      nodes.push_back({dict_root_->FirstChildElement("Header"), msg_root_->FirstChildElement("Header")});
      idx = 1;
    } else if (parts[0] == "B") {
      nodes.push_back({dict_root_->FirstChildElement("Body"), msg_root_->FirstChildElement("Body")});
      idx = 1;
    } else {
      throw std::runtime_error("Path must start with H/B or alias: " + raw_path);
    }
    if (!nodes.front().dict || !nodes.front().msg) {
      throw std::runtime_error("Missing root node for path: " + raw_path);
    }

    if (repeat_suffix) {
      if (idx >= parts.size()) throw std::runtime_error(".iteration must target a Group/Field path: " + raw_path);
      for (; idx + 1 < parts.size(); ++idx) {
        nodes = TraverseSegment(parts[idx], raw_path, nodes);
      }
      const Segment target = ParseSegment(parts.back(), raw_path);
      if (target.kind != 'G' && target.kind != 'F') {
        throw std::runtime_error(".iteration only allowed on Group/Field: " + raw_path);
      }
      if (target.index_mode != IndexMode::kNone) {
        throw std::runtime_error(".iteration target must not carry an index: " + raw_path);
      }
      return ResolveRepeatCounts(target, raw_path, nodes);
    }

    for (; idx < parts.size(); ++idx) {
      const Segment seg = ParseSegment(parts[idx], raw_path);
      const bool is_leaf = (idx + 1 == parts.size());
      if (is_leaf && (seg.kind == 'D' || seg.kind == 'I')) {
        return ResolveLeafValues(seg, raw_path, nodes);
      }
      if (is_leaf) throw std::runtime_error("Path does not point to value leaf: " + raw_path);
      nodes = TraverseSegment(parts[idx], raw_path, nodes);
    }
    throw std::runtime_error("Path does not point to value leaf: " + raw_path);
  }

 private:
  std::string ExpandAlias(const std::string& path, int depth) const {
    if (depth > 8) throw std::runtime_error("Alias expansion too deep: " + path);
    if (path.empty() || path[0] != '$') return path;
    const size_t dot = path.find('.');
    const std::string key = path.substr(1, dot == std::string::npos ? std::string::npos : dot - 1);
    auto it = aliases_.find(key);
    if (it == aliases_.end()) throw std::runtime_error("Unknown alias: " + key);
    const std::string suffix = dot == std::string::npos ? "" : path.substr(dot);
    return ExpandAlias(it->second + suffix, depth + 1);
  }

  std::vector<uint64_t> ResolveRepeatCounts(const Segment& seg, const std::string& raw_path,
                                            const std::vector<NodeRef>& parents) const {
    std::vector<uint64_t> counts;
    for (const auto& parent : parents) {
      const char* tag = seg.kind == 'G' ? "Group" : "Field";
      const XMLElement* dict_child = FindUniqueNamedDictChild(parent.dict, tag, seg.name, raw_path);
      if (!IsTrue(dict_child->Attribute("repeatable"))) {
        throw std::runtime_error(".iteration only allowed on repeatable path: " + raw_path);
      }
      const char* indicator = seg.kind == 'G' ? "GRI" : "FRI";
      if (!FindIndicatorChild(dict_child, indicator)) {
        throw std::runtime_error(".iteration requires " + std::string(indicator) + " in dictionary: " + raw_path);
      }
      const auto matches = FindNamedChildren(parent.msg, tag, seg.name);
      if (matches.empty()) throw std::runtime_error("Path not found: " + raw_path);
      counts.push_back(static_cast<uint64_t>(matches.size()));
    }
    return counts;
  }

  std::vector<NodeRef> TraverseSegment(const std::string& seg_text, const std::string& raw_path,
                                       const std::vector<NodeRef>& nodes) const {
    const Segment seg = ParseSegment(seg_text, raw_path);
    if (seg.kind != 'G' && seg.kind != 'F') {
      throw std::runtime_error("Only Group/Field can be intermediate path nodes: " + raw_path);
    }
    std::vector<NodeRef> out;
    for (const auto& node : nodes) {
      const char* tag = seg.kind == 'G' ? "Group" : "Field";
      const XMLElement* dict_child = FindUniqueNamedDictChild(node.dict, tag, seg.name, raw_path);
      const bool repeatable = IsTrue(dict_child->Attribute("repeatable"));
      const auto msg_matches = FindNamedChildren(node.msg, tag, seg.name);
      const auto selected = SelectMatches(msg_matches, seg, raw_path, repeatable);
      for (const XMLElement* msg_child : selected) out.push_back({dict_child, msg_child});
    }
    return out;
  }

  std::vector<uint64_t> ResolveLeafValues(const Segment& seg, const std::string& raw_path,
                                          const std::vector<NodeRef>& nodes) const {
    if (seg.index_mode != IndexMode::kNone) {
      throw std::runtime_error("Leaf DataUnit/indicator does not support index syntax: " + raw_path);
    }
    std::vector<uint64_t> values;
    for (const auto& node : nodes) {
      if (seg.kind == 'D') {
        if (std::string(node.dict->Name()) == "Header" && std::string(node.msg->Name()) == "Header") {
          const XMLElement* dict_field = FindUniqueNamedDictChild(node.dict, "Field", seg.name, raw_path);
          const auto matches = FindNamedChildren(node.msg, "Field", seg.name);
          if (matches.size() != 1) throw std::runtime_error("Ambiguous header path: " + raw_path);
          values.push_back(ParseUInt(matches.front()->GetText(), raw_path));
          (void)dict_field;
          continue;
        }
        const XMLElement* dict_du = FindUniqueNamedDictChild(node.dict, "DataUnit", seg.name, raw_path);
        const DfiDuiKey key{RequireAttr(dict_du, "DFI", raw_path), RequireAttr(dict_du, "DUI", raw_path)};
        const auto matches = FindDataUnitChildrenByKey(node.msg, key);
        if (matches.size() != 1) throw std::runtime_error("Ambiguous DataUnit path: " + raw_path);
        values.push_back(ParseUInt(matches.front()->GetText(), raw_path));
      } else {
        const XMLElement* dict_ind = FindIndicatorChild(node.dict, seg.name);
        if (!dict_ind) throw std::runtime_error("Indicator not found in dictionary for path: " + raw_path);
        const XMLElement* msg_ind = FindIndicatorChild(node.msg, seg.name);
        if (!msg_ind) throw std::runtime_error("Indicator not found for path: " + raw_path);
        values.push_back(ParseUInt(msg_ind->GetText(), raw_path));
      }
    }
    return values;
  }

  const XMLElement* dict_root_;
  const XMLElement* msg_root_;
  std::unordered_map<std::string, std::string> aliases_;
};

class RuleValidator {
 public:
  RuleValidator(const XMLElement* dict_msg, const XMLElement* msg_root) : dict_msg_(dict_msg), msg_root_(msg_root) {
    if (const char* c = msg_root_->Attribute("case")) message_case_ = Trim(c);
    LoadAliases();
  }

  std::vector<std::string> Validate() {
    std::vector<std::string> failures;
    const XMLElement* rules = dict_msg_->FirstChildElement("Rules");
    if (!rules) return failures;
    std::unordered_set<std::string> matched_cases;
    if (!message_case_.empty()) {
      matched_cases = EvaluateCases(rules);
      if (matched_cases.count(message_case_) == 0) {
        failures.push_back("Case mismatch: message case=" + message_case_ + " not matched by CaseRules");
      }
    }
    EvaluateConditions(rules, matched_cases, &failures);
    return failures;
  }

 private:
  void LoadAliases() {
    const XMLElement* rules = dict_msg_->FirstChildElement("Rules");
    if (!rules) return;
    const XMLElement* aliases = rules->FirstChildElement("Aliases");
    if (!aliases) return;
    for (const XMLElement* a = aliases->FirstChildElement("Alias"); a; a = a->NextSiblingElement("Alias")) {
      const char* name = a->Attribute("name");
      const char* path = a->Attribute("path");
      if (!name || !path) throw std::runtime_error("Alias missing name/path");
      aliases_[name] = path;
    }
  }

  const XMLElement* FirstExprNode(const XMLElement* parent) const {
    for (const XMLElement* c = parent->FirstChildElement(); c; c = c->NextSiblingElement()) {
      const std::string n = c->Name();
      if (n == "AND" || n == "OR" || n == "Not" || n == "Cmp") return c;
    }
    return nullptr;
  }

  bool EvalExpr(const XMLElement* expr) const {
    if (!expr) throw std::runtime_error("Missing expression node");
    const std::string n = expr->Name();
    if (n == "AND") {
      bool any = false;
      for (const XMLElement* c = expr->FirstChildElement(); c; c = c->NextSiblingElement()) {
        any = true;
        if (!EvalExpr(c)) return false;
      }
      return any;
    }
    if (n == "OR") {
      for (const XMLElement* c = expr->FirstChildElement(); c; c = c->NextSiblingElement()) {
        if (EvalExpr(c)) return true;
      }
      return false;
    }
    if (n == "Not") {
      const XMLElement* c = expr->FirstChildElement();
      if (!c || c->NextSiblingElement()) throw std::runtime_error("<Not> must have exactly one child");
      return !EvalExpr(c);
    }
    if (n == "Cmp") return EvalCmp(expr);
    throw std::runtime_error("Unknown expr node: " + n);
  }

  bool EvalCmp(const XMLElement* cmp) const {
    const char* path = cmp->Attribute("path");
    const char* op = cmp->Attribute("op");
    const char* value = cmp->Attribute("value");
    const char* target_path = cmp->Attribute("targetPath");
    if (!path || !op) throw std::runtime_error("Cmp missing path/op");
    if ((value == nullptr) == (target_path == nullptr)) {
      throw std::runtime_error("Cmp must provide exactly one of value or targetPath");
    }

    MessageResolver resolver(dict_msg_, msg_root_, aliases_);
    const std::vector<uint64_t> lhs_values = resolver.ResolveValues(path);
    const std::string op_s = op;
    uint64_t rhs = 0;
    if (target_path) {
      if (op_s == "in" || op_s == "not_in") {
        throw std::runtime_error("Cmp targetPath does not support op: " + op_s);
      }
      const std::vector<uint64_t> rhs_values = resolver.ResolveValues(target_path);
      if (rhs_values.size() != 1) {
        throw std::runtime_error("Cmp targetPath must resolve to exactly one value: " + std::string(target_path));
      }
      rhs = rhs_values.front();
    } else if (op_s != "in" && op_s != "not_in") {
      rhs = ParseUInt(value, std::string("Cmp value at ") + path);
    }

    auto all_cmp = [&](auto fn) {
      for (uint64_t lhs : lhs_values) {
        if (!fn(lhs)) return false;
      }
      return true;
    };

    if (op_s == "eq") return all_cmp([&](uint64_t lhs) { return lhs == rhs; });
    if (op_s == "ne") return all_cmp([&](uint64_t lhs) { return lhs != rhs; });
    if (op_s == "gt") return all_cmp([&](uint64_t lhs) { return lhs > rhs; });
    if (op_s == "ge") return all_cmp([&](uint64_t lhs) { return lhs >= rhs; });
    if (op_s == "lt") return all_cmp([&](uint64_t lhs) { return lhs < rhs; });
    if (op_s == "le") return all_cmp([&](uint64_t lhs) { return lhs <= rhs; });

    if (op_s == "in" || op_s == "not_in") {
      std::unordered_set<uint64_t> allowed;
      std::stringstream ss(value);
      std::string token;
      while (std::getline(ss, token, ',')) {
        allowed.insert(ParseUInt(Trim(token).c_str(), std::string("Cmp list value at ") + path));
      }
      return all_cmp([&](uint64_t lhs) {
        const bool found = allowed.count(lhs) > 0;
        return op_s == "in" ? found : !found;
      });
    }
    throw std::runtime_error("Unsupported cmp op: " + op_s);
  }

  std::unordered_set<std::string> EvaluateCases(const XMLElement* rules) const {
    std::unordered_set<std::string> matched;
    const XMLElement* case_rules = rules->FirstChildElement("CaseRules");
    if (!case_rules) return matched;
    for (const XMLElement* c = case_rules->FirstChildElement("Case"); c; c = c->NextSiblingElement("Case")) {
      const char* id = c->Attribute("id");
      if (!id) throw std::runtime_error("Case missing id");
      const XMLElement* expr = FirstExprNode(c);
      if (!expr) throw std::runtime_error(std::string("Case missing expression: ") + id);
      if (EvalExpr(expr)) matched.insert(id);
    }
    return matched;
  }

  void EvaluateConditions(const XMLElement* rules, const std::unordered_set<std::string>& matched_cases,
                          std::vector<std::string>* failures) const {
    const XMLElement* cond_rules = rules->FirstChildElement("ConditionRules");
    if (!cond_rules) return;
    for (const XMLElement* r = cond_rules->FirstChildElement("Condition"); r;
         r = r->NextSiblingElement("Condition")) {
      const char* id = r->Attribute("id");
      const std::string rid = id ? id : "<no-id>";
      const char* case_ref = r->Attribute("caseRef");
      if (case_ref && matched_cases.count(case_ref) == 0) continue;
      const XMLElement* if_n = r->FirstChildElement("If");
      const XMLElement* then_n = r->FirstChildElement("Then");
      if (!if_n || !then_n) throw std::runtime_error("Condition missing If/Then: " + rid);
      const XMLElement* if_expr = FirstExprNode(if_n);
      const XMLElement* then_expr = FirstExprNode(then_n);
      if (!if_expr || !then_expr) throw std::runtime_error("If/Then missing expression: " + rid);
      if (EvalExpr(if_expr) && !EvalExpr(then_expr)) failures->push_back("Condition failed: " + rid);
    }
  }

  const XMLElement* dict_msg_;
  const XMLElement* msg_root_;
  std::string message_case_;
  std::unordered_map<std::string, std::string> aliases_;
};

class BuiltinSemanticValidator {
 public:
  BuiltinSemanticValidator(const XMLElement* dict_msg, const XMLElement* msg_root)
      : dict_msg_(dict_msg), msg_root_(msg_root) {}

  std::vector<std::string> Validate() const {
    std::vector<std::string> failures;
    const XMLElement* dict_header = dict_msg_->FirstChildElement("Header");
    const XMLElement* dict_body = dict_msg_->FirstChildElement("Body");
    const XMLElement* msg_header = msg_root_->FirstChildElement("Header");
    const XMLElement* msg_body = msg_root_->FirstChildElement("Body");
    if (!dict_header || !dict_body || !msg_header || !msg_body) {
      failures.push_back("Builtin semantic failed: Message root | missing Header/Body");
      return failures;
    }
    ValidateChildren(dict_header, msg_header, "Header", &failures);
    ValidateChildren(dict_body, msg_body, "Body", &failures);
    return failures;
  }

 private:
  void AddFailure(std::vector<std::string>* failures, const std::string& path, const std::string& reason) const {
    failures->push_back("Builtin semantic failed: " + path + " | " + reason);
  }

  size_t ParseRepeatableMax(const XMLElement* dict_node, const std::string& path, bool* has_max) const {
    const char* max_attr = dict_node->Attribute("max");
    if (!max_attr) {
      *has_max = false;
      return 0;
    }
    uint64_t max_v = 0;
    try {
      max_v = ParseUInt(max_attr, path + ".max");
    } catch (const std::exception&) {
      throw std::runtime_error("Dictionary config error: invalid max at " + path + ": " + max_attr);
    }
    if (max_v == 0 || max_v > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
      throw std::runtime_error("Dictionary config error: invalid max at " + path + ": " + max_attr);
    }
    *has_max = true;
    return static_cast<size_t>(max_v);
  }

  std::string InstancePath(const std::string& parent_path, const char* tag, const std::string& name,
                           size_t index) const {
    return parent_path + "." + tag + "(" + name + ")[" + std::to_string(index) + "]";
  }

  void ValidateRepeatSequence(const std::vector<const XMLElement*>& instances, const char* container_tag,
                              const std::string& name, const char* indicator_tag, const std::string& parent_path,
                              std::vector<std::string>* failures) const {
    for (size_t i = 0; i < instances.size(); ++i) {
      const std::string path_i = InstancePath(parent_path, container_tag, name, i);
      const XMLElement* ind = instances[i]->FirstChildElement(indicator_tag);
      const std::string ind_path = path_i + "." + indicator_tag;
      if (!ind) {
        AddFailure(failures, ind_path, std::string("missing repeat indicator ") + indicator_tag);
        continue;
      }
      uint64_t v = 0;
      try {
        v = ParseUInt(ind->GetText(), ind_path);
      } catch (const std::exception& e) {
        AddFailure(failures, ind_path, e.what());
        continue;
      }
      if (v != 0 && v != 1) {
        AddFailure(failures, ind_path, std::string(indicator_tag) + " must be 0 or 1");
        continue;
      }
      const bool is_last = (i + 1 == instances.size());
      if (!is_last && v != 1) {
        AddFailure(failures, ind_path, std::string(indicator_tag) + " must be 1 before the last instance");
      }
      if (is_last && v != 0) {
        AddFailure(failures, ind_path, std::string(indicator_tag) + " must be 0 at the last instance");
      }
    }
  }

  void ValidateContainer(const XMLElement* dict_container, const XMLElement* msg_container,
                         const std::string& container_path, const char* selectable_indicator_tag,
                         std::vector<std::string>* failures) const {
    if (!selectable_indicator_tag) {
      ValidateChildren(dict_container, msg_container, container_path, failures);
      return;
    }
    const XMLElement* ind = msg_container->FirstChildElement(selectable_indicator_tag);
    const std::string ind_path = container_path + "." + selectable_indicator_tag;
    if (!ind) {
      AddFailure(failures, ind_path, std::string("missing selectable indicator ") + selectable_indicator_tag);
      return;
    }
    uint64_t present = 0;
    try {
      present = ParseUInt(ind->GetText(), ind_path);
    } catch (const std::exception& e) {
      AddFailure(failures, ind_path, e.what());
      return;
    }
    if (present != 0 && present != 1) {
      AddFailure(failures, ind_path, std::string(selectable_indicator_tag) + " must be 0 or 1");
      return;
    }
    if (present == 0) {
      for (const XMLElement* child = msg_container->FirstChildElement(); child; child = child->NextSiblingElement()) {
        if (std::string(child->Name()) == selectable_indicator_tag) continue;
        AddFailure(failures, container_path,
                   std::string(selectable_indicator_tag) + "=0 but child node exists: " + child->Name());
        break;
      }
      return;
    }
    ValidateChildren(dict_container, msg_container, container_path, failures);
  }

  void ValidateChildren(const XMLElement* dict_parent, const XMLElement* msg_parent, const std::string& parent_path,
                        std::vector<std::string>* failures) const {
    for (const XMLElement* d = dict_parent->FirstChildElement(); d; d = d->NextSiblingElement()) {
      const std::string tag = d->Name();
      if (tag != "Group" && tag != "Field") continue;
      const char* name = d->Attribute("name");
      if (!name) throw std::runtime_error(tag + " missing name at " + parent_path);
      const bool repeatable = IsTrue(d->Attribute("repeatable"));
      const bool selectable = IsTrue(d->Attribute("selectable"));
      const char* match_tag = tag.c_str();
      auto matches = FindNamedChildren(msg_parent, match_tag, name);
      const std::string node_path = parent_path + "." + tag + "(" + name + ")";

      if (repeatable) {
        bool has_max = false;
        const size_t max_repeat = ParseRepeatableMax(d, node_path, &has_max);
        if (matches.empty()) {
          AddFailure(failures, node_path, "missing repeatable instances in message");
          continue;
        }
        if (has_max && matches.size() > max_repeat) {
          AddFailure(failures, node_path, "repeatable instances exceed max=" + std::to_string(max_repeat) +
                                           ", actual=" + std::to_string(matches.size()));
        }
        const char* repeat_ind = (tag == "Group") ? "GRI" : "FRI";
        ValidateRepeatSequence(matches, match_tag, name, repeat_ind, parent_path, failures);
        const char* selectable_ind = selectable ? ((tag == "Group") ? "GPI" : "FPI") : nullptr;
        for (size_t i = 0; i < matches.size(); ++i) {
          ValidateContainer(d, matches[i], InstancePath(parent_path, match_tag, name, i), selectable_ind, failures);
        }
        continue;
      }

      if (matches.empty()) {
        AddFailure(failures, node_path, "missing required container");
        continue;
      }
      if (matches.size() > 1) AddFailure(failures, node_path, "multiple non-repeatable containers found");
      const char* selectable_ind = selectable ? ((tag == "Group") ? "GPI" : "FPI") : nullptr;
      ValidateContainer(d, matches.front(), node_path, selectable_ind, failures);
    }
  }

  const XMLElement* dict_msg_;
  const XMLElement* msg_root_;
};

class ContentConstraintValidator {
 public:
  ContentConstraintValidator(const XMLElement* msg_root, const MessageDictionary* msg_dict,
                             const ContentDictionary* content_dict)
      : msg_root_(msg_root), msg_dict_(msg_dict), content_dict_(content_dict) {}

  std::vector<std::string> Validate() const {
    std::vector<std::string> failures;
    Walk(msg_root_, "MessageContent", &failures);
    return failures;
  }

 private:
  static bool MatchAllow(uint64_t value, const AllowConstraint& allow) {
    for (uint64_t v : allow.values) {
      if (v == value) return true;
    }
    for (const auto& r : allow.ranges) {
      if (value >= r.first && value <= r.second) return true;
    }
    return false;
  }

  void AddFailure(std::vector<std::string>* failures, const std::string& path, const std::string& reason) const {
    failures->push_back("Content constraint failed: " + path + " | " + reason);
  }

  void Walk(const XMLElement* msg_parent, const std::string& path, std::vector<std::string>* failures) const {
    for (const XMLElement* c = msg_parent->FirstChildElement(); c; c = c->NextSiblingElement()) {
      const std::string tag = c->Name();
      if (tag == "DataUnit") {
        ValidateDataUnit(c, path, failures);
      } else if (tag == "Group" || tag == "Field") {
        const char* name = c->Attribute("name");
        Walk(c, path + "." + tag + "(" + (name ? name : "<no-name>") + ")", failures);
      } else if (tag == "Header" || tag == "Body") {
        Walk(c, path + "." + tag, failures);
      }
    }
  }

  void ValidateDataUnit(const XMLElement* msg_du, const std::string& parent_path, std::vector<std::string>* failures) const {
    const char* dfi = msg_du->Attribute("DFI");
    const char* dui = msg_du->Attribute("DUI");
    const char* msg_name = msg_du->Attribute("name");
    const std::string path = parent_path + ".DataUnit(" + std::string(msg_name ? msg_name : "<no-name>") + ")";

    if (!dfi || !dui) {
      AddFailure(failures, path, "missing required DFI/DUI");
      return;
    }
    const DfiDuiKey key{dfi, dui};
    const MessageDataUnitDef* msg_def = msg_dict_->FindDataUnit(key);
    if (!msg_def) {
      AddFailure(failures, path, "DFI/DUI not found in dic_msg: DFI=" + key.dfi + " DUI=" + key.dui);
      return;
    }
    const ContentItem* content_item = content_dict_->Find(key);
    if (!content_item) {
      AddFailure(failures, path, "DFI/DUI not found in dic_content: DFI=" + key.dfi + " DUI=" + key.dui);
      return;
    }
    if (content_item->bits < 0 || content_item->bits != msg_def->bits) {
      AddFailure(failures, path, "bits mismatch for DFI=" + key.dfi + " DUI=" + key.dui);
      return;
    }
    if (msg_name && (msg_def->name != msg_name || content_item->name != msg_name)) {
      AddFailure(failures, path,
                 "name mismatch for DFI=" + key.dfi + " DUI=" + key.dui + " (msg=" + msg_name +
                     ", dic_msg=" + msg_def->name + ", dic_content=" + content_item->name + ")");
      return;
    }

    uint64_t value = 0;
    try {
      value = ParseUInt(msg_du->GetText(), path);
    } catch (const std::exception& e) {
      AddFailure(failures, path, e.what());
      return;
    }

    if (msg_def->bits < 64) {
      const uint64_t max = (uint64_t{1} << msg_def->bits) - 1;
      if (value > max) {
        AddFailure(failures, path, "value out of bits range for DFI=" + key.dfi + " DUI=" + key.dui);
        return;
      }
    }
    if (!content_item->allow.values.empty() || !content_item->allow.ranges.empty()) {
      if (!MatchAllow(value, content_item->allow)) {
        AddFailure(failures, path, "value " + std::to_string(value) + " not in AllowList for DFI=" + key.dfi +
                                     " DUI=" + key.dui);
      }
    }
  }

  const XMLElement* msg_root_;
  const MessageDictionary* msg_dict_;
  const ContentDictionary* content_dict_;
};

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 4) {
      std::cerr << "Usage: vmf_validate <msg_structure.xml> <dic_content.xml> <msg.xml>\n";
      return 1;
    }

    XMLDocument dict_msg_doc;
    if (dict_msg_doc.LoadFile(argv[1]) != tinyxml2::XML_SUCCESS) {
      std::cerr << "Failed to load dic_msg: " << argv[1] << "\n";
      return 1;
    }
    XMLDocument dict_content_doc;
    if (dict_content_doc.LoadFile(argv[2]) != tinyxml2::XML_SUCCESS) {
      std::cerr << "Failed to load dic_content: " << argv[2] << "\n";
      return 1;
    }
    XMLDocument msg_doc;
    if (msg_doc.LoadFile(argv[3]) != tinyxml2::XML_SUCCESS) {
      std::cerr << "Failed to load message xml: " << argv[3] << "\n";
      return 1;
    }

    const XMLElement* dict_msg = dict_msg_doc.FirstChildElement("Message");
    const XMLElement* dict_content = dict_content_doc.FirstChildElement("dic");
    const XMLElement* msg_root = msg_doc.FirstChildElement("MessageContent");
    if (!dict_msg || !dict_content || !msg_root) {
      std::cerr << "Invalid XML root. Expect <Message>, <dic> and <MessageContent>.\n";
      return 1;
    }

    ContentDictionary content_dict(dict_content);
    DictionaryConsistencyValidator dict_consistency(dict_msg, &content_dict);
    dict_consistency.ValidateOrThrow();
    MessageDictionary message_dict(dict_msg);

    RuleValidator rule_validator(dict_msg, msg_root);
    auto failures = rule_validator.Validate();
    BuiltinSemanticValidator builtin_validator(dict_msg, msg_root);
    auto builtin_failures = builtin_validator.Validate();
    failures.insert(failures.end(), builtin_failures.begin(), builtin_failures.end());
    ContentConstraintValidator content_validator(msg_root, &message_dict, &content_dict);
    auto content_failures = content_validator.Validate();
    failures.insert(failures.end(), content_failures.begin(), content_failures.end());

    if (failures.empty()) {
      std::cout << "Validation PASS\n";
      return 0;
    }
    std::cout << "Validation FAIL (" << failures.size() << ")\n";
    for (const auto& f : failures) std::cout << "- " << f << "\n";
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "Validation error: " << e.what() << "\n";
    return 3;
  }
}
