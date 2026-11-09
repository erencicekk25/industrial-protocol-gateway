#pragma once

#include "ModbusProtocol.hpp"
#include "TcpSocket.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ipg {

struct ModbusClientConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = modbus::kDefaultTcpPort;
    std::chrono::milliseconds responseTimeout{1000};
};

/// Modbus TCP master (client) for Function 0x03, Read Holding Registers.
///
/// Error model:
///  - modbus::ModbusException : the device answered with an exception
///                              response; the connection stays open.
///  - modbus::ProtocolError   : malformed or mismatched frame; the client
///                              disconnects because the stream is out of sync.
///  - net::NetworkError       : socket failure or timeout; the client
///                              disconnects so the caller can reconnect.
///
/// Not thread-safe: one client instance belongs to one polling thread,
/// which mirrors how a Modbus master serialises its requests on a link.
class ModbusClient {
public:
    explicit ModbusClient(ModbusClientConfig config);

    ModbusClient(const ModbusClient&) = delete;
    ModbusClient& operator=(const ModbusClient&) = delete;
    ModbusClient(ModbusClient&&) = delete;
    ModbusClient& operator=(ModbusClient&&) = delete;
    ~ModbusClient() = default;

    void connect();
    void disconnect() noexcept;
    [[nodiscard]] bool isConnected() const noexcept { return socket_.isValid(); }
    [[nodiscard]] const ModbusClientConfig& config() const noexcept { return config_; }

    [[nodiscard]] std::vector<std::uint16_t> readHoldingRegisters(std::uint8_t unitId, std::uint16_t startAddress,
                                                                  std::uint16_t quantity);

    /// Builds the complete ADU for a Read Holding Registers request.
    /// Exposed as a pure function so the wire format can be unit tested.
    [[nodiscard]] static std::vector<std::uint8_t> buildReadHoldingRegistersRequest(std::uint16_t transactionId,
                                                                                    std::uint8_t unitId,
                                                                                    std::uint16_t startAddress,
                                                                                    std::uint16_t quantity);

    /// Decodes the PDU of a Read Holding Registers response.
    [[nodiscard]] static std::vector<std::uint16_t> parseReadHoldingRegistersResponse(
        const std::vector<std::uint8_t>& pdu, std::uint16_t expectedQuantity);

private:
    static void validateResponseHeader(const modbus::MbapHeader& header, std::uint16_t expectedTransactionId,
                                       std::uint8_t expectedUnitId);

    const ModbusClientConfig config_;
    net::NetworkContext networkContext_;  // must be declared before socket_
    net::TcpSocket socket_;
    std::uint16_t nextTransactionId_ = 1;
};

}  // namespace ipg
