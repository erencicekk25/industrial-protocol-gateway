// Self-contained test suite (no external framework required).
// Run directly or through `ctest`.

#include "GatewayEngine.hpp"
#include "Logger.hpp"
#include "ModbusClient.hpp"
#include "MqttPublisher.hpp"
#include "RegisterMap.hpp"
#include "VirtualModbusServer.hpp"

#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define IPG_CHECK(condition)                                                                     \
    do {                                                                                         \
        ++g_checks;                                                                              \
        if (!(condition)) {                                                                      \
            ++g_failures;                                                                        \
            std::cerr << "  FAILED " << __FILE__ << ":" << __LINE__ << "  " #condition << '\n'; \
        }                                                                                        \
    } while (false)

template <typename Exception, typename Function>
bool throwsAs(Function&& function) {
    try {
        function();
    } catch (const Exception&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

bool nearlyEqual(double a, double b, double tolerance = 1e-9) { return std::fabs(a - b) <= tolerance; }

using Bytes = std::vector<std::uint8_t>;

/// Test double for the MQTT transport strategy. Shared state is guarded by a
/// mutex because the engine publishes from its worker thread.
struct CapturedMessages {
    std::mutex mutex;
    std::vector<ipg::MqttMessage> messages;
    std::vector<Bytes> packets;

    std::size_t size() {
        const std::lock_guard<std::mutex> lock(mutex);
        return messages.size();
    }
};

class CapturingTransport final : public ipg::IMqttTransport {
public:
    explicit CapturingTransport(std::shared_ptr<CapturedMessages> sink) : sink_(std::move(sink)) {}
    void transmit(const ipg::MqttMessage& message, const Bytes& packet) override {
        const std::lock_guard<std::mutex> lock(sink_->mutex);
        sink_->messages.push_back(message);
        sink_->packets.push_back(packet);
    }

private:
    std::shared_ptr<CapturedMessages> sink_;
};

ipg::ServerConfig ephemeralServerConfig() {
    ipg::ServerConfig config;
    config.port = 0;  // OS-assigned: tests never collide with a running gateway
    config.randomSeed = 42;
    return config;
}

std::vector<std::uint16_t> sampleBlock() {
    // -10.0 C, 45.5 %RH, 7512.4 W (75124 = 0x0001'2574), 399.6 V, running+overtemp, hb 7, fw 1.2
    return {0xFF9C, 455, 0x0001, 0x2574, 3996, 0x0003, 7, 0x0102};
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
void testModbusRequestEncoding() {
    const Bytes frame = ipg::ModbusClient::buildReadHoldingRegistersRequest(0x0102, 0x11, 0x0000, 0x0008);
    const Bytes expected{0x01, 0x02, 0x00, 0x00, 0x00, 0x06, 0x11, 0x03, 0x00, 0x00, 0x00, 0x08};
    IPG_CHECK(frame == expected);
}

void testModbusResponseParsing() {
    const Bytes ok{0x03, 0x04, 0x01, 0x2C, 0xFF, 0x9C};
    const auto values = ipg::ModbusClient::parseReadHoldingRegistersResponse(ok, 2);
    IPG_CHECK(values.size() == 2);
    IPG_CHECK(values[0] == 300);
    IPG_CHECK(values[1] == 0xFF9C);

    IPG_CHECK(throwsAs<ipg::modbus::ProtocolError>(
        [] { (void)ipg::ModbusClient::parseReadHoldingRegistersResponse({0x03, 0x04, 0x00, 0x01}, 2); }));
    IPG_CHECK(throwsAs<ipg::modbus::ProtocolError>(
        [] { (void)ipg::ModbusClient::parseReadHoldingRegistersResponse({0x04, 0x02, 0x00, 0x01}, 1); }));

    bool sawException = false;
    try {
        (void)ipg::ModbusClient::parseReadHoldingRegistersResponse({0x83, 0x02}, 1);
    } catch (const ipg::modbus::ModbusException& e) {
        sawException = e.exceptionCode() == 0x02 && e.functionCode() == 0x03;
    }
    IPG_CHECK(sawException);
}

void testRegisterDecoding() {
    const ipg::registers::SensorReading r = ipg::registers::decode(sampleBlock());
    IPG_CHECK(nearlyEqual(r.temperatureCelsius, -10.0));
    IPG_CHECK(nearlyEqual(r.humidityPercent, 45.5));
    IPG_CHECK(nearlyEqual(r.powerWatts, 7512.4, 1e-6));
    IPG_CHECK(nearlyEqual(r.voltageVolts, 399.6, 1e-6));
    IPG_CHECK(r.hasStatus(ipg::registers::status::kRunning));
    IPG_CHECK(r.hasStatus(ipg::registers::status::kOverTemperature));
    IPG_CHECK(!r.hasStatus(ipg::registers::status::kHighLoad));
    IPG_CHECK(throwsAs<std::invalid_argument>([] { (void)ipg::registers::decode({1, 2, 3}); }));
}

void testMqttRemainingLength() {
    using ipg::MqttPublisher;
    IPG_CHECK(MqttPublisher::encodeRemainingLength(0) == Bytes{0x00});
    IPG_CHECK(MqttPublisher::encodeRemainingLength(127) == Bytes{0x7F});
    IPG_CHECK(MqttPublisher::encodeRemainingLength(128) == (Bytes{0x80, 0x01}));
    IPG_CHECK(MqttPublisher::encodeRemainingLength(16383) == (Bytes{0xFF, 0x7F}));
    IPG_CHECK(MqttPublisher::encodeRemainingLength(16384) == (Bytes{0x80, 0x80, 0x01}));
    IPG_CHECK(MqttPublisher::encodeRemainingLength(268'435'455) == (Bytes{0xFF, 0xFF, 0xFF, 0x7F}));
    IPG_CHECK(throwsAs<std::length_error>([] { (void)MqttPublisher::encodeRemainingLength(268'435'456); }));
}

void testMqttPublishPacket() {
    using ipg::MqttMessage;
    using ipg::MqttPublisher;

    const Bytes qos0 = MqttPublisher::encodePublishPacket(MqttMessage{"a/b", "hi", 0, false}, 0);
    IPG_CHECK(qos0 == (Bytes{0x30, 0x07, 0x00, 0x03, 'a', '/', 'b', 'h', 'i'}));

    const Bytes qos1 = MqttPublisher::encodePublishPacket(MqttMessage{"a/b", "hi", 1, true}, 10);
    IPG_CHECK(qos1 == (Bytes{0x33, 0x09, 0x00, 0x03, 'a', '/', 'b', 0x00, 0x0A, 'h', 'i'}));

    IPG_CHECK(throwsAs<std::invalid_argument>(
        [] { (void)MqttPublisher::encodePublishPacket(MqttMessage{"a/#", "x", 0, false}, 0); }));
    IPG_CHECK(throwsAs<std::invalid_argument>(
        [] { (void)MqttPublisher::encodePublishPacket(MqttMessage{"a/b", "x", 1, false}, 0); }));
}

void testTelemetryJson() {
    auto sink = std::make_shared<CapturedMessages>();
    ipg::MqttPublisher publisher(ipg::PublisherConfig{}, std::make_unique<CapturingTransport>(sink));

    publisher.publishTelemetry(sampleBlock(), 1);
    IPG_CHECK(sink->size() == 1);
    IPG_CHECK(publisher.publishedCount() == 1);

    const ipg::MqttMessage& message = sink->messages.front();
    IPG_CHECK(message.topic == "factory/line-1/compressor-01/telemetry");
    IPG_CHECK(message.payload.find("\"temperature\":{\"value\":-10.0,\"unit\":\"celsius\"}") != std::string::npos);
    IPG_CHECK(message.payload.find("\"power\":{\"value\":7512.4,\"unit\":\"watt\"}") != std::string::npos);
    IPG_CHECK(message.payload.find("\"overTemperature\":true") != std::string::npos);
    IPG_CHECK(message.payload.find("\"firmware\":\"1.2\"") != std::string::npos);
    IPG_CHECK(message.payload.find("\"sequence\":1,") != std::string::npos);
    IPG_CHECK(message.payload.find("\"raw\":[65436,455,1,9588,3996,3,7,258]") != std::string::npos);
    IPG_CHECK(message.payload.front() == '{' && message.payload.back() == '}');

    IPG_CHECK(throwsAs<std::invalid_argument>([] { ipg::MqttPublisher(ipg::PublisherConfig{}, nullptr); }));
}

void testServerClientRoundTrip() {
    ipg::VirtualModbusServer server(ephemeralServerConfig());
    server.start();
    IPG_CHECK(server.isRunning());
    IPG_CHECK(server.port() != 0);

    ipg::ModbusClientConfig clientConfig;
    clientConfig.port = server.port();
    ipg::ModbusClient client(clientConfig);
    client.connect();
    IPG_CHECK(client.isConnected());

    const auto block = client.readHoldingRegisters(1, ipg::registers::kBlockStart, ipg::registers::kRegisterCount);
    IPG_CHECK(block.size() == ipg::registers::kRegisterCount);

    const auto reading = ipg::registers::decode(block);
    IPG_CHECK(reading.temperatureCelsius > 25.0 && reading.temperatureCelsius < 65.0);
    IPG_CHECK(reading.humidityPercent >= 0.0 && reading.humidityPercent <= 100.0);
    IPG_CHECK(reading.powerWatts > 1000.0 && reading.powerWatts < 20000.0);
    IPG_CHECK(reading.voltageVolts > 380.0 && reading.voltageVolts < 420.0);
    IPG_CHECK(reading.hasStatus(ipg::registers::status::kRunning));
    IPG_CHECK(reading.firmwareVersion == 0x0102);

    const auto tail = client.readHoldingRegisters(1, 6, 2);
    IPG_CHECK(tail.size() == 2 && tail[1] == 0x0102);

    // Out-of-range read: device answers with exception 0x02, link stays up.
    bool sawIllegalAddress = false;
    try {
        (void)client.readHoldingRegisters(1, 7, 2);
    } catch (const ipg::modbus::ModbusException& e) {
        sawIllegalAddress = e.exceptionCode() == static_cast<std::uint8_t>(ipg::modbus::ExceptionCode::IllegalDataAddress);
    }
    IPG_CHECK(sawIllegalAddress);
    IPG_CHECK(client.isConnected());
    IPG_CHECK(client.readHoldingRegisters(1, 0, 1).size() == 1);

    IPG_CHECK(throwsAs<std::invalid_argument>([&] { (void)client.readHoldingRegisters(1, 0, 0); }));
    IPG_CHECK(throwsAs<std::invalid_argument>([&] { (void)client.readHoldingRegisters(1, 0, 126); }));

    client.disconnect();
    IPG_CHECK(throwsAs<ipg::net::NetworkError>([&] { (void)client.readHoldingRegisters(1, 0, 1); }));

    server.stop();
    IPG_CHECK(!server.isRunning());
    IPG_CHECK(server.requestsServed() >= 4);
}

void testEngineEndToEnd() {
    ipg::VirtualModbusServer server(ephemeralServerConfig());
    server.start();

    auto sink = std::make_shared<CapturedMessages>();
    ipg::MqttPublisher publisher(ipg::PublisherConfig{}, std::make_unique<CapturingTransport>(sink));

    ipg::GatewayConfig config;
    config.modbus.port = server.port();
    config.pollInterval = std::chrono::milliseconds(50);

    ipg::GatewayStatistics stats;
    {
        ipg::GatewayEngine engine(config, std::move(publisher));
        engine.start();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (engine.statistics().telemetryPublished < 5 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        engine.stop();
        IPG_CHECK(!engine.isRunning());
        stats = engine.statistics();
    }  // destructor after an explicit stop() must be a harmless no-op
    server.stop();

    IPG_CHECK(stats.telemetryPublished >= 5);
    IPG_CHECK(stats.failedReads == 0);
    IPG_CHECK(stats.successfulReads == stats.telemetryPublished);

    const std::lock_guard<std::mutex> lock(sink->mutex);
    IPG_CHECK(sink->messages.size() == stats.telemetryPublished + 2);  // + online + offline
    IPG_CHECK(sink->messages.front().topic == "factory/line-1/compressor-01/availability");
    IPG_CHECK(sink->messages.front().payload == "online");
    IPG_CHECK(sink->messages.front().retain);
    IPG_CHECK(sink->messages.back().payload == "offline");
}

void testEngineSurvivesMissingDevice() {
    // Grab a free port, then release it so nothing is listening there.
    std::uint16_t unusedPort = 0;
    {
        ipg::VirtualModbusServer probe(ephemeralServerConfig());
        probe.start();
        unusedPort = probe.port();
    }

    auto sink = std::make_shared<CapturedMessages>();
    ipg::MqttPublisher publisher(ipg::PublisherConfig{}, std::make_unique<CapturingTransport>(sink));

    ipg::GatewayConfig config;
    config.modbus.port = unusedPort;
    config.pollInterval = std::chrono::milliseconds(50);
    config.initialBackoff = std::chrono::milliseconds(20);
    config.maxBackoff = std::chrono::milliseconds(40);

    ipg::GatewayEngine engine(config, std::move(publisher));
    engine.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (engine.statistics().failedReads < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    engine.stop();

    const auto stats = engine.statistics();
    IPG_CHECK(stats.failedReads >= 1);
    IPG_CHECK(stats.successfulReads == 0);
    IPG_CHECK(sink->size() == 0);  // never online => no availability flapping
}

}  // namespace

int main() {
    ipg::Logger::setMinimumLevel(ipg::LogLevel::Error);  // keep test output focused

    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"modbus request encoding", testModbusRequestEncoding},
        {"modbus response parsing", testModbusResponseParsing},
        {"register map decoding", testRegisterDecoding},
        {"mqtt remaining length", testMqttRemainingLength},
        {"mqtt publish packet", testMqttPublishPacket},
        {"telemetry json", testTelemetryJson},
        {"server/client round trip", testServerClientRoundTrip},
        {"engine end to end", testEngineEndToEnd},
        {"engine survives missing device", testEngineSurvivesMissingDevice},
    };

    for (const auto& [name, test] : tests) {
        const int failuresBefore = g_failures;
        try {
            test();
        } catch (const std::exception& e) {
            ++g_failures;
            std::cerr << "  FAILED with exception: " << e.what() << '\n';
        }
        std::cout << (g_failures == failuresBefore ? "[ PASS ] " : "[ FAIL ] ") << name << '\n';
    }

    std::cout << '\n' << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
