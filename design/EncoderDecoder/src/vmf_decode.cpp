#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

struct ContentItem {
  std::string name;
  int bits = -1;
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
  static std::string Trim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), s.end());
    return s;
  }

  static uint64_t ParseUInt(const char* text, const std::string& path) {
    if (!text) throw std::runtime_error("Missing numeric text at " + path);
    const std::string s = Trim(text);
    size_t idx = 0;
    const unsigned long long v = std::stoull(s, &idx, 0);
    if (idx != s.size()) throw std::runtime_error("Invalid numeric text at " + path + ": " + s);
    return static_cast<uint64_t>(v);
  }

  static int ParseBits(const XMLElement* elem, const std::string& ctx) {
    const char* bits = elem->Attribute("bits");
    if (!bits) throw std::runtime_error("Dictionary config error: missing bits at " + ctx);
    const uint64_t v = ParseUInt(bits, ctx + ".bits");
    if (v == 0 || v > 64) throw std::runtime_error("Dictionary config error: invalid bits at " + ctx + ": " + bits);
    return static_cast<int>(v);
  }

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
        items_.emplace(key, std::move(item));
      }
    }
  }

  std::unordered_map<DfiDuiKey, ContentItem, DfiDuiKeyHash> items_;
};

class BitReader {
 public:
  explicit BitReader(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

  uint64_t ReadBits(int bits, const std::string& path) {
    if (bits <= 0 || bits > 64) throw std::runtime_error("Invalid bits at " + path + ": " + std::to_string(bits));
    if (bit_pos_ + static_cast<size_t>(bits) > bytes_.size() * 8ULL) {
      throw std::runtime_error("Bitstream out of range at " + path);
    }
    uint64_t value = 0;
    for (int i = 0; i < bits; ++i) {
      const size_t pos = bit_pos_ + static_cast<size_t>(i);
      const size_t byte_idx = pos / 8;
      const int bit_idx = static_cast<int>(7 - (pos % 8));
      const uint64_t bit = (bytes_[byte_idx] >> bit_idx) & 1U;
      value = (value << 1) | bit;
    }
    bit_pos_ += static_cast<size_t>(bits);
    return value;
  }

  size_t BitsRead() const { return bit_pos_; }
  size_t TotalBits() const { return bytes_.size() * 8ULL; }

 private:
  std::vector<uint8_t> bytes_;
  size_t bit_pos_ = 0;
};

bool IsTrue(const char* value) {
  if (!value) return false;
  std::string s(value);
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s == "true" || s == "1";
}

int RequireBitsAttr(const XMLElement* elem, const std::string& path) {
  const char* bits = elem->Attribute("bits");
  if (!bits) throw std::runtime_error("Missing bits attribute at " + path);
  const int b = std::atoi(bits);
  if (b <= 0 || b > 64) throw std::runtime_error("Invalid bits attribute at " + path + ": " + bits);
  return b;
}

std::string RequireAttr(const XMLElement* elem, const char* attr, const std::string& path) {
  const char* value = elem->Attribute(attr);
  if (!value) throw std::runtime_error("Missing attribute " + std::string(attr) + " at " + path);
  return value;
}

void SetTextUInt(XMLDocument* doc, XMLElement* elem, uint64_t v) {
  elem->SetText(std::to_string(v).c_str());
  (void)doc;
}

class Decoder {
 public:
  explicit Decoder(const ContentDictionary* content_dict) : content_dict_(content_dict) {}

  void Decode(const XMLElement* dict_message, BitReader* reader, XMLDocument* out_doc) {
    const XMLElement* dict_header = dict_message->FirstChildElement("Header");
    const XMLElement* dict_body = dict_message->FirstChildElement("Body");
    if (!dict_header || !dict_body) throw std::runtime_error("Dictionary missing Header/Body");

    XMLElement* root = out_doc->NewElement("MessageContent");
    if (const char* msg_name = dict_message->Attribute("name")) root->SetAttribute("message", msg_name);
    out_doc->InsertEndChild(root);

    XMLElement* header = out_doc->NewElement("Header");
    root->InsertEndChild(header);
    for (const XMLElement* f = dict_header->FirstChildElement("Field"); f; f = f->NextSiblingElement("Field")) {
      const char* name = f->Attribute("name");
      if (!name) throw std::runtime_error("Header Field missing name");
      const int bits = RequireBitsAttr(f, std::string("Header.Field(") + name + ")");
      const uint64_t value = reader->ReadBits(bits, std::string("Header.Field(") + name + ")");
      XMLElement* out_f = out_doc->NewElement("Field");
      out_f->SetAttribute("name", name);
      SetTextUInt(out_doc, out_f, value);
      header->InsertEndChild(out_f);
    }

    XMLElement* body = out_doc->NewElement("Body");
    root->InsertEndChild(body);
    DecodeChildren(dict_body, body, reader, out_doc, "Body");
  }

