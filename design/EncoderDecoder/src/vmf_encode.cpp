#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

class BitWriter {
 public:
  void WriteBits(uint64_t value, int bits, const std::string& path) {
    if (bits <= 0 || bits > 64) throw std::runtime_error("Invalid bits at " + path + ": " + std::to_string(bits));
    if (bits < 64) {
      const uint64_t max = (uint64_t{1} << bits) - 1;
      if (value > max) {
        throw std::runtime_error("Value out of range at " + path + ": " + std::to_string(value) +
                                 " exceeds " + std::to_string(max));
      }
    }
    for (int i = bits - 1; i >= 0; --i) {
      const int bit = static_cast<int>((value >> i) & 1U);
      const size_t byte_index = bit_pos_ / 8;
      const int bit_in_byte = static_cast<int>(7 - (bit_pos_ % 8));
      if (byte_index >= bytes_.size()) bytes_.push_back(0);
      bytes_[byte_index] = static_cast<uint8_t>(bytes_[byte_index] | (bit << bit_in_byte));
      ++bit_pos_;
    }
  }

  void SetBits(size_t start_bit, uint64_t value, int bits) {
    if (start_bit + static_cast<size_t>(bits) > bit_pos_) throw std::runtime_error("SetBits out of written range");
    if (bits <= 0 || bits > 64) throw std::runtime_error("SetBits invalid width");
    if (bits < 64) {
      const uint64_t max = (uint64_t{1} << bits) - 1;
      if (value > max) throw std::runtime_error("SetBits value out of range");
    }
    for (int i = 0; i < bits; ++i) {
      const int bit = static_cast<int>((value >> (bits - 1 - i)) & 1U);
      const size_t pos = start_bit + static_cast<size_t>(i);
      const size_t byte_index = pos / 8;
      const int bit_in_byte = static_cast<int>(7 - (pos % 8));
      if (bit == 1) {
        bytes_[byte_index] = static_cast<uint8_t>(bytes_[byte_index] | (1U << bit_in_byte));
      } else {
        bytes_[byte_index] = static_cast<uint8_t>(bytes_[byte_index] & ~(1U << bit_in_byte));
      }
    }
  }

  size_t BitSize() const { return bit_pos_; }
  const std::vector<uint8_t>& Bytes() const { return bytes_; }

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

std::string Trim(std::string s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c) { return !std::isspace(c); }));
  s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), s.end());
  return s;
}

uint64_t ParseUInt(const char* text, const std::string& path) {
  if (!text) throw std::runtime_error("Missing numeric text at " + path);
  const std::string s = Trim(text);
  if (s.empty()) throw std::runtime_error("Empty numeric text at " + path);
  size_t idx = 0;
  const unsigned long long v = std::stoull(s, &idx, 0);
  if (idx != s.size()) throw std::runtime_error("Invalid numeric text at " + path + ": " + s);
  return static_cast<uint64_t>(v);
}

int RequireBitsAttr(const XMLElement* elem, const std::string& path) {
  const char* bits = elem->Attribute("bits");
  if (!bits) throw std::runtime_error("Missing bits attribute at " + path);
  const int bits_i = std::atoi(bits);
  if (bits_i <= 0 || bits_i > 64) throw std::runtime_error("Invalid bits attribute at " + path + ": " + bits);
  return bits_i;
}

std::string RequireAttr(const XMLElement* elem, const char* attr, const std::string& path) {
  const char* value = elem->Attribute(attr);
  if (!value) throw std::runtime_error("Missing attribute " + std::string(attr) + " at " + path);
  return value;
}

class Encoder {
 public:
  explicit Encoder(const ContentDictionary* content_dict) : content_dict_(content_dict) {}

  void Encode(const XMLElement* dict_message, const XMLElement* msg_root, BitWriter* writer) {
    const XMLElement* dict_header = dict_message->FirstChildElement("Header");
    const XMLElement* dict_body = dict_message->FirstChildElement("Body");
    const XMLElement* msg_header = msg_root->FirstChildElement("Header");
    const XMLElement* msg_body = msg_root->FirstChildElement("Body");
    if (!dict_header || !dict_body || !msg_header || !msg_body) {
      throw std::runtime_error("Missing Header/Body in dictionary or message");
    }

    size_t length_offset = 0;
    bool has_length = false;
    int length_bits = 0;
    for (const XMLElement* field = dict_header->FirstChildElement("Field"); field;
         field = field->NextSiblingElement("Field")) {
      const char* name = field->Attribute("name");
      if (!name) throw std::runtime_error("Header Field missing name");
      const int bits = RequireBitsAttr(field, std::string("Header.Field(") + name + ")");
      const XMLElement* msg_field = TakeOne(msg_header, "Field", name, "Header.Field");
      const uint64_t value = ParseUInt(msg_field->GetText(), std::string("Header.Field(") + name + ")");
      if (std::string(name) == "length") {
        has_length = true;
        length_bits = bits;
        length_offset = writer->BitSize();
        writer->WriteBits(0, bits, "Header.Field(length)");
      } else {
        writer->WriteBits(value, bits, std::string("Header.Field(") + name + ")");
      }
    }
    EnsureNoUnexpected(msg_header, "Header");

    EncodeChildren(dict_body, msg_body, writer, "Body");
    EnsureNoUnexpected(msg_body, "Body");

    if (has_length) {
      const uint64_t total_bytes = static_cast<uint64_t>((writer->BitSize() + 7) / 8);
      writer->SetBits(length_offset, total_bytes, length_bits);
    }
  }

