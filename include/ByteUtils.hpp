#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ipg::bytes {

/// Appends a 16-bit value in network byte order (big-endian), which is what
/// both Modbus and MQTT use on the wire.
inline void appendUint16BigEndian(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
}

/// Reads a big-endian 16-bit value. The caller guarantees two readable bytes.
[[nodiscard]] constexpr std::uint16_t readUint16BigEndian(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<unsigned>(data[0]) << 8) | static_cast<unsigned>(data[1]));
}

/// Formats a single byte as two upper-case hex digits, e.g. 0x2F -> "2F".
[[nodiscard]] inline std::string toHexByte(std::uint8_t value) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    return {kDigits[value >> 4], kDigits[value & 0x0F]};
}

/// Formats a byte buffer as space-separated hex, e.g. "00 01 00 00 00 06".
[[nodiscard]] inline std::string toHexString(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.reserve(size * 3);
    for (std::size_t i = 0; i < size; ++i) {
        if (i != 0) {
            out.push_back(' ');
        }
        out += toHexByte(data[i]);
    }
    return out;
}

[[nodiscard]] inline std::string toHexString(const std::vector<std::uint8_t>& data) {
    return toHexString(data.data(), data.size());
}

}  // namespace ipg::bytes