 private:
  const ContentDictionary* content_dict_;

  static const XMLElement* FindChild(const XMLElement* parent, const char* tag) {
    for (const XMLElement* c = parent->FirstChildElement(); c; c = c->NextSiblingElement()) {
      if (std::string(c->Name()) == tag) return c;
    }
    return nullptr;
  }

  void SetKeyAttrs(XMLElement* out, const XMLElement* dict_elem, const std::string& path) {
    out->SetAttribute("DFI", RequireAttr(dict_elem, "DFI", path).c_str());
    out->SetAttribute("DUI", RequireAttr(dict_elem, "DUI", path).c_str());
  }

  void DecodeChildren(const XMLElement* dict_parent, XMLElement* out_parent, BitReader* reader, XMLDocument* out_doc,
                      const std::string& path) {
    for (const XMLElement* d = dict_parent->FirstChildElement(); d; d = d->NextSiblingElement()) {
      const std::string tag = d->Name();
      if (tag == "DataUnit") {
        DecodeDataUnit(d, out_parent, reader, out_doc, path);
      } else if (tag == "Group") {
        DecodeGroup(d, out_parent, reader, out_doc, path);
      } else if (tag == "Field") {
        DecodeField(d, out_parent, reader, out_doc, path);
      } else if (tag == "GRI" || tag == "GPI" || tag == "FRI" || tag == "FPI") {
        DecodeIndicator(d, out_parent, reader, out_doc, path + "." + tag);
      } else {
        throw std::runtime_error("Unknown dictionary element at " + path + ": " + tag);
      }
    }
  }

  uint64_t DecodeIndicator(const XMLElement* dict_ind, XMLElement* out_parent, BitReader* reader, XMLDocument* out_doc,
                           const std::string& path) {
    const std::string tag = dict_ind->Name();
    const int bits = RequireBitsAttr(dict_ind, path);
    const uint64_t v = reader->ReadBits(bits, path);
    XMLElement* out = out_doc->NewElement(tag.c_str());
    SetKeyAttrs(out, dict_ind, path);
    SetTextUInt(out_doc, out, v);
    out_parent->InsertEndChild(out);
    return v;
  }

  void DecodeDataUnit(const XMLElement* dict_du, XMLElement* out_parent, BitReader* reader, XMLDocument* out_doc,
                      const std::string& path) {
    const DfiDuiKey key{RequireAttr(dict_du, "DFI", path + ".DataUnit"), RequireAttr(dict_du, "DUI", path + ".DataUnit")};
    const int bits = RequireBitsAttr(dict_du, path + ".DataUnit(" + RequireAttr(dict_du, "name", path) + ")");
    const ContentItem* item = content_dict_->Find(key);
    if (!item) {
      throw std::runtime_error("Dictionary config error: missing DFI/DUI in dic_content for decode: DFI=" + key.dfi +
                               " DUI=" + key.dui);
    }
    if (item->bits != bits) {
      throw std::runtime_error("Dictionary config error: bits mismatch during decode for DFI=" + key.dfi +
                               " DUI=" + key.dui);
    }
    const uint64_t v = reader->ReadBits(bits, path + ".DataUnit(" + item->name + ")");
    XMLElement* out = out_doc->NewElement("DataUnit");
    out->SetAttribute("DFI", key.dfi.c_str());
    out->SetAttribute("DUI", key.dui.c_str());
    out->SetAttribute("name", item->name.c_str());
    SetTextUInt(out_doc, out, v);
    out_parent->InsertEndChild(out);
  }

  void DecodeGroup(const XMLElement* dict_group, XMLElement* out_parent, BitReader* reader, XMLDocument* out_doc,
                   const std::string& path) {
    const char* name = dict_group->Attribute("name");
    if (!name) throw std::runtime_error("Group missing name at " + path);
    const bool repeatable = IsTrue(dict_group->Attribute("repeatable"));
    const bool selectable = IsTrue(dict_group->Attribute("selectable"));
    const std::string sub_path = path + ".Group(" + name + ")";

    if (repeatable) {
      while (true) {
        XMLElement* out_group = out_doc->NewElement("Group");
        out_group->SetAttribute("name", name);
        out_parent->InsertEndChild(out_group);
        const uint64_t repeat_flag =
            DecodeContainer(dict_group, out_group, reader, out_doc, sub_path, selectable ? "GPI" : nullptr, "GRI");
        if (repeat_flag == 0) break;
      }
      return;
    }

    XMLElement* out_group = out_doc->NewElement("Group");
    out_group->SetAttribute("name", name);
    out_parent->InsertEndChild(out_group);
    (void)DecodeContainer(dict_group, out_group, reader, out_doc, sub_path, selectable ? "GPI" : nullptr, nullptr);
  }

