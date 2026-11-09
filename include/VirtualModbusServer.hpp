#pragma once

#include "ModbusProtocol.hpp"
#include "RegisterMap.hpp"
#include "TcpSocket.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace ipg {

struct ServerConfig {
    std::string bindAddress = "127.0.0.1";
    std::uint16_t port = modbus::kDefaultTcpPort;  ///< 0 = let the OS pick a free port
    std::chrono::milliseconds simulationInterval{200};
    std::chrono::milliseconds clientIoTimeout{1000};
    std::size_t maxClients = 8;
    std::optional<std::uint32_t> randomSeed;  ///< fixed seed => reproducible signal noise
};

/// A mock Modbus TCP field device running on its own thread.
///
/// The worker thread multiplexes the listening socket and all connected
/// clients with select(), and between network events advances a small
/// physical model of a compressor motor (load, thermal lag, supply voltage).
/// Only Function 0x03 (Read Holding Registers) is implemented; any other
/// function code receives a proper "illegal function" exception response.
///
/// Lifetime (RAII): start() binds and launches the thread, stop() or the
/// destructor joins it. Sockets are owned by value and close automatically.
class VirtualModbusServer {
public:
    explicit VirtualModbusServer(ServerConfig config);
    ~VirtualModbusServer();

    VirtualModbusServer(const VirtualModbusServer&) = delete;
    VirtualModbusServer& operator=(const VirtualModbusServer&) = delete;
    VirtualModbusServer(VirtualModbusServer&&) = delete;
    VirtualModbusServer& operator=(VirtualModbusServer&&) = delete;

    /// Binds synchronously, so "port already in use" surfaces to the caller
    /// as an exception instead of dying silently on the worker thread.
    void start();
    void stop() noexcept;

    [[nodiscard]] bool isRunning() const noexcept { return running_.load(); }
    [[nodiscard]] std::uint16_t port() const noexcept { return boundPort_; }
    [[nodiscard]] std::uint64_t requestsServed() const noexcept { return requestsServed_.load(); }

    /// Thread-safe copy of the current register bank (useful for tests/diagnostics).
    [[nodiscard]] std::vector<std::uint16_t> snapshotRegisters() const;

private:
    struct ClientSession {
        net::TcpSocket socket;
        std::string peer;
    };

    void serveLoop() noexcept;
    void acceptClient();
    [[nodiscard]] bool serveRequest(ClientSession& session);
    [[nodiscard]] std::vector<std::uint8_t> processPdu(const std::vector<std::uint8_t>& pdu) const;
    void updateSimulation();

    // Declaration order matters: the network context must outlive every socket.
    const ServerConfig config_;
    net::NetworkContext networkContext_;
    net::TcpSocket listener_;
    std::vector<ClientSession> clients_;  // owned exclusively by the worker thread

    mutable std::mutex registerMutex_;
    std::array<std::uint16_t, registers::kRegisterCount> registers_{};

    // Simulation state (worker thread only after start()).
    std::mt19937 rng_;
    std::chrono::steady_clock::time_point startTime_{};
    double housingTemperature_ = 42.0;
    int surgeTicksRemaining_ = 0;
    std::uint16_t heartbeat_ = 0;

    std::uint16_t boundPort_ = 0;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> requestsServed_{0};
    std::thread worker_;  // joined in the destructor body, before any member is destroyed
};

}  // namespace ipg
