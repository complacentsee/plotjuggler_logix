#include "logix_tag_browser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace logix {

// ─── Helpers ────────────────────────────────────────────────────────────────

static void appendU8(std::vector<uint8_t>& v, uint8_t val) { v.push_back(val); }
static void appendU16(std::vector<uint8_t>& v, uint16_t val) {
    v.push_back(val & 0xFF); v.push_back((val >> 8) & 0xFF);
}
static void appendU32(std::vector<uint8_t>& v, uint32_t val) {
    v.push_back(val & 0xFF); v.push_back((val >> 8) & 0xFF);
    v.push_back((val >> 16) & 0xFF); v.push_back((val >> 24) & 0xFF);
}
static uint16_t readU16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
static uint32_t readU32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

bool TagBrowser::isFilteredTag(const std::string& name) {
    static const std::vector<std::string> filters = {"__", "Routine:", "Map:", "Task:", "UDI:"};
    for (const auto& f : filters) {
        if (name.find(f) != std::string::npos) return true;
    }
    return false;
}

std::string TagBrowser::lookupTypeName(uint16_t symbol_type, uint16_t data_type_value) const {
    // Check if it's a known atomic type
    std::string name = cipTypeName(symbol_type);
    if (name != "UNKNOWN") return name;

    // Check if we have a UDT definition for it
    auto it = udts_.find(data_type_value);
    if (it != udts_.end()) return it->second.name;

    return "UNKNOWN(0x" + std::to_string(data_type_value) + ")";
}

// ─── Tag List Request (Service 0x55) ────────────────────────────────────────

std::vector<uint8_t> TagBrowser::buildTagListRequest(const std::string& program_name,
                                                      uint16_t offset) {
    std::vector<uint8_t> path_segment;

    // If program-scoped, add symbolic segment for program name
    if (!program_name.empty()) {
        appendU8(path_segment, 0x91);
        appendU8(path_segment, static_cast<uint8_t>(program_name.size()));
        for (char c : program_name) path_segment.push_back(static_cast<uint8_t>(c));
        if (program_name.size() % 2) path_segment.push_back(0x00);
    }

    // Class 0x6B, instance = offset
    appendU16(path_segment, 0x6B20); // 0x20, 0x6B packed as little-endian word
    if (offset < 256) {
        appendU8(path_segment, 0x24);
        appendU8(path_segment, static_cast<uint8_t>(offset));
    } else {
        appendU16(path_segment, 0x0025); // 0x25, 0x00 packed
        appendU16(path_segment, offset);
    }

    uint8_t path_words = static_cast<uint8_t>(path_segment.size() / 2);

    // Attributes: count=3, symbol_name(1), symbol_type(2), byte_count(8)
    std::vector<uint8_t> attributes;
    appendU16(attributes, 3);    // attribute count
    appendU16(attributes, 1);    // symbol name
    appendU16(attributes, 2);    // symbol type
    appendU16(attributes, 8);    // byte count

    std::vector<uint8_t> request;
    appendU8(request, 0x55);     // Get Instance Attribute List
    appendU8(request, path_words);
    request.insert(request.end(), path_segment.begin(), path_segment.end());
    request.insert(request.end(), attributes.begin(), attributes.end());

    return request;
}

// ─── Parse Tag Packet ───────────────────────────────────────────────────────

