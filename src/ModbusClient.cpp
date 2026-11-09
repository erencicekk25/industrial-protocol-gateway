#include "ModbusClient.hpp"

#include "Logger.hpp"

#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ipg {
namespace {

constexpr std::string_view kComponent = "ModbusClient";

}  // namespace

ModbusClient::ModbusClient(ModbusClientConfig config) : config_(std::move(config)) {
    if (config_.responseTimeout.count() <= 0) {
        throw std::invalid_argument("responseTimeout must be positive");
    }
}

void ModbusClient::connect() {
    disconnect();
    net::TcpSocket socket = net::TcpSocket::connectTo(config_.host, config_.port);
    socket.setIoTimeout(config_.responseTimeout);
    socket_ = std::move(socket);  // commit only after the socket is fully configured
}

void ModbusClient::disconnect() noexcept { socket_.close(); }

std::vector<std::uint8_t> ModbusClient::buildReadHoldingRegistersRequest(std::uint16_t transactionId,
                                                                         std::uint8_t unitId,
                                                                         std::uint16_t startAddress,
                                                                         std::uint16_t quantity) {
    constexpr std::uint16_t kPduSize = 5;  // function(1) + start(2) + quantity(2)

    std::vector<std::uint8_t> frame;
    frame.reserve(modbus::kMbapHeaderSize + kPduSize);
    modbus::appendMbapHeader(frame, modbus::MbapHeader{transactionId, modbus::kProtocolIdentifier,
                                                       static_cast<std::uint16_t>(kPduSize + 1u), unitId});
    frame.push_back(modbus::kFunctionReadHoldingRegisters);
    bytes::appendUint16BigEndian(frame, startAddress);
    bytes::appendUint16BigEndian(frame, quantity);
    return frame;
}

void ModbusClient::validateResponseHeader(const modbus::MbapHeader& header, std::uint16_t expectedTransactionId,
                                          std::uint8_t expectedUnitId) {
    if (header.transactionId != expectedTransactionId) {
        throw modbus::ProtocolError("transaction id mismatch: expected " + std::to_string(expectedTransactionId) +
                                    ", got " + std::to_string(header.transactionId));
    }
    if (header.protocolId != modbus::kProtocolIdentifier) {
        throw modbus::ProtocolError("unexpected protocol id " + std::to_string(header.protocolId));
    }
    if (header.unitId != expectedUnitId) {
        throw modbus::ProtocolError("unit id mismatch: expected " + std::to_string(expectedUnitId) + ", got " +
                                    std::to_string(header.unitId));
    }
    if (header.length < modbus::kMinMbapLength || header.length > modbus::kMaxMbapLength) {
        throw modbus::ProtocolError("invalid MBAP length " + std::to_string(header.length));
    }
}

std::vector<std::uint16_t> ModbusClient::parseReadHoldingRegistersResponse(const std::vector<std::uint8_t>& pdu,
                                                                           std::uint16_t expectedQuantity) {
    if (pdu.empty()) {
        throw modbus::ProtocolError("empty response PDU");
    }

    const std::uint8_t functionCode = pdu[0];
    if (functionCode == (modbus::kFunctionReadHoldingRegisters | modbus::kExceptionFlag)) {
        if (pdu.size() != 2) {
            throw modbus::ProtocolError("malformed exception response");
        }
        throw modbus::ModbusException(modbus::kFunctionReadHoldingRegisters, pdu[1]);
    }
    if (functionCode != modbus::kFunctionReadHoldingRegisters) {
        throw modbus::ProtocolError("unexpected function code 0x" + bytes::toHexByte(functionCode));
    }
    if (pdu.size() < 2) {
        throw modbus::ProtocolError("response PDU too short");
    }

    const std::size_t byteCount = pdu[1];
    const std::size_t expectedByteCount = static_cast<std::size_t>(expectedQuantity) * 2u;
    if (byteCount != expectedByteCount || pdu.size() != 2 + byteCount) {
        throw modbus::ProtocolError("byte count mismatch: expected " + std::to_string(expectedByteCount) +
                                    ", header says " + std::to_string(byteCount) + ", PDU carries " +
                                    std::to_string(pdu.size() - 2));
    }

    std::vector<std::uint16_t> values;
    values.reserve(expectedQuantity);
    for (std::size_t offset = 2; offset < pdu.size(); offset += 2) {
        values.push_back(bytes::readUint16BigEndian(&pdu[offset]));
    }
    return values;
}

std::vector<std::uint16_t> ModbusClient::readHoldingRegisters(std::uint8_t unitId, std::uint16_t startAddress,
                                                              std::uint16_t quantity) {
    if (quantity == 0 || quantity > modbus::kMaxReadRegisterCount) {
        throw std::invalid_argument("quantity must be between 1 and " +
                                    std::to_string(modbus::kMaxReadRegisterCount));
    }
    if (static_cast<std::uint32_t>(startAddress) + quantity > 0x10000u) {
        throw std::invalid_argument("register range exceeds the 16-bit address space");
    }
    if (!isConnected()) {
        throw net::NetworkError("Modbus client is not connected");
    }

    const std::uint16_t transactionId = nextTransactionId_++;
    const std::vector<std::uint8_t> request =
        buildReadHoldingRegistersRequest(transactionId, unitId, startAddress, quantity);

    try {
        if (Logger::isEnabled(LogLevel::Debug)) {
            Logger::debug(kComponent, "TX " + bytes::toHexString(request));
        }
        socket_.sendAll(request.data(), request.size());

        std::array<std::uint8_t, modbus::kMbapHeaderSize> headerBytes{};
        socket_.receiveExact(headerBytes.data(), headerBytes.size());
        const modbus::MbapHeader header = modbus::parseMbapHeader(headerBytes.data());
        validateResponseHeader(header, transactionId, unitId);

        std::vector<std::uint8_t> pdu(header.length - 1u);
        socket_.receiveExact(pdu.data(), pdu.size());

        if (Logger::isEnabled(LogLevel::Debug)) {
            Logger::debug(kComponent, "RX " + bytes::toHexString(headerBytes.data(), headerBytes.size()) + " | " +
                                          bytes::toHexString(pdu));
        }
        return parseReadHoldingRegistersResponse(pdu, quantity);
    } catch (const modbus::ModbusException&) {
        throw;  // complete frame consumed: the stream is still in sync
    } catch (...) {
        disconnect();  // timeout, broken socket or garbage: start clean next time
        throw;
    }
}

}  // namespace ipg
