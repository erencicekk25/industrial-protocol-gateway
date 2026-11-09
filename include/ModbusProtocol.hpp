#pragma once

#include "ByteUtils.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/// Shared Modbus TCP definitions used by both the virtual server and the client.
///
/// A Modbus TCP frame (ADU) is an MBAP header followed by a PDU:
///
///   +----------------+-------------+--------+---------+---------------------+
///   | Transaction ID | Protocol ID | Length | Unit ID | PDU (function+data) |
///   |    2 bytes     |   2 bytes   | 2 bytes| 1 byte  |   up to 253 bytes   |
///   +----------------+-------------+--------+---------+---------------------+
///
/// "Length" counts the bytes that follow it: Unit ID + PDU.
/// All multi-byte fields are big-endian.
namespace ipg::modbus {

/// 502 is the IANA port, but it is privileged on POSIX systems. 5020 is the
/// common choice for simulators and lets the project run without root.
inline constexpr std::uint16_t kDefaultTcpPort = 5020;

inline constexpr std::size_t kMbapHeaderSize = 7;
inline constexpr std::uint16_t kProtocolIdentifier = 0x0000;
inline constexpr std::size_t kMaxPduSize = 253;
inline constexpr std::uint16_t kMinMbapLength = 2;  // unit id + function code
inline constexpr std::uint16_t kMaxMbapLength = static_cast<std::uint16_t>(1 + kMaxPduSize);
inline constexpr std::uint16_t kMaxReadRegisterCount = 125;  // 125 * 2 bytes + 2 fits in 253

inline constexpr std::uint8_t kFunctionReadHoldingRegisters = 0x03;
inline constexpr std::uint8_t kExceptionFlag = 0x80;

enum class ExceptionCode : std::uint8_t {
    IllegalFunction = 0x01,
    IllegalDataAddress = 0x02,
    IllegalDataValue = 0x03,
    ServerDeviceFailure = 0x04,
};

[[nodiscard]] constexpr std::string_view describeException(std::uint8_t code) noexcept {
    switch (code) {
        case 0x01:
            return "illegal function";
        case 0x02:
            return "illegal data address";
        case 0x03:
            return "illegal data value";
        case 0x04:
            return "server device failure";
        case 0x06:
            return "server device busy";
        case 0x0A:
            return "gateway path unavailable";
        case 0x0B:
            return "gateway target failed to respond";
        default:
            return "unknown exception";
    }
}

struct MbapHeader {
    std::uint16_t transactionId = 0;
    std::uint16_t protocolId = kProtocolIdentifier;
    std::uint16_t length = 0;
    std::uint8_t unitId = 0;
};

inline void appendMbapHeader(std::vector<std::uint8_t>& out, const MbapHeader& header) {
    bytes::appendUint16BigEndian(out, header.transactionId);
    bytes::appendUint16BigEndian(out, header.protocolId);
    bytes::appendUint16BigEndian(out, header.length);
    out.push_back(header.unitId);
}

/// Decodes the 7 header bytes. The caller guarantees kMbapHeaderSize bytes.
[[nodiscard]] constexpr MbapHeader parseMbapHeader(const std::uint8_t* data) noexcept {
    return MbapHeader{bytes::readUint16BigEndian(data), bytes::readUint16BigEndian(data + 2),
                      bytes::readUint16BigEndian(data + 4), data[6]};
}

/// The frame on the wire was malformed or did not match the request.
/// After a ProtocolError the TCP stream must be considered out of sync.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The device understood the request but answered with a Modbus exception
/// response. The connection itself is still healthy.
class ModbusException : public std::runtime_error {
public:
    ModbusException(std::uint8_t functionCode, std::uint8_t exceptionCode)
        : std::runtime_error("Modbus exception 0x" + bytes::toHexByte(exceptionCode) + " (" +
                             std::string(describeException(exceptionCode)) + ") for function 0x" +
                             bytes::toHexByte(functionCode)),
          functionCode_(functionCode),
          exceptionCode_(exceptionCode) {}

    [[nodiscard]] std::uint8_t functionCode() const noexcept { return functionCode_; }
    [[nodiscard]] std::uint8_t exceptionCode() const noexcept { return exceptionCode_; }

private:
    std::uint8_t functionCode_;
    std::uint8_t exceptionCode_;
};

}  // namespace ipg::modbus