std::vector<TagInfo> TagBrowser::parseTagPacket(const std::vector<uint8_t>& data,
                                                 const std::string& program_name,
                                                 uint16_t& last_offset) {
    std::vector<TagInfo> tags;

    // Tags start at byte 50 in the response (after CIP + ENIP headers)
    // But since we receive raw CIP data after status parsing, offset from 0
    size_t pos = 0;

    while (pos + 6 < data.size()) {
        TagInfo tag;

        // Instance ID (offset for next request)
        tag.instance_id = readU16(&data[pos]);
        last_offset = tag.instance_id;

        // Tag name length at pos+4
        uint16_t name_len = readU16(&data[pos + 4]);
        if (pos + 6 + name_len + 2 > data.size()) break;

        // Tag name
        std::string name(data.begin() + pos + 6, data.begin() + pos + 6 + name_len);
        if (!program_name.empty()) {
            tag.name = program_name + "." + name;
        } else {
            tag.name = name;
        }

        // Symbol type word at pos + 6 + name_len
        uint16_t type_val = readU16(&data[pos + 6 + name_len]);
        tag.symbol_type = type_val & 0xFF;
        tag.data_type_value = type_val & 0xFFF;
        tag.array_dims = (type_val & 0x6000) >> 13;
        tag.is_struct = (type_val & 0x8000) != 0;

        // Array size follows type word if array
        if (tag.array_dims > 0 && pos + 6 + name_len + 4 <= data.size()) {
            tag.array_size = readU16(&data[pos + 6 + name_len + 2]);
        }

        // Check for Program: prefix to track program names
        if (program_name.empty() && name.find("Program:") == 0) {
            program_names_.push_back(name);
        }

        // Filter garbage tags
        if (!isFilteredTag(tag.name)) {
            tags.push_back(tag);
        }

        // Each tag record is name_len + 20 bytes (fixed overhead of 20)
        pos += name_len + 20;
    }

    return tags;
}

// ─── Browse ─────────────────────────────────────────────────────────────────

std::vector<TagInfo> TagBrowser::browse(EipConnection& conn, bool include_program_tags) {
    program_names_.clear();
    udts_.clear();

    std::vector<TagInfo> all_tags;

    // Get controller-scoped tags
    {
        uint16_t offset = 0;
        int status = 6;
        while (status == 6 || status == 0) {
            auto request = buildTagListRequest("", offset);
            auto response = conn.sendUnconnected(request);
            auto cip = parseCipStatus(response);
            status = cip.status;

            if (status == 0 || status == 6) {
                uint16_t last_offset = 0;
                auto tags = parseTagPacket(cip.data, "", last_offset);
                all_tags.insert(all_tags.end(), tags.begin(), tags.end());
                offset = last_offset + 1;
            }

            if (status == 0) break; // Done, no more data
        }
    }

    // Get program-scoped tags
    if (include_program_tags) {
        for (const auto& prog : program_names_) {
            uint16_t offset = 0;
            int status = 6;
            while (status == 6 || status == 0) {
                auto request = buildTagListRequest(prog, offset);
                auto response = conn.sendUnconnected(request);
                auto cip = parseCipStatus(response);
                status = cip.status;

                if (status == 0 || status == 6) {
                    uint16_t last_offset = 0;
                    auto tags = parseTagPacket(cip.data, prog, last_offset);
                    all_tags.insert(all_tags.end(), tags.begin(), tags.end());
                    offset = last_offset + 1;
                }

                if (status == 0) break;
            }
        }
    }

    // Resolve UDT definitions
    resolveUdts(conn, all_tags);

    // Assign human-readable type names
    for (auto& tag : all_tags) {
        tag.data_type_name = lookupTypeName(tag.symbol_type, tag.data_type_value);
    }

    return all_tags;
}

// ─── UDT Resolution ────────────────────────────────────────────────────────

std::vector<uint8_t> TagBrowser::getTemplateAttribute(EipConnection& conn, uint16_t instance) {
    // GetAttributeList (0x03) on class 0x6C (Template), requesting attrs 4, 3, 2, 1
    auto path = buildCipPath(0x6C, instance);
    uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

    std::vector<uint8_t> request;
    appendU8(request, 0x03); // Get Attribute List
    appendU8(request, path_words);
    request.insert(request.end(), path.begin(), path.end());
    appendU16(request, 4); // attribute count
    appendU16(request, 4); // attr 4
    appendU16(request, 3); // attr 3
    appendU16(request, 2); // attr 2
    appendU16(request, 1); // attr 1

    return conn.sendUnconnected(request);
}

