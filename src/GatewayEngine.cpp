#include "GatewayEngine.hpp"

#include "Logger.hpp"
#include "RegisterMap.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace ipg {
namespace {

constexpr std::string_view kComponent = "Gateway";

std::string endpointOf(const ModbusClientConfig& config) {
    return config.host + ":" + std::to_string(config.port);
}

}  // namespace

GatewayEngine::GatewayEngine(GatewayConfig config, MqttPublisher publisher)
    : config_(std::move(config)), client_(config_.modbus), publisher_(std::move(publisher)) {
    if (config_.pollInterval.count() <= 0 || config_.initialBackoff.count() <= 0 ||
        config_.maxBackoff < config_.initialBackoff) {
        throw std::invalid_argument("invalid gateway timing configuration");
    }
}

GatewayEngine::~GatewayEngine() { stop(); }

void GatewayEngine::start() {
    if (worker_.joinable()) {
        throw std::logic_error("GatewayEngine is already running");
    }
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        stopRequested_ = false;
    }
    running_.store(true);
    try {
        worker_ = std::thread(&GatewayEngine::runLoop, this);
    } catch (...) {
        running_.store(false);
        throw;
    }
}

void GatewayEngine::stop() noexcept {
    try {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        stopRequested_ = true;
    } catch (const std::system_error&) {
        // Locking an uncontended std::mutex does not fail in practice.
    }
    wakeCondition_.notify_all();

    if (worker_.joinable()) {
        try {
            worker_.join();
        } catch (const std::system_error&) {
        }
    }
    running_.store(false);
}

GatewayStatistics GatewayEngine::statistics() const noexcept {
    GatewayStatistics stats;
    stats.pollCycles = pollCycles_.load();
    stats.successfulReads = successfulReads_.load();
    stats.failedReads = failedReads_.load();
    stats.telemetryPublished = telemetryPublished_.load();
    stats.reconnects = reconnects_.load();
    return stats;
}

bool GatewayEngine::waitUntil(std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    const bool stopping = wakeCondition_.wait_until(lock, deadline, [this] { return stopRequested_; });
    return !stopping;
}

void GatewayEngine::runLoop() noexcept {
    try {
        Logger::info(kComponent, "polling unit " + std::to_string(config_.unitId) + " at " +
                                     endpointOf(config_.modbus) + " every " +
                                     std::to_string(config_.pollInterval.count()) + " ms");

        auto backoff = config_.initialBackoff;
        auto nextPoll = std::chrono::steady_clock::now();

        while (waitUntil(nextPoll)) {
            if (executeCycle()) {
                backoff = config_.initialBackoff;
                nextPoll += config_.pollInterval;
                const auto now = std::chrono::steady_clock::now();
                if (nextPoll < now) {
                    nextPoll = now;  // overran the interval: skip, never burst to catch up
                }
            } else {
                Logger::warning(kComponent, "retrying in " + std::to_string(backoff.count()) + " ms");
                nextPoll = std::chrono::steady_clock::now() + backoff;
                backoff = std::min(backoff * 2, config_.maxBackoff);
            }
        }
    } catch (const std::exception& e) {
        Logger::error(kComponent, std::string("engine loop terminated: ") + e.what());
    } catch (...) {
        Logger::error(kComponent, "engine loop terminated by an unknown error");
    }

    client_.disconnect();
    if (deviceOnline_) {
        setAvailability(false);
    }
    Logger::info(kComponent, "engine stopped");
    running_.store(false);
}

void GatewayEngine::ensureConnected() {
    if (client_.isConnected()) {
        return;
    }
    Logger::info(kComponent, "connecting to Modbus device at " + endpointOf(config_.modbus) + " ...");
    client_.connect();
    if (everConnected_) {
        reconnects_.fetch_add(1);
    }
    everConnected_ = true;
    Logger::info(kComponent, "connected to Modbus device");
}

bool GatewayEngine::executeCycle() {
    const std::uint64_t cycle = pollCycles_.fetch_add(1) + 1;
    const std::string prefix = "cycle #" + std::to_string(cycle) + ": ";

    try {
        ensureConnected();
        if (!deviceOnline_) {
            setAvailability(true);  // birth message once the southbound link is up
        }

        const auto started = std::chrono::steady_clock::now();
        const std::vector<std::uint16_t> values =
            client_.readHoldingRegisters(config_.unitId, registers::kBlockStart, registers::kRegisterCount);
        const auto latency =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started);
        successfulReads_.fetch_add(1);

        Logger::debug(kComponent, prefix + "read " + std::to_string(values.size()) + " registers in " +
                                      std::to_string(latency.count()) + " us");

        publisher_.publishTelemetry(values, config_.unitId);
        telemetryPublished_.fetch_add(1);
        return true;
    } catch (const modbus::ModbusException& e) {
        Logger::warning(kComponent, prefix + "device rejected the request: " + e.what());
    } catch (const modbus::ProtocolError& e) {
        Logger::warning(kComponent, prefix + "protocol error: " + e.what());
    } catch (const net::NetworkError& e) {
        Logger::warning(kComponent, prefix + "link error: " + e.what());
        if (deviceOnline_) {
            setAvailability(false);  // northbound consumers learn the device is unreachable
        }
    } catch (const std::exception& e) {
        Logger::error(kComponent, prefix + "unexpected error: " + e.what());
    }

    failedReads_.fetch_add(1);
    return false;
}

void GatewayEngine::setAvailability(bool online) noexcept {
    try {
        publisher_.publishAvailability(online);
        deviceOnline_ = online;
    } catch (const std::exception& e) {
        Logger::error(kComponent, std::string("failed to publish availability: ") + e.what());
    }
}

}  // namespace ipg
