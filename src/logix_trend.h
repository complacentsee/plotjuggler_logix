#pragma once

#include "logix_eip.h"
#include <string>
#include <vector>

namespace logix {

constexpr uint16_t kTrendClass = 0xB2;

/// A single sample from a trend buffer
struct TrendSample {
    uint16_t count;      // Sample sequence number
    uint32_t timestamp;  // Microseconds (PLC wall clock)
    double value;        // Interpreted value (converted from raw bytes + type)
};

/// Manages a single CIP trend instance on the PLC (class 0xB2)
class TrendInstance {
public:
    TrendInstance(EipConnection& conn, const std::string& tag_name, uint16_t data_type);
    ~TrendInstance();

    TrendInstance(const TrendInstance&) = delete;
    TrendInstance& operator=(const TrendInstance&) = delete;

    /// Create the trend object on the PLC, configure rate, bind tag, and start
    /// sample_rate_us: sample period in microseconds
    /// buffer_size: PLC-side buffer allocation (default 4096)
    void start(uint32_t sample_rate_us, uint32_t buffer_size = 0x1000);

    /// Read available samples from the trend buffer
    std::vector<TrendSample> readData();

    /// Stop, unbind tag, and delete the trend instance
    void stop();

    bool isRunning() const { return running_; }
    const std::string& tagName() const { return tag_name_; }
    uint16_t dataType() const { return data_type_; }
    uint32_t instanceId() const { return instance_id_; }

    /// Size of one sample record in bytes: 6 (count + timestamp) + value size
    int sampleSize() const { return 6 + cipTypeSize(data_type_); }

private:
    uint8_t createTrend(uint32_t buffer_size);
    uint8_t setAttributes(uint32_t sample_rate_us, uint8_t state);
    uint8_t addTag();
    uint8_t startTrend();
    uint8_t stopTrend();
    uint8_t removeTag();
    uint8_t deleteTrend();

    /// Interpret raw 4/8-byte sample value according to data type
    double interpretValue(const uint8_t* raw_bytes) const;

    EipConnection& conn_;
    std::string tag_name_;
    uint16_t data_type_;
    uint32_t instance_id_ = 0;
    bool running_ = false;
};

} // namespace logix
