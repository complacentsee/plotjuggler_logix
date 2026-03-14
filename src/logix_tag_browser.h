/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "logix_eip.h"
#include <map>
#include <string>
#include <vector>

namespace logix
{

/// Represents a single PLC tag (controller or program scoped)
struct TagInfo
{
  std::string name;  // Full tag name (e.g. "Program:MainProgram.MyTag")
  uint16_t instance_id = 0;
  uint16_t symbol_type = 0;      // Low byte of type word
  uint16_t data_type_value = 0;  // Lower 12 bits
  std::string data_type_name;    // "DINT", "REAL", "MyUDT", etc.
  int array_dims = 0;            // 0=scalar, 1=1D, 2=2D, 3=3D
  bool is_struct = false;
  int array_size = 0;  // Element count for first dimension
};

/// Represents a UDT field/member
struct UdtField
{
  std::string name;
  uint16_t symbol_type = 0;
  uint16_t data_type_value = 0;
  std::string data_type_name;
  int array_dims = 0;
  bool is_struct = false;
  int array_size = 0;
  bool is_internal = false;  // Starts with "__"
};

/// Represents a UDT (User Defined Type) definition
struct UdtDef
{
  uint16_t type_id = 0;
  std::string name;
  std::vector<UdtField> fields;
};

/// Browses tags on a ControlLogix/CompactLogix PLC
class TagBrowser
{
public:
  /// Browse all tags on the PLC. Connection must already be established.
  /// Returns list of all tags (controller + program scoped).
  std::vector<TagInfo> browse(EipConnection& conn, bool include_program_tags = true);

  /// Get UDT definition by type ID (populated after browse())
  const UdtDef* getUdt(uint16_t type_id) const;

  /// Get all resolved UDTs
  const std::map<uint16_t, UdtDef>& allUdts() const
  {
    return udts_;
  }

  /// Expand a struct tag into its trendable (numeric) member paths
  /// Returns fully qualified names like "MyTag.Member1", "MyTag.SubStruct.Field"
  std::vector<std::pair<std::string, uint16_t>> expandStructMembers(const TagInfo& tag) const;

private:
  // Request tag list starting from offset (service 0x55)
  std::vector<uint8_t> buildTagListRequest(const std::string& program_name, uint16_t offset);

  // Parse a tag list response packet into TagInfo entries
  std::vector<TagInfo> parseTagPacket(const std::vector<uint8_t>& data,
                                      const std::string& program_name, uint16_t& last_offset);

  // Resolve UDT definitions for all struct tags
  void resolveUdts(EipConnection& conn, const std::vector<TagInfo>& tags);

  // Get template attribute (service 0x03, class 0x6C)
  std::vector<uint8_t> getTemplateAttribute(EipConnection& conn, uint16_t instance);

  // Get template members (service 0x4C, class 0x6C)
  std::vector<uint8_t> getTemplate(EipConnection& conn, uint16_t instance, int data_len);

  // Check if tag name should be filtered out
  static bool isFilteredTag(const std::string& name);

  // CIP type name lookup
  std::string lookupTypeName(uint16_t symbol_type, uint16_t data_type_value) const;

  // Recursive helper for expandStructMembers
  void expandMembers(const std::string& prefix, uint16_t type_id,
                     std::vector<std::pair<std::string, uint16_t>>& result) const;

  std::map<uint16_t, UdtDef> udts_;
  std::vector<std::string> program_names_;
};

}  // namespace logix
