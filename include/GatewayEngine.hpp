#pragma once

#include "ModbusClient.hpp"
#include "MqttPublisher.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace ipg {

struct GatewayConfig {
    ModbusClientConfig modbus;
    std::uint8_t unitId = 1;
    std::chrono::milliseconds pollInterval{1000};
    std::chrono::milliseconds initialBackoff{500};
    std::chrono::milliseconds maxBackoff{8000};
};

struct GatewayStatistics {
    std::uint64_t pollCycles = 0;
    std::uint64_t successfulReads = 0;
    std::uint64_t failedReads = 0;
    std::uint64_t telemetryPublished = 0;
    std::uint64_t reconnects = 0;
};

/// The orchestrator: owns the southbound Modbus client and the northbound
/// MQTT publisher, and runs the poll -> decode -> publish loop on its own
/// thread.
///
/// Scheduling is fixed-rate (deadline += interval), so polling does not drift
/// over time. On failure it switches to exponential backoff and reconnects
/// transparently, publishing a retained "offline"/"online" availability
/// message whenever the device link changes state.
class GatewayEngine {
public:
    GatewayEngine(GatewayConfig config, MqttPublisher publisher);
    ~GatewayEngine();

    GatewayEngine(const GatewayEngine&) = delete;
    GatewayEngine& operator=(const GatewayEngine&) = delete;
    GatewayEngine(GatewayEngine&&) = delete;
    GatewayEngine& operator=(GatewayEngine&&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] bool isRunning() const noexcept { return running_.load(); }
    [[nodiscard]] GatewayStatistics statistics() const noexcept;

private:
    void runLoop() noexcept;
    [[nodiscard]] bool executeCycle();
    void ensureConnected();
    void setAvailability(bool online) noexcept;

    /// Sleeps until the deadline or until stop() is called.
    /// Returns false when the engine should shut down.
    [[nodiscard]] bool waitUntil(std::chrono::steady_clock::time_point deadline);

    const GatewayConfig config_;
    ModbusClient client_;       // worker thread only
    MqttPublisher publisher_;   // worker thread only
    bool everConnected_ = false;
    bool deviceOnline_ = false;

    std::mutex stateMutex_;
    std::condition_variable wakeCondition_;
    bool stopRequested_ = false;  // guarded by stateMutex_

    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> pollCycles_{0};
    std::atomic<std::uint64_t> successfulReads_{0};
    std::atomic<std::uint64_t> failedReads_{0};
    std::atomic<std::uint64_t> telemetryPublished_{0};
    std::atomic<std::uint64_t> reconnects_{0};

    std::thread worker_;
};

}  // namespace ipg
