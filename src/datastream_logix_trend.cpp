#include "datastream_logix_trend.h"

#include <QDomDocument>
#include <QMessageBox>
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

    // Clear any data from previous session
    {
        std::lock_guard<std::mutex> lock(mutex());
        dataMap().numeric.clear();
        dataMap().strings.clear();
    }

    // Connect to PLC
    try {
        auto route = EipConnection::parseRouteString(config_.route);
        conn_ = std::make_unique<EipConnection>();
        conn_->connect(config_.ip_address, route);
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "Connection Error",
                              QString("Failed to connect:\n%1").arg(e.what()));
        emit closed();
        return false;
    }

    // Create one trend instance per tag
    trends_.clear();
    poll_interval_ms_ = 200;
    for (const auto& [tag_name, data_type] : config_.selected_tags) {
        try {
            auto trend = std::make_unique<TrendInstance>(*conn_, tag_name, data_type);
            int sample_size = trend->sampleSize();
            auto [buf_size, poll_ms] = computeBufferParams(config_.sample_rate_us, sample_size,
                                                            conn_->connectionSize());
            poll_interval_ms_ = std::min(poll_interval_ms_, poll_ms);
            trend->start(config_.sample_rate_us, buf_size);
            trends_.push_back(std::move(trend));
        } catch (const std::exception& e) {
            QMessageBox::critical(nullptr, "Trend Error",
                                  QString("Failed to start trend for '%1':\n%2")
                                      .arg(QString::fromStdString(tag_name))
                                      .arg(e.what()));
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
    base_timestamp_ = 0;
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
                        constexpr double kTickToSeconds = 128.0 / 1e6;
                        double time_s;
                        if (first_sample_) {
                            base_timestamp_ = sample.timestamp;
                            first_sample_ = false;
                            time_s = 0.0;
                        } else {
                            uint32_t delta;
                            if (sample.timestamp >= base_timestamp_) {
                                delta = sample.timestamp - base_timestamp_;
                            } else {
                                delta = (0xFFFFFFFF - base_timestamp_) +
                                        sample.timestamp + 1;
                            }
                            time_s = static_cast<double>(delta) * kTickToSeconds;
                        }

                        series.pushBack({time_s, sample.value});
                    }

                    got_data = true;
                }
            } catch (const std::exception&) {
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
    uint32_t sample_rate_us, int sample_size, uint32_t connection_size) {

    // Size the buffer to fit within a single CIP response.
    // Leave headroom for CIP headers (~20 bytes).
    uint32_t max_payload = connection_size > 20 ? connection_size - 20 : 480;
    uint32_t max_samples = max_payload / sample_size;
    uint32_t buffer_size = max_samples * sample_size;
    buffer_size = std::max(buffer_size, static_cast<uint32_t>(sample_size * 4));

    // Poll interval: drain before buffer fills (50% safety margin)
    uint32_t capacity = buffer_size / sample_size;
    double fill_time_ms = capacity * (sample_rate_us / 1000.0);
    uint32_t poll_ms = static_cast<uint32_t>(fill_time_ms / 2.0);

    // Clamp poll interval: min 20ms, max 200ms
    poll_ms = std::max(20u, std::min(200u, poll_ms));

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