 private:
  using ConsumedSet = std::unordered_set<const XMLElement*>;
  std::unordered_map<const XMLElement*, ConsumedSet> consumed_;
  const ContentDictionary* content_dict_;

  const XMLElement* TakeOne(const XMLElement* msg_parent, const char* tag, const char* name, const std::string& path) {
    for (const XMLElement* child = msg_parent->FirstChildElement(); child; child = child->NextSiblingElement()) {
      if (consumed_[msg_parent].count(child) > 0) continue;
      if (std::string(child->Name()) != tag) continue;
      if (name) {
        const char* n = child->Attribute("name");
        if (!n || std::string(n) != name) continue;
      }
      consumed_[msg_parent].insert(child);
      return child;
    }
    throw std::runtime_error("Missing message element at " + path + ": tag=" + tag +
                             (name ? (", name=" + std::string(name)) : ""));
  }

  std::vector<const XMLElement*> TakeAll(const XMLElement* msg_parent, const char* tag, const char* name) {
    std::vector<const XMLElement*> out;
    for (const XMLElement* child = msg_parent->FirstChildElement(); child; child = child->NextSiblingElement()) {
      if (consumed_[msg_parent].count(child) > 0) continue;
      if (std::string(child->Name()) != tag) continue;
      const char* n = child->Attribute("name");
      if (!n || std::string(n) != name) continue;
      consumed_[msg_parent].insert(child);
      out.push_back(child);
    }
    return out;
  }

  const XMLElement* TakeOneDataUnitByKey(const XMLElement* msg_parent, const DfiDuiKey& key, const std::string& path) {
    const XMLElement* found = nullptr;
    for (const XMLElement* child = msg_parent->FirstChildElement("DataUnit"); child; child = child->NextSiblingElement("DataUnit")) {
      if (consumed_[msg_parent].count(child) > 0) continue;
      const char* dfi = child->Attribute("DFI");
      const char* dui = child->Attribute("DUI");
      if (!dfi || !dui) continue;
      if (key.dfi != dfi || key.dui != dui) continue;
      if (found) {
        throw std::runtime_error("Duplicate message DataUnit for DFI=" + key.dfi + " DUI=" + key.dui + " at " + path);
      }
      found = child;
    }
    if (!found) {
      throw std::runtime_error("Missing message DataUnit for DFI=" + key.dfi + " DUI=" + key.dui + " at " + path);
    }
    consumed_[msg_parent].insert(found);
    return found;
  }

  void EnsureNoUnexpected(const XMLElement* msg_parent, const std::string& path) {
    for (const XMLElement* child = msg_parent->FirstChildElement(); child; child = child->NextSiblingElement()) {
      if (consumed_[msg_parent].count(child) == 0) {
        const char* n = child->Attribute("name");
        throw std::runtime_error("Unexpected element at " + path + ": " + child->Name() +
                                 (n ? (std::string("(") + n + ")") : ""));
      }
    }
  }

  void ValidateIndicatorKey(const XMLElement* dict_ind, const XMLElement* msg_ind, const std::string& path) {
    const char* msg_dfi = msg_ind->Attribute("DFI");
    const char* msg_dui = msg_ind->Attribute("DUI");
    if (!msg_dfi && !msg_dui) return;
    if (!msg_dfi || !msg_dui) throw std::runtime_error("Indicator must provide both DFI and DUI at " + path);
    const std::string dict_dfi = RequireAttr(dict_ind, "DFI", path);
    const std::string dict_dui = RequireAttr(dict_ind, "DUI", path);
    if (dict_dfi != msg_dfi || dict_dui != msg_dui) {
      throw std::runtime_error("Indicator DFI/DUI mismatch at " + path + " (msg=" + std::string(msg_dfi) + "/" +
                               std::string(msg_dui) + ", dic_msg=" + dict_dfi + "/" + dict_dui + ")");
    }
  }

