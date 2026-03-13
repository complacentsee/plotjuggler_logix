#pragma once

#include "logix_config_dialog.h"
#include "logix_eip.h"
#include "logix_trend.h"

#include <PlotJuggler/datastreamer_base.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace logix {

class DataStreamLogixTrend : public PJ::DataStreamer {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "facontidavide.PlotJuggler3.DataStreamer")
    Q_INTERFACES(PJ::DataStreamer)

public:
    DataStreamLogixTrend();
    ~DataStreamLogixTrend() override;

    const char* name() const override { return "Logix (CIP 0xB2)"; }

    bool start(QStringList* selected_datasources) override;
    void shutdown() override;
    bool isRunning() const override;

    bool xmlSaveState(QDomDocument& doc, QDomElement& parent_element) const override;
    bool xmlLoadState(const QDomElement& parent_element) override;

private:
    void pollingLoop();

    // Compute buffer size and poll interval to prevent overflow.
    // sample_size = bytes per sample record (6 + value size).
    // Returns {buffer_size_bytes, poll_interval_ms}.
    static std::pair<uint32_t, uint32_t> computeBufferParams(uint32_t sample_rate_us,
                                                              int sample_size);

    std::atomic<bool> running_{false};
    uint32_t poll_interval_ms_ = 200;
    std::thread poll_thread_;

    std::unique_ptr<EipConnection> conn_;
    std::vector<std::unique_ptr<TrendInstance>> trends_;

    LogixConfig config_;

    // Timestamp tracking: we convert PLC microsecond timestamps to
    // a monotonic seconds value for PlotJuggler
    bool first_sample_ = true;
    uint32_t base_timestamp_us_ = 0;
    double time_offset_s_ = 0.0;
};

} // namespace logix
