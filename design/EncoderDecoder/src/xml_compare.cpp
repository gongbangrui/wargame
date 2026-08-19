#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <tinyxml2.h>

using tinyxml2::XMLAttribute;
using tinyxml2::XMLDocument;
using tinyxml2::XMLElement;
using tinyxml2::XMLNode;
using tinyxml2::XMLText;

namespace {

// 表示一条 XML 差异记录。
struct DiffItem {
  std::string path;
  std::string type;
  std::string a_value;
  std::string b_value;
};

struct DiffSet {
  std::vector<DiffItem> failures;
  std::vector<DiffItem> warnings;
};

// 去除字符串两端空白字符。
std::string Trim(const std::string& s) {
  size_t left = 0;
  while (left < s.size() && std::isspace(static_cast<unsigned char>(s[left])) != 0) {
    ++left;
  }
  size_t right = s.size();
  while (right > left && std::isspace(static_cast<unsigned char>(s[right - 1])) != 0) {
    --right;
  }
  return s.substr(left, right - left);
}

bool IsHeaderPath(const std::string& path) {
  return path == "/MessageContent/Header[1]" || path.rfind("/MessageContent/Header[1]/", 0) == 0;
}

bool IsRootCaseAttrDiff(const std::string& path, const std::string& type,
                        const std::string& a_value, const std::string& b_value) {
  if (path != "/MessageContent" && path != "/MessageContent/@case") return false;
  if (type == "attr_value_mismatch" && path == "/MessageContent/@case") return true;
  if (type == "attr_missing_in_a") return b_value.rfind("case=", 0) == 0;
  if (type == "attr_missing_in_b") return a_value.rfind("case=", 0) == 0;
  return false;
}

// 向差异列表追加一条差异项；非编码稳定字段差异降级为 warning。
void AddDiff(DiffSet* diffs, const std::string& path, const std::string& type,
             const std::string& a_value, const std::string& b_value) {
  DiffItem item{path, type, a_value, b_value};
  if (IsHeaderPath(path) || IsRootCaseAttrDiff(path, type, a_value, b_value)) {
    diffs->warnings.push_back(item);
  } else {
    diffs->failures.push_back(item);
  }
}

// 读取元素的全部属性为键值映射。
std::unordered_map<std::string, std::string> ReadAttributes(const XMLElement* elem) {
  std::unordered_map<std::string, std::string> attrs;
  for (const XMLAttribute* a = elem->FirstAttribute(); a; a = a->Next()) {
    attrs[a->Name()] = a->Value() ? a->Value() : "";
  }
  return attrs;
}

// 收集元素的直接子元素列表（保持文档顺序）。
std::vector<const XMLElement*> ElementChildren(const XMLElement* elem) {
  std::vector<const XMLElement*> out;
  for (const XMLElement* c = elem->FirstChildElement(); c; c = c->NextSiblingElement()) {
    out.push_back(c);
  }
  return out;
}

// 提取元素的直接文本内容并合并连续文本片段。
std::string ElementDirectText(const XMLElement* elem) {
  std::string merged;
  for (const XMLNode* n = elem->FirstChild(); n; n = n->NextSibling()) {
    const XMLText* t = n->ToText();
    if (!t || !t->Value()) continue;
    std::string part = Trim(t->Value());
    if (part.empty()) continue;
    if (!merged.empty()) merged.push_back(' ');
    merged += part;
  }
  return merged;
}

// 基于父路径与同层序号生成子节点路径。
std::string ChildPath(const std::string& parent_path, const XMLElement* child, int ordinal_1based) {
  std::ostringstream oss;
  oss << parent_path << "/" << child->Name() << "[" << ordinal_1based << "]";
  return oss.str();
}

void CompareDataUnit(const XMLElement* a, const XMLElement* b, const std::string& path, DiffSet* diffs) {
  for (const char* attr : {"DFI", "DUI"}) {
    const char* va = a->Attribute(attr);
    const char* vb = b->Attribute(attr);
    const std::string sa = va ? va : "";
    const std::string sb = vb ? vb : "";
    if (!va) {
      AddDiff(diffs, path, "attr_missing_in_a", "", std::string(attr));
    } else if (!vb) {
      AddDiff(diffs, path, "attr_missing_in_b", std::string(attr) + "=" + sa, "");
    } else if (sa != sb) {
      AddDiff(diffs, path + "/@" + attr, "attr_value_mismatch", sa, sb);
    }
  }

  const std::string text_a = ElementDirectText(a);
  const std::string text_b = ElementDirectText(b);
  if (text_a != text_b) {
    AddDiff(diffs, path, "text_mismatch", text_a, text_b);
  }
}

// 递归比较两个元素的标签、属性、文本和子节点顺序差异。
void CompareElements(const XMLElement* a, const XMLElement* b, const std::string& path, DiffSet* diffs) {
  if (std::string(a->Name()) != std::string(b->Name())) {
    AddDiff(diffs, path, "tag_mismatch", a->Name(), b->Name());
    return;
  }

  const bool is_data_unit = std::string(a->Name()) == "DataUnit";
  if (is_data_unit) {
    CompareDataUnit(a, b, path, diffs);
  } else {
    const auto attrs_a = ReadAttributes(a);
    const auto attrs_b = ReadAttributes(b);
    for (const auto& [k, va] : attrs_a) {
      auto it = attrs_b.find(k);
      if (it == attrs_b.end()) {
        AddDiff(diffs, path, "attr_missing_in_b", k + "=" + va, "");
        continue;
      }
      if (it->second != va) {
        AddDiff(diffs, path + "/@" + k, "attr_value_mismatch", va, it->second);
      }
    }
    for (const auto& [k, vb] : attrs_b) {
      if (attrs_a.find(k) == attrs_a.end()) {
        AddDiff(diffs, path, "attr_missing_in_a", "", k + "=" + vb);
      }
    }

    const std::string text_a = ElementDirectText(a);
    const std::string text_b = ElementDirectText(b);
    if (text_a != text_b) {
      AddDiff(diffs, path, "text_mismatch", text_a, text_b);
    }
  }

  const auto children_a = ElementChildren(a);
  const auto children_b = ElementChildren(b);
  if (children_a.size() != children_b.size()) {
    AddDiff(diffs, path, "child_count_mismatch", std::to_string(children_a.size()),
            std::to_string(children_b.size()));
  }

  const size_t common = std::min(children_a.size(), children_b.size());
  for (size_t i = 0; i < common; ++i) {
    const std::string child_path = ChildPath(path, children_a[i], static_cast<int>(i + 1));
    CompareElements(children_a[i], children_b[i], child_path, diffs);
  }

  for (size_t i = common; i < children_a.size(); ++i) {
    const std::string extra_path = ChildPath(path, children_a[i], static_cast<int>(i + 1));
    AddDiff(diffs, extra_path, "node_missing_in_b", children_a[i]->Name(), "");
  }
  for (size_t i = common; i < children_b.size(); ++i) {
    const std::string extra_path = ChildPath(path, children_b[i], static_cast<int>(i + 1));
    AddDiff(diffs, extra_path, "node_missing_in_a", "", children_b[i]->Name());
  }
}

void PrintDiffs(const char* label, const std::vector<DiffItem>& diffs) {
  if (diffs.empty()) return;
  std::cout << label << " (" << diffs.size() << ")\n";
  for (const auto& d : diffs) {
    std::cout << d.path << " | " << d.type << " | " << d.a_value << " | " << d.b_value << "\n";
  }
}

}  // namespace