std::vector<uint8_t> TagBrowser::getTemplate(EipConnection& conn, uint16_t instance, int data_len) {
    std::vector<uint8_t> full_data;
    int part_offset = 0;
    int remaining = data_len;
    int status = 0;

    while (remaining > 0) {
        auto path = buildCipPath(0x6C, instance);
        uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

        std::vector<uint8_t> req_data;
        appendU32(req_data, part_offset);
        appendU16(req_data, static_cast<uint16_t>(remaining));

        std::vector<uint8_t> request;
        appendU8(request, 0x4C); // Read Template
        appendU8(request, path_words);
        request.insert(request.end(), path.begin(), path.end());
        request.insert(request.end(), req_data.begin(), req_data.end());

        auto response = conn.sendUnconnected(request);
        auto cip = parseCipStatus(response);

        if (cip.status != 0 && cip.status != 6) break;

        if (full_data.empty()) {
            full_data = cip.data;
        } else {
            full_data.insert(full_data.end(), cip.data.begin(), cip.data.end());
        }

        part_offset = static_cast<int>(full_data.size());
        remaining = data_len - part_offset;

        if (cip.status == 0) break;
    }

    return full_data;
}

void TagBrowser::resolveUdts(EipConnection& conn, const std::vector<TagInfo>& tags) {
    // Collect unique struct data type values
    std::set<uint16_t> unique_types;
    for (const auto& tag : tags) {
        if (tag.is_struct) {
            unique_types.insert(tag.data_type_value);
        }
    }

    // Iteratively resolve UDTs (nested structs may need multiple passes)
    std::set<uint16_t> to_resolve = unique_types;

    while (!to_resolve.empty()) {
        std::set<uint16_t> next_resolve;

        for (uint16_t type_id : to_resolve) {
            if (udts_.count(type_id)) continue;

            try {
                // Get template attribute to determine size and member count
                auto attr_resp = getTemplateAttribute(conn, type_id);
                auto attr_cip = parseCipStatus(attr_resp);
                if (attr_cip.status != 0 || attr_cip.data.size() < 20) continue;

                // Parse: attr4 status(2) + handle(4), attr3 status(2) + member_count...
                // The response format includes status words for each attribute
                // Simplified: extract what we need
                // From pylogix: block = temp[46:], val at offset 10, member_count at 24
                // Since we get raw CIP data, offsets differ
                // Let's use the approach: get the full template data directly

                uint32_t obj_def_size = 0;
                uint16_t member_count = 0;

                // Parse attribute responses
                // Each attr response: attr_id(2) + status(2) + data
                size_t pos = 0;
                auto& d = attr_cip.data;
                // Attr count first
                if (d.size() < 2) continue;
                // uint16_t attr_count = readU16(&d[0]);
                pos = 2;

                // Parse each attribute response
                for (int a = 0; a < 4 && pos + 4 <= d.size(); a++) {
                    uint16_t attr_id = readU16(&d[pos]);
                    uint16_t attr_status = readU16(&d[pos + 2]);
                    pos += 4;

                    if (attr_status != 0) continue;

                    switch (attr_id) {
                        case 4: // Object Definition Size
                            if (pos + 4 <= d.size()) {
                                obj_def_size = readU32(&d[pos]);
                                pos += 4;
                            }
                            break;
                        case 3: // Template member count (includes hidden members)
                            if (pos + 2 <= d.size()) {
                                // This is the structure handle, not member count
                                pos += 4; // handle is 4 bytes
                            }
                            break;
                        case 2: // Member count
                            if (pos + 2 <= d.size()) {
                                member_count = readU16(&d[pos]);
                                pos += 2;
                            }
                            break;
                        case 1: // Structure handle
                            if (pos + 2 <= d.size()) {
                                pos += 2;
                            }
                            break;
                    }
                }

                if (obj_def_size == 0 || member_count == 0) continue;

                // Calculate template data size (from pylogix)
                int words = static_cast<int>(obj_def_size * 4) - 23;
                int size = static_cast<int>(std::ceil(words / 4.0)) * 4;

                // Get full template
                auto tmpl_data = getTemplate(conn, type_id, size);
                if (tmpl_data.empty()) continue;

                // Parse template: member definitions (8 bytes each) followed by name strings
                UdtDef udt;
                udt.type_id = type_id;

                size_t member_defs_size = member_count * 8;
                if (tmpl_data.size() < member_defs_size) continue;

                // Names follow the member definitions, null-separated
                // First name (before first null) is the UDT name itself, possibly with
                // semicolon-separated scope info
                std::vector<std::string> names;
                {
                    size_t name_start = member_defs_size;
                    std::string current;
                    for (size_t i = name_start; i < tmpl_data.size(); i++) {
                        if (tmpl_data[i] == 0x00) {
                            if (!current.empty()) names.push_back(current);
                            current.clear();
                        } else {
                            current += static_cast<char>(tmpl_data[i]);
                        }
                    }
                    if (!current.empty()) names.push_back(current);
                }

                // First entry contains UDT name (possibly with ;scope info)
                if (!names.empty()) {
                    auto semicol = names[0].find(';');
                    udt.name = (semicol != std::string::npos) ?
                               names[0].substr(0, semicol) : names[0];
                }

                // Parse each member
                for (uint16_t i = 0; i < member_count && i + 1 < names.size(); i++) {
                    UdtField field;
                    field.name = names[i + 1];

                    // Parse 8-byte member definition
                    const uint8_t* def = &tmpl_data[i * 8];
                    uint16_t type_word = readU16(&def[2]);
                    field.symbol_type = type_word & 0xFF;
                    field.data_type_value = type_word & 0xFFF;
                    field.array_dims = (type_word & 0x6000) >> 13;
                    field.is_struct = (type_word & 0x8000) != 0;

                    if (field.array_dims > 0) {
                        field.array_size = readU16(&def[0]);
                    }

                    field.is_internal = field.name.substr(0, 2) == "__";

                    // Check if this field is a nested struct we haven't resolved
                    if (field.is_struct && !udts_.count(field.data_type_value)) {
                        next_resolve.insert(field.data_type_value);
                    }

                    if (!field.is_internal) {
                        udt.fields.push_back(field);
                    }
                }

                udts_[type_id] = udt;

            } catch (const std::exception&) {
                // Skip types we can't resolve
                continue;
            }
        }

        to_resolve = next_resolve;
    }

    // Assign type names to UDT fields
    for (auto& [id, udt] : udts_) {
        for (auto& field : udt.fields) {
            field.data_type_name = lookupTypeName(field.symbol_type, field.data_type_value);
        }
    }
}

// ─── Struct Member Expansion ────────────────────────────────────────────────

std::vector<std::pair<std::string, uint16_t>> TagBrowser::expandStructMembers(
    const TagInfo& tag) const {
    std::vector<std::pair<std::string, uint16_t>> result;
    if (tag.is_struct) {
        expandMembers(tag.name, tag.data_type_value, result);
    } else if (isNumericType(tag.symbol_type)) {
        result.push_back({tag.name, tag.symbol_type});
    }
    return result;
}

void TagBrowser::expandMembers(const std::string& prefix, uint16_t type_id,
                                std::vector<std::pair<std::string, uint16_t>>& result) const {
    auto it = udts_.find(type_id);
    if (it == udts_.end()) return;

    for (const auto& field : it->second.fields) {
        std::string full_name = prefix + "." + field.name;
        if (field.is_struct) {
            expandMembers(full_name, field.data_type_value, result);
        } else if (isNumericType(field.symbol_type)) {
            result.push_back({full_name, field.symbol_type});
        }
    }
}

const UdtDef* TagBrowser::getUdt(uint16_t type_id) const {
    auto it = udts_.find(type_id);
    return (it != udts_.end()) ? &it->second : nullptr;
}

} // namespace logix
