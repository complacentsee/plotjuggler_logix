/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#pragma once

#include "logix_config_dialog.h"
#include "logix_eip.h"
#include "logix_trend.h"

#include <PlotJuggler/datastreamer_base.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace logix
{

class DataStreamLogixTrend : public PJ::DataStreamer
{
  Q_OBJECT
  Q_PLUGIN_METADATA(IID "facontidavide.PlotJuggler3.DataStreamer")
  Q_INTERFACES(PJ::DataStreamer)

public:
  DataStreamLogixTrend();
  ~DataStreamLogixTrend() override;

  const char* name() const override
  {
    return "Logix (CIP 0xB2)";
  }

  bool start(QStringList* selected_datasources) override;
  void shutdown() override;
  bool isRunning() const override;

  bool xmlSaveState(QDomDocument& doc, QDomElement& parent_element) const override;
  bool xmlLoadState(const QDomElement& parent_element) override;

  /// Compute how many tags fit in one trend instance while ensuring
  /// the buffer doesn't overflow between polls.
  static size_t maxTagsPerInstance(uint32_t sample_rate_us, uint32_t connection_size,
                                   uint32_t min_fill_time_ms = 40);

private:
  void pollingLoop();

  // Compute buffer size and poll interval for a multi-tag trend instance.
  // Returns {buffer_size_bytes, poll_interval_ms}.
  static std::pair<uint32_t, uint32_t> computeBufferParams(uint32_t sample_rate_us, int sample_size,
                                                           size_t num_tags,
                                                           uint32_t connection_size);

  std::atomic<bool> running_{ false };
  uint32_t poll_interval_ms_ = 200;
  std::thread poll_thread_;

  std::unique_ptr<EipConnection> conn_;
  std::vector<std::unique_ptr<TrendInstance>> trends_;

  LogixConfig config_;
  std::vector<TagInfo> cached_tags_;
  TagBrowser cached_browser_;

  // Per-tag timestamp tracking. Each tag has its own PLC timestamp
  // epoch, so we anchor each to the host clock on first sample.
  struct TrendTimeState
  {
    bool initialized = false;
    uint32_t base_plc_ts = 0;   // PLC timestamp of first sample
    double base_host_s = 0.0;   // host time (seconds from start) of first read
    double last_time_s = -1.0;  // last emitted time for monotonicity
  };
  std::map<std::string, TrendTimeState> trend_time_;
  std::chrono::steady_clock::time_point start_time_;
};

}  // namespace logix