// 程序入口：读取两个 XML 文件并输出严格差异结果。
int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: xml_compare <a.xml> <b.xml>\n";
    return 1;
  }

  XMLDocument a_doc;
  if (a_doc.LoadFile(argv[1]) != tinyxml2::XML_SUCCESS) {
    std::cerr << "Failed to load: " << argv[1] << "\n";
    return 3;
  }
  XMLDocument b_doc;
  if (b_doc.LoadFile(argv[2]) != tinyxml2::XML_SUCCESS) {
    std::cerr << "Failed to load: " << argv[2] << "\n";
    return 3;
  }

  const XMLElement* a_root = a_doc.RootElement();
  const XMLElement* b_root = b_doc.RootElement();
  if (!a_root || !b_root) {
    std::cerr << "Invalid XML: missing root element\n";
    return 3;
  }

  DiffSet diffs;
  const std::string root_path = std::string("/") + a_root->Name();
  CompareElements(a_root, b_root, root_path, &diffs);

  if (diffs.failures.empty() && diffs.warnings.empty()) {
    std::cout << "XML_COMPARE_PASS\n";
    return 0;
  }

  if (diffs.failures.empty()) {
    std::cout << "XML_COMPARE_PASS_WITH_WARNINGS (" << diffs.warnings.size() << ")\n";
    PrintDiffs("WARNINGS", diffs.warnings);
    return 0;
  }

  std::cout << "XML_COMPARE_FAIL (" << diffs.failures.size() << ")\n";
  PrintDiffs("FAILURES", diffs.failures);
  PrintDiffs("WARNINGS", diffs.warnings);
  return 2;
}
