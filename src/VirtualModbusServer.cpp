#include "VirtualModbusServer.hpp"

#include "Logger.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace ipg {
namespace {

constexpr std::string_view kComponent = "ModbusServer";
constexpr std::chrono::milliseconds kSelectSlice{50};  // upper bound on shutdown latency
constexpr double kTwoPi = 6.283185307179586;

// Physical model parameters for the simulated compressor motor.
constexpr double kNominalPowerWatts = 7500.0;
constexpr double kSurgePowerWatts = 1800.0;
constexpr double kNominalTemperatureCelsius = 42.0;
constexpr double kNominalVoltage = 400.0;
constexpr double kThermalLagFactor = 0.15;  // first-order low-pass per simulation tick
constexpr double kOverTemperatureThreshold = 50.0;
constexpr double kHighLoadThresholdWatts = 9500.0;
constexpr std::uint16_t kFirmwareVersion = 0x0102;  // v1.2

std::uint16_t encodeSigned16(double value, double scale) {
    const long scaled = std::lround(value / scale);
    const long clamped = std::clamp<long>(scaled, std::numeric_limits<std::int16_t>::min(),
                                          std::numeric_limits<std::int16_t>::max());
    // Two's complement bit pattern of the INT16 value.
    return static_cast<std::uint16_t>(static_cast<std::int16_t>(clamped));
}

std::uint16_t encodeUnsigned16(double value, double scale) {
    const long scaled = std::lround(value / scale);
    return static_cast<std::uint16_t>(std::clamp<long>(scaled, 0, std::numeric_limits<std::uint16_t>::max()));
}

std::uint32_t encodeUnsigned32(double value, double scale) {
    const long long scaled = std::llround(value / scale);
    return static_cast<std::uint32_t>(
        std::clamp<long long>(scaled, 0, static_cast<long long>(std::numeric_limits<std::uint32_t>::max())));
}

std::vector<std::uint8_t> exceptionPdu(std::uint8_t functionCode, modbus::ExceptionCode code) {
    return {static_cast<std::uint8_t>(functionCode | modbus::kExceptionFlag), static_cast<std::uint8_t>(code)};
}

std::uint32_t makeSeed(const std::optional<std::uint32_t>& seed) {
    return seed.has_value() ? *seed : std::random_device{}();
}

}  // namespace

VirtualModbusServer::VirtualModbusServer(ServerConfig config)
    : config_(std::move(config)), rng_(makeSeed(config_.randomSeed)) {
    if (config_.simulationInterval.count() <= 0 || config_.clientIoTimeout.count() <= 0) {
        throw std::invalid_argument("server intervals must be positive");
    }
    if (config_.maxClients == 0) {
        throw std::invalid_argument("maxClients must be at least 1");
    }
}

VirtualModbusServer::~VirtualModbusServer() { stop(); }

void VirtualModbusServer::start() {
    if (worker_.joinable()) {
        throw std::logic_error("VirtualModbusServer is already running");
    }

    listener_ = net::TcpSocket::listenOn(config_.bindAddress, config_.port);
    boundPort_ = listener_.localPort();

    startTime_ = std::chrono::steady_clock::now();
    updateSimulation();  // registers hold valid data before the first request arrives

    stopRequested_.store(false);
    running_.store(true);
    try {
        worker_ = std::thread(&VirtualModbusServer::serveLoop, this);
    } catch (...) {
        running_.store(false);
        listener_.close();
        throw;
    }

    Logger::info(kComponent, "virtual device listening on " + config_.bindAddress + ":" + std::to_string(boundPort_) +
                                 " (" + std::to_string(registers::kRegisterCount) + " holding registers)");
}

void VirtualModbusServer::stop() noexcept {
    stopRequested_.store(true);
    if (worker_.joinable()) {
        try {
            worker_.join();
        } catch (const std::system_error&) {
            // join() only throws for invalid states we have already excluded.
        }
        Logger::info(kComponent, "stopped after serving " + std::to_string(requestsServed()) + " requests");
    }
    listener_.close();
    running_.store(false);
}

