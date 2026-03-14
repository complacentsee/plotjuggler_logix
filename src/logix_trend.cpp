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

TrendInstance::TrendInstance(EipConnection& conn, const std::string& tag_name, uint16_t data_type)
  : conn_(conn), tag_name_(tag_name), data_type_(data_type)
{
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

  qDebug() << "Trend: creating for" << QString::fromStdString(tag_name_)
           << "buffer_size=" << buffer_size << "sample_rate_us=" << sample_rate_us
           << "sample_size=" << sampleSize();

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

  status = addTag();
  if (status != 0)
  {
    qDebug() << "Trend: addTag FAILED status=" << QString("0x%1").arg(status, 2, 16, QChar('0'));
    deleteTrend();
    throw std::runtime_error("Failed to bind tag '" + tag_name_ + "' to trend");
  }

  status = startTrend();
  if (status != 0)
  {
    qDebug() << "Trend: startTrend FAILED status="
             << QString("0x%1").arg(status, 2, 16, QChar('0'));
    removeTag();
    deleteTrend();
    throw std::runtime_error("Failed to start trend");
  }

  qDebug() << "Trend: started successfully for" << QString::fromStdString(tag_name_);
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

  int rec_size = sampleSize();
  if (rec_size > 0 && cip.data.size() >= static_cast<size_t>(rec_size))
  {
    for (size_t i = 0; i + rec_size <= cip.data.size(); i += rec_size)
    {
      TrendSample s;
      s.count = readU16(&cip.data[i]);
      s.timestamp = readU32(&cip.data[i + 2]);
      s.value = interpretValue(&cip.data[i + 6]);
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
    try
    {
      removeTag();
    }
    catch (...)
    {
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
  appendU16(data, 3);  // attr 3: num tags
  appendU8(data, 1);   // 1 tag per trend (PLC limitation)

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

uint8_t TrendInstance::addTag()
{
  auto path = buildCipPath(kTrendClass, instance_id_);
  uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

  auto sym_path = buildSymbolicSegment(tag_name_);
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
  return parseCipStatus(resp).status;
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

uint8_t TrendInstance::removeTag()
{
  auto path = buildCipPath(kTrendClass, instance_id_);
  uint8_t path_words = static_cast<uint8_t>(path.size() / 2);

  std::vector<uint8_t> data;
  appendU16(data, 1);  // tag index

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

double TrendInstance::interpretValue(const uint8_t* raw) const
{
  switch (data_type_)
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
    case kTypeLINT: {
      int64_t v;
      std::memcpy(&v, raw, 8);
      return static_cast<double>(v);
    }
    case kTypeLREAL: {
      double v;
      std::memcpy(&v, raw, 8);
      return v;
    }
    default: {
      uint32_t v;
      std::memcpy(&v, raw, 4);
      return static_cast<double>(v);
    }
  }
}

}  // namespace logix
