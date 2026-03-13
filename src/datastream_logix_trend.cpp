#include "datastream_logix_trend.h"

#include <QDomDocument>
#include <chrono>

namespace logix {

DataStreamLogixTrend::DataStreamLogixTrend() = default;

DataStreamLogixTrend::~DataStreamLogixTrend() {
    shutdown();
}

bool DataStreamLogixTrend::start(QStringList* selected_datasources) {
    if (running_) return false;

    // Show configuration dialog
    LogixConfigDialog dialog(nullptr, config_.ip_address.empty() ? nullptr : &config_);
    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    config_ = dialog.getConfig();

    // Connect to PLC
    try {
        auto route = EipConnection::parseRouteString(config_.route);
        conn_ = std::make_unique<EipConnection>();
        conn_->connect(config_.ip_address, route);
    } catch (const std::exception& e) {
        emit closed();
        return false;
    }

    // Create trend instances for each selected tag with appropriate buffer sizing
    trends_.clear();
    poll_interval_ms_ = 500; // will be reduced based on fastest-filling trend
    for (const auto& [tag_name, data_type] : config_.selected_tags) {
        try {
            auto trend = std::make_unique<TrendInstance>(*conn_, tag_name, data_type);
            int sample_size = trend->sampleSize();
            auto [buf_size, poll_ms] = computeBufferParams(config_.sample_rate_us, sample_size);
            poll_interval_ms_ = std::min(poll_interval_ms_, poll_ms);
            trend->start(config_.sample_rate_us, buf_size);
            trends_.push_back(std::move(trend));
        } catch (const std::exception& e) {
            // Clean up already-started trends
            trends_.clear();
            conn_->close();
            conn_.reset();
            emit closed();
            return false;
        }
    }

    if (trends_.empty()) {
        conn_->close();
        conn_.reset();
        return false;
    }

    // Populate selected_datasources for PlotJuggler
    if (selected_datasources) {
        for (const auto& trend : trends_) {
            selected_datasources->append(QString::fromStdString(trend->tagName()));
        }
    }

    // Reset timestamp tracking
    first_sample_ = true;
    base_timestamp_us_ = 0;
    time_offset_s_ = 0.0;

    // Start polling thread
    running_ = true;
    poll_thread_ = std::thread(&DataStreamLogixTrend::pollingLoop, this);

    return true;
}

void DataStreamLogixTrend::shutdown() {
    if (!running_) return;

    running_ = false;
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }

    // Stop all trend instances (stop → remove tag → delete)
    trends_.clear();

    // Close connection
    if (conn_) {
        conn_->close();
        conn_.reset();
    }
}

bool DataStreamLogixTrend::isRunning() const {
    return running_;
}

void DataStreamLogixTrend::pollingLoop() {
    auto poll_interval = std::chrono::milliseconds(poll_interval_ms_);

    while (running_) {
        bool got_data = false;

        for (auto& trend : trends_) {
            try {
                auto samples = trend->readData();

                if (!samples.empty()) {
                    std::lock_guard<std::mutex> lock(mutex());

                    auto& series = dataMap().getOrCreateNumeric(trend->tagName());

                    for (const auto& sample : samples) {
                        // Convert PLC timestamp to seconds
                        double time_s;
                        if (first_sample_) {
                            base_timestamp_us_ = sample.timestamp;
                            first_sample_ = false;
                            time_s = 0.0;
                        } else {
                            // Handle timestamp wraparound (uint32 microseconds)
                            uint32_t delta_us;
                            if (sample.timestamp >= base_timestamp_us_) {
                                delta_us = sample.timestamp - base_timestamp_us_;
                            } else {
                                // Wraparound
                                delta_us = (0xFFFFFFFF - base_timestamp_us_) +
                                           sample.timestamp + 1;
                            }
                            time_s = static_cast<double>(delta_us) / 1e6;
                        }

                        series.pushBack({time_s, sample.value});
                    }

                    got_data = true;
                }
            } catch (const std::exception&) {
                // Connection lost or read error
                running_ = false;
                emit closed();
                return;
            }
        }

        if (got_data) {
            emit dataReceived();
        }

        std::this_thread::sleep_for(poll_interval);
    }
}

// ─── Buffer Sizing ──────────────────────────────────────────────────────────

std::pair<uint32_t, uint32_t> DataStreamLogixTrend::computeBufferParams(
    uint32_t sample_rate_us, int sample_size) {

    // Target: buffer holds at least 2 seconds of samples
    double samples_per_sec = 1e6 / static_cast<double>(sample_rate_us);
    uint32_t target_samples = static_cast<uint32_t>(samples_per_sec * 2.0);
    uint32_t target_bytes = target_samples * sample_size;

    // Clamp buffer: min 4096, max 65536
    uint32_t buffer_size = std::max(4096u, std::min(65536u, target_bytes));

    // Compute poll interval: drain at ~50% capacity for safety margin
    uint32_t capacity = buffer_size / sample_size;
    double fill_time_ms = capacity * (sample_rate_us / 1000.0);
    uint32_t poll_ms = static_cast<uint32_t>(fill_time_ms / 2.0);

    // Clamp poll interval: min 50ms, max 500ms
    poll_ms = std::max(50u, std::min(500u, poll_ms));

    return {buffer_size, poll_ms};
}

// ─── XML State Persistence ──────────────────────────────────────────────────

bool DataStreamLogixTrend::xmlSaveState(QDomDocument& doc, QDomElement& parent) const {
    auto elem = doc.createElement("LogixTrend");

    elem.setAttribute("ip", QString::fromStdString(config_.ip_address));
    elem.setAttribute("route", QString::fromStdString(config_.route));
    elem.setAttribute("sample_rate_us", static_cast<int>(config_.sample_rate_us));

    for (const auto& [name, type] : config_.selected_tags) {
        auto tag_elem = doc.createElement("Tag");
        tag_elem.setAttribute("name", QString::fromStdString(name));
        tag_elem.setAttribute("type", type);
        elem.appendChild(tag_elem);
    }

    parent.appendChild(elem);
    return true;
}

bool DataStreamLogixTrend::xmlLoadState(const QDomElement& parent) {
    auto elem = parent.firstChildElement("LogixTrend");
    if (elem.isNull()) return false;

    config_.ip_address = elem.attribute("ip").toStdString();
    config_.route = elem.attribute("route").toStdString();
    config_.sample_rate_us = elem.attribute("sample_rate_us", "10000").toUInt();

    config_.selected_tags.clear();
    auto tag_elem = elem.firstChildElement("Tag");
    while (!tag_elem.isNull()) {
        std::string name = tag_elem.attribute("name").toStdString();
        uint16_t type = tag_elem.attribute("type").toUShort();
        config_.selected_tags.push_back({name, type});
        tag_elem = tag_elem.nextSiblingElement("Tag");
    }

    return true;
}

} // namespace logix
