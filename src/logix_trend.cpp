/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "logix_trend.h"

#include <cstring>
#include <stdexcept>

#include <QDebug>

namespace logix
{

// ─── Helpers ────────────────────────────────────────────────────────────────

static void appendU8(std::vector<uint8_t>& v, uint8_t val)
{
  v.push_back(val);
}
static void appendU16(std::vector<uint8_t>& v, uint16_t val)
{
  v.push_back(val & 0xFF);
  v.push_back((val >> 8) & 0xFF);
}
static void appendU32(std::vector<uint8_t>& v, uint32_t val)
{
  v.push_back(val & 0xFF);
  v.push_back((val >> 8) & 0xFF);
  v.push_back((val >> 16) & 0xFF);
  v.push_back((val >> 24) & 0xFF);
}
static uint16_t readU16(const uint8_t* p)
{
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}
static uint32_t readU32(const uint8_t* p)
{
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

// ─── TrendInstance ──────────────────────────────────────────────────────────

TrendInstance::TrendInstance(EipConnection& conn,
                             const std::vector<std::pair<std::string, uint16_t>>& tags)
  : conn_(conn)
{
  for (const auto& [name, data_type] : tags)
  {
    tags_.push_back({ name, data_type, 0 });
  }
}

TrendInstance::~TrendInstance()
{
  try
  {
    stop();
  }
  catch (...)
  {
  }
}

void TrendInstance::start(uint32_t sample_rate_us, uint32_t buffer_size)
{
  if (running_)
  {
    return;
  }

  qDebug() << "Trend: creating for" << tags_.size() << "tags"
           << "buffer_size=" << buffer_size << "sample_rate_us=" << sample_rate_us;

  uint8_t status = createTrend(buffer_size);
  if (status != 0)
  {
    qDebug() << "Trend: createTrend FAILED status="
             << QString("0x%1").arg(status, 2, 16, QChar('0'));
    throw std::runtime_error("Failed to create trend (status=0x" + std::to_string(status) + ")");
  }
  qDebug() << "Trend: created instance_id=" << instance_id_;

  status = setAttributes(sample_rate_us, 0);
  if (status != 0)
  {
    qDebug() << "Trend: setAttributes FAILED status="
             << QString("0x%1").arg(status, 2, 16, QChar('0'));
    deleteTrend();
    throw std::runtime_error("Failed to set trend attributes");
  }

  // Add each tag to the trend instance
  for (size_t i = 0; i < tags_.size(); i++)
  {
    status = addTag(tags_[i].name);
    if (status != 0)
    {
      qDebug() << "Trend: addTag FAILED for" << QString::fromStdString(tags_[i].name)
               << "status=" << QString("0x%1").arg(status, 2, 16, QChar('0'));
      // Remove any tags we already added (reverse order)
      for (size_t j = i; j > 0; j--)
      {
        removeTag(tags_[j - 1].tag_number);
      }
      deleteTrend();
      throw std::runtime_error("Failed to bind tag '" + tags_[i].name + "' to trend");
    }
    tags_[i].tag_number = last_tag_number_;
    qDebug() << "Trend: added tag" << QString::fromStdString(tags_[i].name)
             << "tag_number=" << tags_[i].tag_number;
  }

  status = startTrend();
  if (status != 0)
  {
    qDebug() << "Trend: startTrend FAILED status="
             << QString("0x%1").arg(status, 2, 16, QChar('0'));
    for (size_t i = tags_.size(); i > 0; i--)
    {
      removeTag(tags_[i - 1].tag_number);
    }
    deleteTrend();
    throw std::runtime_error("Failed to start trend");
  }

  qDebug() << "Trend: started successfully with" << tags_.size() << "tags";
  running_ = true;
}

std::vector<TrendSample> TrendInstance::readData()
{
  if (!running_ || instance_id_ == 0)
  {
    return {};
  }

  auto path = buildCipPath(kTrendClass, instance_id_);
  uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

  std::vector<uint8_t> msg;
  appendU8(msg, 0x4C);  // Read Data service
  appendU8(msg, path_words);
  msg.insert(msg.end(), path.begin(), path.end());

  auto resp = conn_.sendCip(msg);
  auto cip = parseCipStatus(resp);

  std::vector<TrendSample> samples;

  constexpr int rec_size = 10;  // Fixed: 2 (tag_number) + 4 (timestamp) + 4 (value)
  if (cip.data.size() >= rec_size)
  {
    for (size_t i = 0; i + rec_size <= cip.data.size(); i += rec_size)
    {
      uint16_t tag_number = readU16(&cip.data[i]);
      if (tag_number == 0 || tag_number > tags_.size())
      {
        continue;  // Unknown tag number, skip
      }

      uint16_t tag_index = tag_number - 1;  // tag_number is 1-based
      const auto& tag = tags_[tag_index];

      TrendSample s;
      s.tag_index = tag_index;
      s.timestamp = readU32(&cip.data[i + 2]);
      s.value = interpretValue(&cip.data[i + 6], tag.data_type);
      samples.push_back(s);
    }
  }

  return samples;
}

void TrendInstance::stop()
{
  if (!running_ && instance_id_ == 0)
  {
    return;
  }

  if (running_)
  {
    try
    {
      stopTrend();
    }
    catch (...)
    {
    }
    running_ = false;
  }

  if (instance_id_ != 0)
  {
    // Remove all tags in reverse order
    for (size_t i = tags_.size(); i > 0; i--)
    {
      if (tags_[i - 1].tag_number != 0)
      {
        try
        {
          removeTag(tags_[i - 1].tag_number);
        }
        catch (...)
        {
        }
      }
    }
    try
    {
      deleteTrend();
    }
    catch (...)
    {
    }
    instance_id_ = 0;
  }
}

// ─── CIP Trend Services ────────────────────────────────────────────────────

uint8_t TrendInstance::createTrend(uint32_t buffer_size)
{
  auto path = buildCipPath(kTrendClass, 0);  // instance 0 = class-level create
  uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

  // SetAttributeList on create: 2 attrs
  // attr 8 = buffer size (UDINT), attr 3 = num_tags (USINT)
  std::vector<uint8_t> data;
  appendU16(data, 2);  // 2 attributes
  appendU16(data, 8);  // attr 8: buffer size
  appendU32(data, buffer_size);
  appendU16(data, 3);                              // attr 3: num tags
  appendU8(data, static_cast<uint8_t>(1));         // always create with 1, addTag expands

  std::vector<uint8_t> msg;
  appendU8(msg, 0x08);  // Create service
  appendU8(msg, path_words);
  msg.insert(msg.end(), path.begin(), path.end());
  msg.insert(msg.end(), data.begin(), data.end());

  auto resp = conn_.sendCip(msg);
  auto cip = parseCipStatus(resp);

  if (cip.status == 0 && cip.data.size() >= 4)
  {
    instance_id_ = readU32(&cip.data[0]);
  }

  return cip.status;
}

uint8_t TrendInstance::setAttributes(uint32_t sample_rate_us, uint8_t state)
{
  auto path = buildCipPath(kTrendClass, instance_id_);
  uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

  std::vector<uint8_t> data;
  appendU16(data, 2);  // 2 attributes
  appendU16(data, 1);  // attr 1: sample rate
  appendU32(data, sample_rate_us);
  appendU16(data, 5);  // attr 5: state
  appendU8(data, state);

  std::vector<uint8_t> msg;
  appendU8(msg, 0x04);  // Set Attribute List
  appendU8(msg, path_words);
  msg.insert(msg.end(), path.begin(), path.end());
  msg.insert(msg.end(), data.begin(), data.end());

  auto resp = conn_.sendCip(msg);
  return parseCipStatus(resp).status;
}

uint8_t TrendInstance::addTag(const std::string& tag_name)
{
  auto path = buildCipPath(kTrendClass, instance_id_);
  uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

  auto sym_path = buildSymbolicSegment(tag_name);
  uint8_t sym_path_words = static_cast<uint8_t>(sym_path.size() / 2);

  std::vector<uint8_t> data;
  appendU16(data, 1);              // constant
  appendU8(data, 1);               // tag_index
  appendU8(data, 1);               // type
  appendU8(data, sym_path_words);  // path size in words
  data.insert(data.end(), sym_path.begin(), sym_path.end());
  appendU32(data, 0xFFFFFFFF);  // mask = all bits

  std::vector<uint8_t> msg;
  appendU8(msg, 0x4E);  // Add Tag service
  appendU8(msg, path_words);
  msg.insert(msg.end(), path.begin(), path.end());
  msg.insert(msg.end(), data.begin(), data.end());

  auto resp = conn_.sendCip(msg);
  auto cip = parseCipStatus(resp);

  if (cip.status == 0 && cip.data.size() >= 2)
  {
    last_tag_number_ = readU16(&cip.data[0]);
  }
  else
  {
    last_tag_number_ = 0;
  }

  return cip.status;
}

uint8_t TrendInstance::startTrend()
{
  auto path = buildCipPath(kTrendClass, instance_id_);
  uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

  std::vector<uint8_t> msg;
  appendU8(msg, 0x06);  // Start (Apply Attributes)
  appendU8(msg, path_words);
  msg.insert(msg.end(), path.begin(), path.end());

  auto resp = conn_.sendCip(msg);
  return parseCipStatus(resp).status;
}

uint8_t TrendInstance::stopTrend()
{
  auto path = buildCipPath(kTrendClass, instance_id_);
  uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

  std::vector<uint8_t> msg;
  appendU8(msg, 0x07);  // Stop (Reset)
  appendU8(msg, path_words);
  msg.insert(msg.end(), path.begin(), path.end());

  auto resp = conn_.sendCip(msg);
  return parseCipStatus(resp).status;
}

uint8_t TrendInstance::removeTag(uint16_t tag_number)
{
  auto path = buildCipPath(kTrendClass, instance_id_);
  uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

  std::vector<uint8_t> data;
  appendU16(data, tag_number);

  std::vector<uint8_t> msg;
  appendU8(msg, 0x4F);  // Remove Tag
  appendU8(msg, path_words);
  msg.insert(msg.end(), path.begin(), path.end());
  msg.insert(msg.end(), data.begin(), data.end());

  auto resp = conn_.sendCip(msg);
  return parseCipStatus(resp).status;
}

uint8_t TrendInstance::deleteTrend()
{
  auto path = buildCipPath(kTrendClass, instance_id_);
  uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

  std::vector<uint8_t> msg;
  appendU8(msg, 0x09);  // Delete
  appendU8(msg, path_words);
  msg.insert(msg.end(), path.begin(), path.end());

  auto resp = conn_.sendCip(msg);
  auto cip = parseCipStatus(resp);
  if (cip.status == 0)
  {
    instance_id_ = 0;
  }
  return cip.status;
}

double TrendInstance::interpretValue(const uint8_t* raw, uint16_t data_type)
{
  // In multi-tag trend mode, the value field is always 4 bytes.
  // 8-byte types (LINT, LREAL) don't fit — treat as 4-byte.
  switch (data_type)
  {
    case kTypeBOOL:
    case kTypeUSINT:
      return static_cast<double>(raw[0]);
    case kTypeSINT:
      return static_cast<double>(static_cast<int8_t>(raw[0]));
    case kTypeINT: {
      int16_t v;
      std::memcpy(&v, raw, 2);
      return static_cast<double>(v);
    }
    case kTypeUINT: {
      uint16_t v;
      std::memcpy(&v, raw, 2);
      return static_cast<double>(v);
    }
    case kTypeDINT: {
      int32_t v;
      std::memcpy(&v, raw, 4);
      return static_cast<double>(v);
    }
    case kTypeUDINT: {
      uint32_t v;
      std::memcpy(&v, raw, 4);
      return static_cast<double>(v);
    }
    case kTypeREAL: {
      float v;
      std::memcpy(&v, raw, 4);
      return static_cast<double>(v);
    }
    default: {
      // For LINT, LREAL, and unknown types — read as 4-byte unsigned
      uint32_t v;
      std::memcpy(&v, raw, 4);
      return static_cast<double>(v);
    }
  }
}

}  // namespace logix
