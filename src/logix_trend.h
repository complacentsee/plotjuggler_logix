/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "logix_eip.h"
#include <string>
#include <vector>

namespace logix
{

constexpr uint16_t kTrendClass = 0xB2;

/// A tag bound to a trend instance
struct TagEntry
{
  std::string name;
  uint16_t data_type;
  uint16_t tag_number;  // PLC-assigned, 1-based
};

/// A single sample from a trend buffer
struct TrendSample
{
  uint16_t tag_index;    // 0-based index into tags vector
  uint32_t timestamp;    // CIP Wall Clock ticks (128 us per tick)
  double value;          // Interpreted value (converted from raw bytes + type)
};

/// Manages a single CIP trend instance on the PLC (class 0xB2).
/// Supports multiple tags per instance — ReadData returns interleaved
/// 10-byte entries identified by tag_number.
class TrendInstance
{
public:
  TrendInstance(EipConnection& conn,
               const std::vector<std::pair<std::string, uint16_t>>& tags);
  ~TrendInstance();

  TrendInstance(const TrendInstance&) = delete;
  TrendInstance& operator=(const TrendInstance&) = delete;

  /// Create the trend object on the PLC, configure rate, bind tags, and start
  void start(uint32_t sample_rate_us, uint32_t buffer_size = 0x1000);

  /// Read available samples from the trend buffer
  std::vector<TrendSample> readData();

  /// Stop, unbind tags, and delete the trend instance
  void stop();

  bool isRunning() const
  {
    return running_;
  }
  const std::vector<TagEntry>& tags() const
  {
    return tags_;
  }
  size_t numTags() const
  {
    return tags_.size();
  }
  uint32_t instanceId() const
  {
    return instance_id_;
  }

  /// Size of one sample record in bytes — always 10 for multi-tag trends
  /// (2 tag_number + 4 timestamp + 4 value)
  int sampleSize() const
  {
    return 10;
  }

private:
  uint8_t createTrend(uint32_t buffer_size);
  uint8_t setAttributes(uint32_t sample_rate_us, uint8_t state);
  uint8_t addTag(const std::string& tag_name);
  uint8_t startTrend();
  uint8_t stopTrend();
  uint8_t removeTag(uint16_t tag_number);
  uint8_t deleteTrend();

  static double interpretValue(const uint8_t* raw_bytes, uint16_t data_type);

  EipConnection& conn_;
  std::vector<TagEntry> tags_;
  uint32_t instance_id_ = 0;
  bool running_ = false;

  // Temporary storage for addTag response parsing
  uint16_t last_tag_number_ = 0;
};

}  // namespace logix