  void ValidateDataUnitName(const XMLElement* msg_du, const DfiDuiKey& key, const std::string& dict_name,
                            const std::string& path) {
    const ContentItem* content_item = content_dict_->Find(key);
    if (!content_item) {
      throw std::runtime_error("DFI/DUI not found in dic_content at " + path + ": DFI=" + key.dfi +
                               " DUI=" + key.dui);
    }
    if (content_item->bits < 0) {
      throw std::runtime_error("Missing bits in dic_content at " + path + ": DFI=" + key.dfi + " DUI=" + key.dui);
    }
    const char* msg_name = msg_du->Attribute("name");
    if (!msg_name) return;
    if (dict_name != msg_name || content_item->name != msg_name) {
      throw std::runtime_error("DataUnit name mismatch at " + path + " for DFI=" + key.dfi + " DUI=" + key.dui +
                               " (msg=" + std::string(msg_name) + ", dic_msg=" + dict_name +
                               ", dic_content=" + content_item->name + ")");
    }
  }

  void EncodeChildren(const XMLElement* dict_parent, const XMLElement* msg_parent, BitWriter* writer,
                      const std::string& path) {
    for (const XMLElement* d = dict_parent->FirstChildElement(); d; d = d->NextSiblingElement()) {
      const std::string tag = d->Name();
      if (tag == "DataUnit") {
        EncodeDataUnit(d, msg_parent, writer, path);
      } else if (tag == "Group") {
        EncodeGroup(d, msg_parent, writer, path);
      } else if (tag == "Field") {
        EncodeField(d, msg_parent, writer, path);
      } else if (tag == "GRI" || tag == "GPI" || tag == "FRI" || tag == "FPI") {
        EncodeIndicator(d, msg_parent, writer, path + "." + tag);
      } else {
        throw std::runtime_error("Unknown dictionary element at " + path + ": " + tag);
      }
    }
  }

  void EncodeIndicator(const XMLElement* dict_ind, const XMLElement* msg_parent, BitWriter* writer,
                       const std::string& path) {
    const int bits = RequireBitsAttr(dict_ind, path);
    const std::string tag = dict_ind->Name();
    const XMLElement* msg_ind = TakeOne(msg_parent, tag.c_str(), nullptr, path);
    ValidateIndicatorKey(dict_ind, msg_ind, path);
    writer->WriteBits(ParseUInt(msg_ind->GetText(), path), bits, path);
  }

  void EncodeDataUnit(const XMLElement* dict_du, const XMLElement* msg_parent, BitWriter* writer,
                      const std::string& path) {
    const std::string name = RequireAttr(dict_du, "name", path + ".DataUnit");
    const DfiDuiKey key{RequireAttr(dict_du, "DFI", path + ".DataUnit(" + name + ")"),
                        RequireAttr(dict_du, "DUI", path + ".DataUnit(" + name + ")")};
    const int bits = RequireBitsAttr(dict_du, path + ".DataUnit(" + name + ")");
    const ContentItem* content_item = content_dict_->Find(key);
    if (!content_item) {
      throw std::runtime_error("DFI/DUI not found in dic_content at " + path + ".DataUnit(" + name + ")");
    }
    if (content_item->bits != bits) {
      throw std::runtime_error("bits mismatch between dic_msg and dic_content at " + path + ".DataUnit(" + name + ")");
    }
    const XMLElement* msg_du = TakeOneDataUnitByKey(msg_parent, key, path + ".DataUnit(" + name + ")");
    ValidateDataUnitName(msg_du, key, name, path + ".DataUnit(" + name + ")");
    writer->WriteBits(ParseUInt(msg_du->GetText(), path + ".DataUnit(" + name + ")"), bits,
                      path + ".DataUnit(" + name + ")");
  }

  void EncodeGroup(const XMLElement* dict_group, const XMLElement* msg_parent, BitWriter* writer,
                   const std::string& path) {
    const char* name = dict_group->Attribute("name");
    if (!name) throw std::runtime_error("Group missing name at " + path);
    const bool repeatable = IsTrue(dict_group->Attribute("repeatable"));
    const bool selectable = IsTrue(dict_group->Attribute("selectable"));
    const std::string sub_path = path + ".Group(" + name + ")";
    if (repeatable) {
      auto groups = TakeAll(msg_parent, "Group", name);
      if (groups.empty()) throw std::runtime_error("Missing repeatable group instances at " + sub_path);
      for (const XMLElement* g : groups) EncodeContainer(dict_group, g, writer, sub_path, selectable ? "GPI" : nullptr);
    } else {
      const XMLElement* g = TakeOne(msg_parent, "Group", name, sub_path);
      EncodeContainer(dict_group, g, writer, sub_path, selectable ? "GPI" : nullptr);
    }
  }