std::vector<std::uint16_t> VirtualModbusServer::snapshotRegisters() const {
    const std::lock_guard<std::mutex> lock(registerMutex_);
    return {registers_.begin(), registers_.end()};
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------
void VirtualModbusServer::serveLoop() noexcept {
    try {
        auto nextUpdate = std::chrono::steady_clock::now() + config_.simulationInterval;
        std::vector<const net::TcpSocket*> watched;

        while (!stopRequested_.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextUpdate) {
                updateSimulation();
                nextUpdate += config_.simulationInterval;
                if (nextUpdate < now) {
                    nextUpdate = now + config_.simulationInterval;  // fell behind, do not burst
                }
            }

            watched.clear();
            watched.push_back(&listener_);
            for (const ClientSession& session : clients_) {
                watched.push_back(&session.socket);
            }

            const std::vector<std::size_t> ready = net::waitForReadable(watched, kSelectSlice);

            bool listenerReady = false;
            std::vector<bool> keep(clients_.size(), true);
            for (const std::size_t index : ready) {
                if (index == 0) {
                    listenerReady = true;
                } else {
                    keep[index - 1] = serveRequest(clients_[index - 1]);
                }
            }

            // Drop sessions that failed or disconnected; their sockets close via RAII.
            std::size_t write = 0;
            for (std::size_t read = 0; read < clients_.size(); ++read) {
                if (keep[read]) {
                    if (write != read) {
                        clients_[write] = std::move(clients_[read]);
                    }
                    ++write;
                }
            }
            clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(write), clients_.end());

            if (listenerReady) {
                acceptClient();
            }
        }
    } catch (const std::exception& e) {
        Logger::error(kComponent, std::string("server loop terminated: ") + e.what());
    } catch (...) {
        Logger::error(kComponent, "server loop terminated by an unknown error");
    }

    clients_.clear();
    running_.store(false);
}

void VirtualModbusServer::acceptClient() {
    try {
        net::TcpSocket socket = listener_.accept();
        std::string peer = socket.peerAddress();

        if (clients_.size() >= config_.maxClients) {
            Logger::warning(kComponent, "rejecting " + peer + ": client limit reached");
            return;  // socket closes when it goes out of scope
        }

        socket.setIoTimeout(config_.clientIoTimeout);
        Logger::info(kComponent, "client connected from " + peer);
        clients_.push_back(ClientSession{std::move(socket), std::move(peer)});
    } catch (const net::NetworkError& e) {
        Logger::warning(kComponent, std::string("accept failed: ") + e.what());
    }
}

