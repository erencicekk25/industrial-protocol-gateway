#pragma once

#include "RegisterMap.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ipg {

struct MqttMessage {
    std::string topic;
    std::string payload;
    std::uint8_t qos = 0;
    bool retain = false;
};

/// Strategy interface: where encoded MQTT packets go.
///
/// The publisher produces byte-exact MQTT 3.1.1 PUBLISH packets; a transport
/// decides what to do with them. Swapping ConsoleMqttTransport for a real
/// TCP transport to a broker (e.g. Mosquitto on port 1883) would not require
/// touching the publisher or the gateway engine.
class IMqttTransport {
public:
    virtual ~IMqttTransport() = default;
    virtual void transmit(const MqttMessage& message, const std::vector<std::uint8_t>& packet) = 0;

protected:
    IMqttTransport() = default;
    IMqttTransport(const IMqttTransport&) = default;
    IMqttTransport& operator=(const IMqttTransport&) = default;
};

/// Prints every PUBLISH to the console instead of sending it to a broker.
class ConsoleMqttTransport final : public IMqttTransport {
public:
    explicit ConsoleMqttTransport(bool dumpWireBytes = false) noexcept : dumpWireBytes_(dumpWireBytes) {}
    void transmit(const MqttMessage& message, const std::vector<std::uint8_t>& packet) override;

private:
    bool dumpWireBytes_;
};

struct PublisherConfig {
    std::string clientId = "ipg-gateway-01";
    std::string topicPrefix = "factory/line-1";
    std::string deviceId = "compressor-01";
    std::uint8_t telemetryQos = 0;
};

/// Turns raw Modbus registers into structured JSON telemetry and wraps it
/// in MQTT PUBLISH packets.
///
/// Topic layout:
///   <prefix>/<deviceId>/telemetry     JSON measurements, QoS configurable
///   <prefix>/<deviceId>/availability  "online"/"offline", QoS 1, retained
///
/// Not thread-safe; owned and driven by a single gateway thread.
class MqttPublisher {
public:
    MqttPublisher(PublisherConfig config, std::unique_ptr<IMqttTransport> transport);

    MqttPublisher(MqttPublisher&&) noexcept = default;
    MqttPublisher& operator=(MqttPublisher&&) noexcept = default;
    MqttPublisher(const MqttPublisher&) = delete;
    MqttPublisher& operator=(const MqttPublisher&) = delete;
    ~MqttPublisher() = default;

    /// Decodes the register block, builds the JSON document and publishes it.
    void publishTelemetry(const std::vector<std::uint16_t>& rawRegisters, std::uint8_t unitId);

    /// Retained availability message, the same pattern a broker uses for a
    /// client's Last Will: subscribers always see the latest device state.
    void publishAvailability(bool online);

    void publish(const MqttMessage& message);

    [[nodiscard]] std::string telemetryTopic() const;
    [[nodiscard]] std::string availabilityTopic() const;
    [[nodiscard]] std::uint64_t publishedCount() const noexcept { return publishedCount_; }
    [[nodiscard]] const PublisherConfig& config() const noexcept { return config_; }

    [[nodiscard]] std::string buildTelemetryJson(const registers::SensorReading& reading,
                                                 const std::vector<std::uint16_t>& rawRegisters, std::uint8_t unitId,
                                                 std::chrono::system_clock::time_point timestamp,
                                                 std::uint64_t sequence) const;

    /// MQTT "Remaining Length" variable byte integer (1-4 bytes, 7 bits each).
    [[nodiscard]] static std::vector<std::uint8_t> encodeRemainingLength(std::size_t length);

    /// Encodes an MQTT 3.1.1 PUBLISH control packet.
    /// packetId must be non-zero when qos > 0 and is ignored for qos 0.
    [[nodiscard]] static std::vector<std::uint8_t> encodePublishPacket(const MqttMessage& message,
                                                                       std::uint16_t packetId);

private:
    [[nodiscard]] std::uint16_t nextPacketId() noexcept;

    PublisherConfig config_;
    std::unique_ptr<IMqttTransport> transport_;
    std::uint64_t telemetrySequence_ = 0;
    std::uint64_t publishedCount_ = 0;
    std::uint16_t packetId_ = 0;
};

}  // namespace ipg