  void EncodeField(const XMLElement* dict_field, const XMLElement* msg_parent, BitWriter* writer,
                   const std::string& path) {
    const char* name = dict_field->Attribute("name");
    if (!name) throw std::runtime_error("Field missing name at " + path);
    const bool repeatable = IsTrue(dict_field->Attribute("repeatable"));
    const bool selectable = IsTrue(dict_field->Attribute("selectable"));
    const std::string sub_path = path + ".Field(" + name + ")";
    if (repeatable) {
      auto fields = TakeAll(msg_parent, "Field", name);
      if (fields.empty()) throw std::runtime_error("Missing repeatable field instances at " + sub_path);
      for (const XMLElement* f : fields) EncodeContainer(dict_field, f, writer, sub_path, selectable ? "FPI" : nullptr);
    } else {
      const XMLElement* f = TakeOne(msg_parent, "Field", name, sub_path);
      EncodeContainer(dict_field, f, writer, sub_path, selectable ? "FPI" : nullptr);
    }
  }

  void EncodeContainer(const XMLElement* dict_container, const XMLElement* msg_container, BitWriter* writer,
                       const std::string& path, const char* selectable_indicator_tag) {
    if (!selectable_indicator_tag) {
      EncodeChildren(dict_container, msg_container, writer, path);
      EnsureNoUnexpected(msg_container, path);
      return;
    }
    const XMLElement* dict_indicator = dict_container->FirstChildElement(selectable_indicator_tag);
    if (!dict_indicator) throw std::runtime_error("Selectable container missing indicator in dictionary at " + path);
    const int bits = RequireBitsAttr(dict_indicator, path + "." + selectable_indicator_tag);
    const XMLElement* msg_indicator = TakeOne(msg_container, selectable_indicator_tag, nullptr, path);
    ValidateIndicatorKey(dict_indicator, msg_indicator, path + "." + selectable_indicator_tag);
    const uint64_t present = ParseUInt(msg_indicator->GetText(), path + "." + selectable_indicator_tag);
    writer->WriteBits(present, bits, path + "." + selectable_indicator_tag);

    if (present != 0) {
      for (const XMLElement* d = dict_container->FirstChildElement(); d; d = d->NextSiblingElement()) {
        if (d == dict_indicator) continue;
        const std::string tag = d->Name();
        if (tag == "DataUnit") {
          EncodeDataUnit(d, msg_container, writer, path);
        } else if (tag == "Group") {
          EncodeGroup(d, msg_container, writer, path);
        } else if (tag == "Field") {
          EncodeField(d, msg_container, writer, path);
        } else if (tag == "GRI" || tag == "FRI" || tag == "GPI" || tag == "FPI") {
          EncodeIndicator(d, msg_container, writer, path + "." + tag);
        } else {
          throw std::runtime_error("Unknown dictionary element at " + path + ": " + tag);
        }
      }
    }
    EnsureNoUnexpected(msg_container, path);
  }
};

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 5) {
      std::cerr << "Usage: vmf_encode <msg_structure.xml> <dic_content.xml> <msg.xml> <out.bin>\n";
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
    XMLDocument msg_doc;
    if (msg_doc.LoadFile(argv[3]) != tinyxml2::XML_SUCCESS) {
      std::cerr << "Failed to load message: " << argv[3] << "\n";
      return 1;
    }

    const XMLElement* dict_message = dict_doc.FirstChildElement("Message");
    const XMLElement* content_root = content_doc.FirstChildElement("dic");
    const XMLElement* msg_root = msg_doc.FirstChildElement("MessageContent");
    if (!dict_message || !content_root || !msg_root) {
      std::cerr << "Invalid XML root. Expect <Message>, <dic> and <MessageContent>.\n";
      return 1;
    }

    ContentDictionary content_dict(content_root);
    BitWriter writer;
    Encoder encoder(&content_dict);
    encoder.Encode(dict_message, msg_root, &writer);

    FILE* fp = std::fopen(argv[4], "wb");
    if (!fp) {
      std::cerr << "Failed to open output file: " << argv[4] << "\n";
      return 1;
    }
    const auto& bytes = writer.Bytes();
    if (!bytes.empty()) {
      const size_t wrote = std::fwrite(bytes.data(), 1, bytes.size(), fp);
      if (wrote != bytes.size()) {
        std::fclose(fp);
        std::cerr << "Failed to write output file completely\n";
        return 1;
      }
    }
    std::fclose(fp);

    std::cout << "Encoded bits: " << writer.BitSize() << ", bytes: " << bytes.size() << "\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Encode error: " << e.what() << "\n";
    return 2;
  }
}
