/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "datastream_logix_trend.h"

#include <QDomDocument>
#include <QMessageBox>
#include <QDebug>
#include <chrono>
#include <cmath>
#include <algorithm>

namespace logix
{

DataStreamLogixTrend::DataStreamLogixTrend() = default;

DataStreamLogixTrend::~DataStreamLogixTrend()
{
  shutdown();
}

bool DataStreamLogixTrend::start(QStringList* selected_datasources)
{
  if (running_)
  {
    return false;
  }

  // Show configuration dialog
  LogixConfigDialog dialog(nullptr, config_.ip_address.empty() ? nullptr : &config_,
                           cached_tags_, cached_browser_);
  if (dialog.exec() != QDialog::Accepted)
  {
    return false;
  }

  config_ = dialog.getConfig();
  cached_tags_ = dialog.getTags();
  cached_browser_ = dialog.getBrowser();

  // Clear any data from previous session
  {
    std::lock_guard<std::mutex> lock(mutex());
    dataMap().numeric.clear();
    dataMap().strings.clear();
  }

  // Connect to PLC
  try
  {
    auto route = EipConnection::parseRouteString(config_.route);
    conn_ = std::make_unique<EipConnection>();
    conn_->connect(config_.ip_address, route);
  }
  catch (const std::exception& e)
  {
    QMessageBox::critical(nullptr, "Connection Error",
                          QString("Failed to connect:\n%1").arg(e.what()));
    emit closed();
    return false;
  }

  // Group tags into multi-tag trend instances
  trends_.clear();
  trend_time_.clear();
  start_time_ = std::chrono::steady_clock::now();
  poll_interval_ms_ = 200;

  size_t max_tags = maxTagsPerInstance(config_.sample_rate_us, conn_->connectionSize());
  size_t total_tags = config_.selected_tags.size();
  size_t num_instances = (total_tags + max_tags - 1) / max_tags;
  // Balance tags evenly across instances (e.g., 7-7-7 instead of 9-9-3)
  size_t tags_per_inst = (total_tags + num_instances - 1) / num_instances;

  qDebug() << "Trend: connection_size=" << conn_->connectionSize()
           << "max_tags_per_instance=" << max_tags
           << "balanced_tags_per_instance=" << tags_per_inst
           << "instances=" << num_instances
           << "total_tags=" << total_tags;

  size_t tag_offset = 0;
  for (size_t inst = 0; inst < num_instances; inst++)
  {
    // Read all running instances to prevent FIFO overflow during setup
    for (auto& existing : trends_)
    {
      existing->readData();
    }

    size_t chunk_size = std::min(tags_per_inst, total_tags - tag_offset);
    std::vector<std::pair<std::string, uint16_t>> chunk(
        config_.selected_tags.begin() + tag_offset,
        config_.selected_tags.begin() + tag_offset + chunk_size);
    tag_offset += chunk_size;

    try
    {
      auto trend = std::make_unique<TrendInstance>(*conn_, chunk);
      auto [buf_size, poll_ms] =
          computeBufferParams(config_.sample_rate_us, 10, chunk.size(), conn_->connectionSize());
      poll_interval_ms_ = std::min(poll_interval_ms_, poll_ms);
      trend->start(config_.sample_rate_us, buf_size);
      trends_.push_back(std::move(trend));
    }
    catch (const std::exception& e)
    {
      QMessageBox::critical(nullptr, "Trend Error",
                            QString("Failed to start trend instance %1:\n%2")
                                .arg(trends_.size())
                                .arg(e.what()));
      trends_.clear();
      conn_->close();
      conn_.reset();
      emit closed();
      return false;
    }
  }

  if (trends_.empty())
  {
    conn_->close();
    conn_.reset();
    return false;
  }

  // Recompute poll interval accounting for CIP read overhead of all instances.
  // Each instance read takes ~7ms. Total cycle = sleep + N × read_time.
  // Buffer fill_time must exceed total cycle time.
  {
    constexpr uint32_t kReadTimePerInstance = 7;  // ms, estimated CIP round-trip
    uint32_t estimated_cycle_ms = static_cast<uint32_t>(trends_.size()) * kReadTimePerInstance;
    uint32_t max_entries = (conn_->connectionSize() > 20 ? conn_->connectionSize() - 20 : 480) / 10;

    // Find the tightest instance (most tags = fastest fill)
    size_t max_tags_in_any = 0;
    for (const auto& t : trends_)
    {
      max_tags_in_any = std::max(max_tags_in_any, t->numTags());
    }

    uint32_t periods = max_entries / static_cast<uint32_t>(max_tags_in_any);
    double fill_ms = periods * (config_.sample_rate_us / 1000.0);

    // sleep = (fill_time - cycle_overhead) / 2, so total ≈ fill_time × 0.75
    double available_ms = fill_ms - estimated_cycle_ms;
    if (available_ms > 0)
    {
      poll_interval_ms_ = std::max(5u, static_cast<uint32_t>(available_ms / 2.0));
    }
    else
    {
      poll_interval_ms_ = 5;  // minimum — bandwidth is very tight
    }
    poll_interval_ms_ = std::min(200u, poll_interval_ms_);
  }

  qDebug() << "Trend: created" << trends_.size() << "instances, poll_interval=" << poll_interval_ms_ << "ms";

  // Populate selected_datasources for PlotJuggler
  if (selected_datasources)
  {
    for (const auto& trend : trends_)
    {
      for (const auto& tag : trend->tags())
      {
        selected_datasources->append(QString::fromStdString(tag.name));
      }
    }
  }

  // Start polling thread
  running_ = true;
  poll_thread_ = std::thread(&DataStreamLogixTrend::pollingLoop, this);

  return true;
}

void DataStreamLogixTrend::shutdown()
{
  if (!running_)
  {
    return;
  }

  running_ = false;
  if (poll_thread_.joinable())
  {
    poll_thread_.join();
  }

  // Stop all trend instances (stop → remove tags → delete)
  trends_.clear();

  // Close connection
  if (conn_)
  {
    conn_->close();
    conn_.reset();
  }
}

bool DataStreamLogixTrend::isRunning() const
{
  return running_;
}

void DataStreamLogixTrend::pollingLoop()
{
  auto poll_interval = std::chrono::milliseconds(poll_interval_ms_);
  constexpr double kTickToSeconds = 128.0 / 1e6;
  int poll_count = 0;

  while (running_)
  {
    bool got_data = false;
    auto cycle_start = std::chrono::steady_clock::now();
    double host_now_s = std::chrono::duration<double>(cycle_start - start_time_).count();

    for (size_t idx = 0; idx < trends_.size(); idx++)
    {
      auto& trend = trends_[idx];
      try
      {
        auto samples = trend->readData();

        if (!samples.empty())
        {
          std::lock_guard<std::mutex> lock(mutex());

          int accepted = 0;
          int skipped = 0;

          for (const auto& sample : samples)
          {
            const auto& tag = trend->tags()[sample.tag_index];
            auto& series = dataMap().getOrCreateNumeric(tag.name);
            auto& ts = trend_time_[tag.name];

            if (!ts.initialized)
            {
              // Anchor this tag: PLC timestamp → host time
              ts.base_plc_ts = sample.timestamp;
              ts.base_host_s = host_now_s;
              ts.initialized = true;
            }

            // Compute time relative to this tag's base,
            // then offset to host time axis
            uint32_t delta;
            if (sample.timestamp >= ts.base_plc_ts)
            {
              delta = sample.timestamp - ts.base_plc_ts;
            }
            else
            {
              delta = (0xFFFFFFFF - ts.base_plc_ts) + sample.timestamp + 1;
            }
            double time_s = ts.base_host_s + static_cast<double>(delta) * kTickToSeconds;

            // Enforce monotonic timestamps — skip already-seen entries
            if (time_s > ts.last_time_s)
            {
              series.pushBack({ time_s, sample.value });
              ts.last_time_s = time_s;
              accepted++;
            }
            else
            {
              skipped++;
            }
          }

          if (poll_count < 5)
          {
            qDebug() << "Poll" << poll_count << "trend" << idx
                     << "tags=" << trend->numTags()
                     << "samples=" << samples.size()
                     << "accepted=" << accepted << "skipped=" << skipped;
          }

          got_data = true;
        }
        else if (poll_count < 3)
        {
          qDebug() << "Poll" << poll_count << "trend" << idx << "samples=0 (empty)";
        }
      }
      catch (const std::exception&)
      {
        running_ = false;
        emit closed();
        return;
      }
    }

    if (poll_count < 3)
    {
      auto cycle_end = std::chrono::steady_clock::now();
      auto cycle_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(cycle_end - cycle_start).count();
      qDebug() << "Poll" << poll_count << "cycle_time=" << cycle_ms << "ms"
               << "poll_interval=" << poll_interval_ms_ << "ms";
    }
    poll_count++;

    if (got_data)
    {
      emit dataReceived();
    }

    std::this_thread::sleep_for(poll_interval);
  }
}

// ─── Buffer Sizing ──────────────────────────────────────────────────────────

size_t DataStreamLogixTrend::maxTagsPerInstance(uint32_t sample_rate_us, uint32_t connection_size,
                                                uint32_t min_fill_time_ms)
{
  uint32_t max_payload = connection_size > 20 ? connection_size - 20 : 480;
  size_t max_entries = max_payload / 10;  // 10 bytes per entry
  double sample_rate_ms = sample_rate_us / 1000.0;

  // fill_time = (max_entries / num_tags) * sample_rate_ms
  // We need fill_time >= min_fill_time_ms (must be > 2× CIP round-trip)
  // → num_tags <= max_entries * sample_rate_ms / min_fill_time_ms
  size_t max_tags_fill = static_cast<size_t>(max_entries * sample_rate_ms / min_fill_time_ms);

  // Also cap at max_entries / 4 to ensure at least 4 periods per read
  size_t max_tags_periods = max_entries / 4;

  size_t max_tags = std::min(max_tags_fill, max_tags_periods);
  return std::max(static_cast<size_t>(1), max_tags);
}

std::pair<uint32_t, uint32_t> DataStreamLogixTrend::computeBufferParams(uint32_t sample_rate_us,
                                                                        int sample_size,
                                                                        size_t num_tags,
                                                                        uint32_t connection_size)
{
  // Size the buffer to fit within a single CIP response.
  // One read drains the buffer completely — no backlog buildup.
  uint32_t max_payload = connection_size > 20 ? connection_size - 20 : 480;
  uint32_t max_entries = max_payload / sample_size;
  uint32_t buffer_size = max_entries * sample_size;

  // Poll interval: drain before buffer fills (50% safety margin)
  uint32_t n_tags = num_tags > 0 ? static_cast<uint32_t>(num_tags) : 1;
  uint32_t periods = max_entries / n_tags;
  double fill_time_ms = periods * (sample_rate_us / 1000.0);
  uint32_t poll_ms = static_cast<uint32_t>(fill_time_ms / 2.0);

  // Clamp poll interval: min 5ms, max 200ms
  poll_ms = std::max(5u, std::min(200u, poll_ms));

  return { buffer_size, poll_ms };
}

// ─── XML State Persistence ──────────────────────────────────────────────────

bool DataStreamLogixTrend::xmlSaveState(QDomDocument& doc, QDomElement& parent) const
{
  auto elem = doc.createElement("LogixTrend");

  elem.setAttribute("ip", QString::fromStdString(config_.ip_address));
  elem.setAttribute("route", QString::fromStdString(config_.route));
  elem.setAttribute("sample_rate_us", static_cast<int>(config_.sample_rate_us));

  for (const auto& [name, type] : config_.selected_tags)
  {
    auto tag_elem = doc.createElement("Tag");
    tag_elem.setAttribute("name", QString::fromStdString(name));
    tag_elem.setAttribute("type", type);
    elem.appendChild(tag_elem);
  }

  parent.appendChild(elem);
  return true;
}

bool DataStreamLogixTrend::xmlLoadState(const QDomElement& parent)
{
  auto elem = parent.firstChildElement("LogixTrend");
  if (elem.isNull())
  {
    return false;
  }

  config_.ip_address = elem.attribute("ip").toStdString();
  config_.route = elem.attribute("route").toStdString();
  config_.sample_rate_us = elem.attribute("sample_rate_us", "10000").toUInt();

  config_.selected_tags.clear();
  auto tag_elem = elem.firstChildElement("Tag");
  while (!tag_elem.isNull())
  {
    std::string name = tag_elem.attribute("name").toStdString();
    uint16_t type = tag_elem.attribute("type").toUShort();
    config_.selected_tags.push_back({ name, type });
    tag_elem = tag_elem.nextSiblingElement("Tag");
  }

  return true;
}

}  // namespace logix