  void DecodeField(const XMLElement* dict_field, XMLElement* out_parent, BitReader* reader, XMLDocument* out_doc,
                   const std::string& path) {
    const char* name = dict_field->Attribute("name");
    if (!name) throw std::runtime_error("Field missing name at " + path);
    const bool repeatable = IsTrue(dict_field->Attribute("repeatable"));
    const bool selectable = IsTrue(dict_field->Attribute("selectable"));
    const std::string sub_path = path + ".Field(" + name + ")";

    if (repeatable) {
      while (true) {
        XMLElement* out_field = out_doc->NewElement("Field");
        out_field->SetAttribute("name", name);
        out_parent->InsertEndChild(out_field);
        const uint64_t repeat_flag =
            DecodeContainer(dict_field, out_field, reader, out_doc, sub_path, selectable ? "FPI" : nullptr, "FRI");
        if (repeat_flag == 0) break;
      }
      return;
    }

    XMLElement* out_field = out_doc->NewElement("Field");
    out_field->SetAttribute("name", name);
    out_parent->InsertEndChild(out_field);
    (void)DecodeContainer(dict_field, out_field, reader, out_doc, sub_path, selectable ? "FPI" : nullptr, nullptr);
  }

  uint64_t DecodeContainer(const XMLElement* dict_container, XMLElement* out_container, BitReader* reader,
                           XMLDocument* out_doc, const std::string& path, const char* selectable_indicator_tag,
                           const char* repeat_indicator_tag) {
    const XMLElement* selectable_dict = nullptr;
    if (selectable_indicator_tag) {
      selectable_dict = FindChild(dict_container, selectable_indicator_tag);
      if (!selectable_dict) {
        throw std::runtime_error("Selectable container missing indicator in dictionary at " + path);
      }
    }

    uint64_t present = 1;
    if (selectable_dict) {
      present = DecodeIndicator(selectable_dict, out_container, reader, out_doc,
                                path + "." + std::string(selectable_indicator_tag));
    }

    uint64_t repeat_flag = 0;
    if (present != 0) {
      for (const XMLElement* d = dict_container->FirstChildElement(); d; d = d->NextSiblingElement()) {
        if (d == selectable_dict) continue;
        const std::string tag = d->Name();
        if (tag == "DataUnit") {
          DecodeDataUnit(d, out_container, reader, out_doc, path);
        } else if (tag == "Group") {
          DecodeGroup(d, out_container, reader, out_doc, path);
        } else if (tag == "Field") {
          DecodeField(d, out_container, reader, out_doc, path);
        } else if (tag == "GRI" || tag == "GPI" || tag == "FRI" || tag == "FPI") {
          const uint64_t v = DecodeIndicator(d, out_container, reader, out_doc, path + "." + tag);
          if (repeat_indicator_tag && tag == repeat_indicator_tag) repeat_flag = v;
        } else {
          throw std::runtime_error("Unknown dictionary element at " + path + ": " + tag);
        }
      }
    }
    return repeat_flag;
  }
};

std::vector<uint8_t> ReadAllBytes(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) throw std::runtime_error("Failed to open input: " + path);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 5) {
      std::cerr << "Usage: vmf_decode <msg_structure.xml> <dic_content.xml> <in.bin> <out.xml>\n";
      return 1;
    }

    XMLDocument dict_doc;
    if (dict_doc.LoadFile(argv[1]) != tinyxml2::XML_SUCCESS) {
      std::cerr << "Failed to load dictionary: " << argv[1] << "\n";
      return 1;
    }
    XMLDocument content_doc;
    if (content_doc.LoadFile(argv[2]) != tinyxml2::XML_SUCCESS) {
      std::cerr << "Failed to load content dictionary: " << argv[2] << "\n";
      return 1;
    }
    const XMLElement* dict_message = dict_doc.FirstChildElement("Message");
    const XMLElement* content_root = content_doc.FirstChildElement("dic");
    if (!dict_message || !content_root) {
      std::cerr << "Invalid dictionary root, expect <Message> and <dic>\n";
      return 1;
    }

    std::vector<uint8_t> input = ReadAllBytes(argv[3]);
    BitReader reader(std::move(input));
    ContentDictionary content_dict(content_root);
    XMLDocument out_doc;
    Decoder decoder(&content_dict);
    decoder.Decode(dict_message, &reader, &out_doc);

    if (out_doc.SaveFile(argv[4]) != tinyxml2::XML_SUCCESS) {
      std::cerr << "Failed to save output XML: " << argv[4] << "\n";
      return 1;
    }

    std::cout << "Decoded bits: " << reader.BitsRead() << " / " << reader.TotalBits() << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Decode error: " << e.what() << "\n";
    return 2;
  }
}
