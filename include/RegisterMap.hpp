#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

/// Register map of the simulated device: an industrial air compressor
/// motor with a small embedded controller.
///
/// This is the "contract" between the field device and the gateway. In a
/// real project it comes from the device vendor's manual; both the virtual
/// server (encoder) and the gateway (decoder) include this single header so
/// they can never disagree.
///
///   Addr | Name             | Type   | Scale | Unit
///   -----+------------------+--------+-------+--------------------------
///    0   | Temperature      | INT16  | 0.1   | degC (motor housing)
///    1   | Humidity         | UINT16 | 0.1   | %RH (ambient)
///    2   | Power (high word)| UINT32 | 0.1   | W   (big-endian word order)
///    3   | Power (low word) |        |       |
///    4   | Voltage          | UINT16 | 0.1   | V   (line-to-line)
///    5   | Status word      | BITS   | -     | see status::k* masks
///    6   | Heartbeat        | UINT16 | 1     | rolling counter
///    7   | Firmware version | UINT16 | -     | major << 8 | minor
namespace ipg::registers {

inline constexpr std::uint16_t kBlockStart = 0;

inline constexpr std::uint16_t kTemperature = 0;
inline constexpr std::uint16_t kHumidity = 1;
inline constexpr std::uint16_t kPowerHigh = 2;
inline constexpr std::uint16_t kPowerLow = 3;
inline constexpr std::uint16_t kVoltage = 4;
inline constexpr std::uint16_t kStatusWord = 5;
inline constexpr std::uint16_t kHeartbeat = 6;
inline constexpr std::uint16_t kFirmwareVersion = 7;

inline constexpr std::uint16_t kRegisterCount = 8;

inline constexpr double kTemperatureScale = 0.1;
inline constexpr double kHumidityScale = 0.1;
inline constexpr double kPowerScale = 0.1;
inline constexpr double kVoltageScale = 0.1;

namespace status {
inline constexpr std::uint16_t kRunning = 1u << 0;
inline constexpr std::uint16_t kOverTemperature = 1u << 1;
inline constexpr std::uint16_t kHighLoad = 1u << 2;
}  // namespace status

/// Engineering-unit view of one register block.
struct SensorReading {
    double temperatureCelsius = 0.0;
    double humidityPercent = 0.0;
    double powerWatts = 0.0;
    double voltageVolts = 0.0;
    std::uint16_t statusWord = 0;
    std::uint16_t heartbeat = 0;
    std::uint16_t firmwareVersion = 0;

    [[nodiscard]] bool hasStatus(std::uint16_t mask) const noexcept { return (statusWord & mask) != 0; }
};

/// Converts raw registers (starting at kBlockStart) into engineering units.
[[nodiscard]] inline SensorReading decode(const std::vector<std::uint16_t>& block) {
    if (block.size() < kRegisterCount) {
        throw std::invalid_argument("register block too short: expected " + std::to_string(kRegisterCount) +
                                    ", got " + std::to_string(block.size()));
    }

    SensorReading reading;
    // Reinterpreting the 16 bits as two's complement is how every PLC encodes INT16.
    reading.temperatureCelsius = static_cast<std::int16_t>(block[kTemperature]) * kTemperatureScale;
    reading.humidityPercent = block[kHumidity] * kHumidityScale;

    const std::uint32_t rawPower =
        (static_cast<std::uint32_t>(block[kPowerHigh]) << 16) | static_cast<std::uint32_t>(block[kPowerLow]);
    reading.powerWatts = rawPower * kPowerScale;

    reading.voltageVolts = block[kVoltage] * kVoltageScale;
    reading.statusWord = block[kStatusWord];
    reading.heartbeat = block[kHeartbeat];
    reading.firmwareVersion = block[kFirmwareVersion];
    return reading;
}

}  // namespace ipg::registers
