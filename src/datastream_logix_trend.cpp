#include "datastream_logix_trend.h"

#include <QDomDocument>
#include <QMessageBox>
#include <QDebug>
#include <chrono>
#include <cmath>

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

    // Create one trend instance per tag (PLC limitation: 1 tag per trend in high-speed mode)
    trends_.clear();
    trend_time_.clear();
    start_time_ = std::chrono::steady_clock::now();
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

    // Timestamp tracking initialized per-trend on first sample

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
    constexpr double kTickToSeconds = 128.0 / 1e6;
    int poll_count = 0;

    while (running_) {
        bool got_data = false;
        auto cycle_start = std::chrono::steady_clock::now();
        double host_now_s = std::chrono::duration<double>(
            cycle_start - start_time_).count();

        for (size_t idx = 0; idx < trends_.size(); idx++) {
            auto& trend = trends_[idx];
            try {
                auto samples = trend->readData();

                if (!samples.empty()) {
                    std::lock_guard<std::mutex> lock(mutex());

                    auto& series = dataMap().getOrCreateNumeric(trend->tagName());
                    auto& ts = trend_time_[trend->tagName()];

                    int accepted = 0;
                    int skipped = 0;

                    for (const auto& sample : samples) {
                        if (!ts.initialized) {
                            // Anchor this trend: PLC timestamp → host time
                            ts.base_plc_ts = sample.timestamp;
                            ts.base_host_s = host_now_s;
                            ts.initialized = true;
                        }

                        // Compute time relative to this trend's base,
                        // then offset to host time axis
                        uint32_t delta;
                        if (sample.timestamp >= ts.base_plc_ts) {
                            delta = sample.timestamp - ts.base_plc_ts;
                        } else {
                            delta = (0xFFFFFFFF - ts.base_plc_ts) +
                                    sample.timestamp + 1;
                        }
                        double time_s = ts.base_host_s +
                                        static_cast<double>(delta) * kTickToSeconds;

                        // Enforce monotonic timestamps — skip FIFO overflow artifacts
                        if (time_s > ts.last_time_s) {
                            series.pushBack({time_s, sample.value});
                            ts.last_time_s = time_s;
                            accepted++;
                        } else {
                            skipped++;
                        }
                    }

                    if (poll_count < 3) {
                        qDebug() << "Poll" << poll_count << "trend" << idx
                                 << QString::fromStdString(trend->tagName())
                                 << "samples=" << samples.size()
                                 << "accepted=" << accepted
                                 << "skipped=" << skipped
                                 << "last_t=" << ts.last_time_s;
                    }

                    got_data = true;
                } else if (poll_count < 3) {
                    qDebug() << "Poll" << poll_count << "trend" << idx
                             << QString::fromStdString(trend->tagName())
                             << "samples=0 (empty)";
                }
            } catch (const std::exception&) {
                running_ = false;
                emit closed();
                return;
            }
        }

        if (poll_count < 3) {
            auto cycle_end = std::chrono::steady_clock::now();
            auto cycle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                cycle_end - cycle_start).count();
            qDebug() << "Poll" << poll_count << "cycle_time=" << cycle_ms << "ms"
                     << "poll_interval=" << poll_interval_ms_ << "ms";
        }
        poll_count++;

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