bool VirtualModbusServer::serveRequest(ClientSession& session) {
    try {
        std::array<std::uint8_t, modbus::kMbapHeaderSize> headerBytes{};
        session.socket.receiveExact(headerBytes.data(), headerBytes.size());
        const modbus::MbapHeader request = modbus::parseMbapHeader(headerBytes.data());

        if (request.protocolId != modbus::kProtocolIdentifier || request.length < modbus::kMinMbapLength ||
            request.length > modbus::kMaxMbapLength) {
            // We cannot trust the length field, so the stream cannot be resynchronised.
            Logger::warning(kComponent, "dropping " + session.peer + ": malformed MBAP header (protocol=" +
                                            std::to_string(request.protocolId) +
                                            ", length=" + std::to_string(request.length) + ")");
            return false;
        }

        std::vector<std::uint8_t> pdu(request.length - 1u);
        session.socket.receiveExact(pdu.data(), pdu.size());

        const std::vector<std::uint8_t> responsePdu = processPdu(pdu);

        std::vector<std::uint8_t> frame;
        frame.reserve(modbus::kMbapHeaderSize + responsePdu.size());
        modbus::appendMbapHeader(frame, modbus::MbapHeader{request.transactionId, modbus::kProtocolIdentifier,
                                                           static_cast<std::uint16_t>(responsePdu.size() + 1u),
                                                           request.unitId});
        frame.insert(frame.end(), responsePdu.begin(), responsePdu.end());

        session.socket.sendAll(frame.data(), frame.size());
        requestsServed_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (const net::ConnectionClosedError&) {
        Logger::info(kComponent, "client " + session.peer + " disconnected");
    } catch (const net::NetworkError& e) {
        Logger::warning(kComponent, "dropping " + session.peer + ": " + e.what());
    }
    return false;
}

std::vector<std::uint8_t> VirtualModbusServer::processPdu(const std::vector<std::uint8_t>& pdu) const {
    const std::uint8_t functionCode = pdu.front();  // non-empty: MBAP length >= 2 was validated

    if (functionCode != modbus::kFunctionReadHoldingRegisters) {
        return exceptionPdu(functionCode, modbus::ExceptionCode::IllegalFunction);
    }
    if (pdu.size() != 5) {  // function + start(2) + quantity(2)
        return exceptionPdu(functionCode, modbus::ExceptionCode::IllegalDataValue);
    }

    const std::uint16_t startAddress = bytes::readUint16BigEndian(&pdu[1]);
    const std::uint16_t quantity = bytes::readUint16BigEndian(&pdu[3]);

    if (quantity == 0 || quantity > modbus::kMaxReadRegisterCount) {
        return exceptionPdu(functionCode, modbus::ExceptionCode::IllegalDataValue);
    }
    const std::size_t endAddress = static_cast<std::size_t>(startAddress) + quantity;
    if (endAddress > registers::kRegisterCount) {
        return exceptionPdu(functionCode, modbus::ExceptionCode::IllegalDataAddress);
    }

    std::vector<std::uint8_t> response;
    response.reserve(2 + quantity * 2u);
    response.push_back(functionCode);
    response.push_back(static_cast<std::uint8_t>(quantity * 2u));

    const std::lock_guard<std::mutex> lock(registerMutex_);
    for (std::size_t address = startAddress; address < endAddress; ++address) {
        bytes::appendUint16BigEndian(response, registers_[address]);
    }
    return response;
}

// ---------------------------------------------------------------------------
// Physical model
// ---------------------------------------------------------------------------
void VirtualModbusServer::updateSimulation() {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime_).count();
    std::normal_distribution<double> noise(0.0, 1.0);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);

    // Electrical load follows a slow production cycle, plus occasional
    // multi-second surges (e.g. a second compressor stage kicking in).
    double power = kNominalPowerWatts + 2200.0 * std::sin(kTwoPi * t / 45.0) + 120.0 * noise(rng_);
    if (surgeTicksRemaining_ == 0 && uniform(rng_) < 0.01) {
        surgeTicksRemaining_ = 10 + static_cast<int>(rng_() % 15u);
    }
    if (surgeTicksRemaining_ > 0) {
        power += kSurgePowerWatts;
        --surgeTicksRemaining_;
    }
    power = std::clamp(power, 0.0, 20000.0);
    const double loadAboveNominalKw = (power - kNominalPowerWatts) / 1000.0;

    // Housing temperature does not jump with the load: it chases a target
    // value through a first-order lag, like real thermal mass.
    const double targetTemperature =
        kNominalTemperatureCelsius + 2.5 * std::sin(kTwoPi * t / 120.0) + 1.8 * loadAboveNominalKw;
    housingTemperature_ += kThermalLagFactor * (targetTemperature - housingTemperature_);
    const double temperature = housingTemperature_ + 0.1 * noise(rng_);

    const double humidity = std::clamp(45.0 + 6.0 * std::cos(kTwoPi * t / 150.0) + 0.4 * noise(rng_), 0.0, 100.0);

    // Supply voltage sags slightly under heavy load.
    const double voltage =
        kNominalVoltage + 3.0 * std::sin(kTwoPi * t / 23.0) - 1.1 * loadAboveNominalKw + 0.4 * noise(rng_);

    std::uint16_t statusWord = registers::status::kRunning;
    if (temperature > kOverTemperatureThreshold) {
        statusWord = static_cast<std::uint16_t>(statusWord | registers::status::kOverTemperature);
    }
    if (power > kHighLoadThresholdWatts) {
        statusWord = static_cast<std::uint16_t>(statusWord | registers::status::kHighLoad);
    }
    ++heartbeat_;

    const std::uint32_t rawPower = encodeUnsigned32(power, registers::kPowerScale);

    const std::lock_guard<std::mutex> lock(registerMutex_);
    registers_[registers::kTemperature] = encodeSigned16(temperature, registers::kTemperatureScale);
    registers_[registers::kHumidity] = encodeUnsigned16(humidity, registers::kHumidityScale);
    registers_[registers::kPowerHigh] = static_cast<std::uint16_t>(rawPower >> 16);
    registers_[registers::kPowerLow] = static_cast<std::uint16_t>(rawPower & 0xFFFFu);
    registers_[registers::kVoltage] = encodeUnsigned16(voltage, registers::kVoltageScale);
    registers_[registers::kStatusWord] = statusWord;
    registers_[registers::kHeartbeat] = heartbeat_;
    registers_[registers::kFirmwareVersion] = kFirmwareVersion;
}

}  // namespace ipg
