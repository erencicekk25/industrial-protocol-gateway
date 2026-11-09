#include "MqttPublisher.hpp"

#include "ByteUtils.hpp"
#include "Logger.hpp"
#include "TimeUtils.hpp"

#include <cstdio>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ipg {
namespace {

constexpr std::string_view kComponent = "MQTT";
constexpr std::size_t kMaxRemainingLength = 268'435'455;  // 0xFF 0xFF 0xFF 0x7F
constexpr std::uint8_t kPublishPacketType = 0x30;          // packet type 3 in the high nibble

std::string escapeJson(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (const char c : text) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                    out += buffer;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

void validateTopicName(const std::string& topic) {
    if (topic.empty()) {
        throw std::invalid_argument("MQTT topic must not be empty");
    }
    if (topic.size() > std::numeric_limits<std::uint16_t>::max()) {
        throw std::invalid_argument("MQTT topic exceeds 65535 bytes");
    }
    if (topic.find_first_of(std::string("+#\0", 3)) != std::string::npos) {
        throw std::invalid_argument("MQTT topic name must not contain wildcards or NUL: '" + topic + "'");
    }
}

void appendMeasurement(std::ostream& json, std::string_view name, double value, std::string_view unit,
                       int precision) {
    json << '"' << name << "\":{\"value\":" << std::setprecision(precision) << value << ",\"unit\":\"" << unit
         << "\"}";
}

}  // namespace

// ---------------------------------------------------------------------------
// ConsoleMqttTransport
// ---------------------------------------------------------------------------
void ConsoleMqttTransport::transmit(const MqttMessage& message, const std::vector<std::uint8_t>& packet) {
    Logger::info(kComponent, "PUBLISH " + message.topic + " (qos=" + std::to_string(message.qos) +
                                 ", retain=" + (message.retain ? "1" : "0") + ", " + std::to_string(packet.size()) +
                                 " bytes on the wire)");
    Logger::info(kComponent, "  payload " + message.payload);
    if (dumpWireBytes_ && Logger::isEnabled(LogLevel::Debug)) {
        Logger::debug(kComponent, "  packet  " + bytes::toHexString(packet));
    }
}

// ---------------------------------------------------------------------------
// MqttPublisher
// ---------------------------------------------------------------------------
MqttPublisher::MqttPublisher(PublisherConfig config, std::unique_ptr<IMqttTransport> transport)
    : config_(std::move(config)), transport_(std::move(transport)) {
    if (!transport_) {
        throw std::invalid_argument("MqttPublisher requires a transport");
    }
    if (config_.telemetryQos > 2) {
        throw std::invalid_argument("MQTT QoS must be 0, 1 or 2");
    }
    if (config_.deviceId.empty() || config_.clientId.empty()) {
        throw std::invalid_argument("deviceId and clientId must not be empty");
    }
    validateTopicName(telemetryTopic());
    validateTopicName(availabilityTopic());
}

std::string MqttPublisher::telemetryTopic() const {
    return config_.topicPrefix + "/" + config_.deviceId + "/telemetry";
}

std::string MqttPublisher::availabilityTopic() const {
    return config_.topicPrefix + "/" + config_.deviceId + "/availability";
}

std::uint16_t MqttPublisher::nextPacketId() noexcept {
    // Packet identifiers are 1..65535; zero is reserved.
    packetId_ = (packetId_ == std::numeric_limits<std::uint16_t>::max()) ? std::uint16_t{1}
                                                                          : static_cast<std::uint16_t>(packetId_ + 1u);
    return packetId_;
}

void MqttPublisher::publish(const MqttMessage& message) {
    const std::uint16_t packetId = message.qos > 0 ? nextPacketId() : std::uint16_t{0};
    const std::vector<std::uint8_t> packet = encodePublishPacket(message, packetId);
    transport_->transmit(message, packet);
    ++publishedCount_;
}

void MqttPublisher::publishTelemetry(const std::vector<std::uint16_t>& rawRegisters, std::uint8_t unitId) {
    const registers::SensorReading reading = registers::decode(rawRegisters);
    const std::uint64_t sequence = telemetrySequence_ + 1;

    MqttMessage message;
    message.topic = telemetryTopic();
    message.payload = buildTelemetryJson(reading, rawRegisters, unitId, std::chrono::system_clock::now(), sequence);
    message.qos = config_.telemetryQos;
    message.retain = false;

    publish(message);
    telemetrySequence_ = sequence;  // only advance once the message actually went out
}

void MqttPublisher::publishAvailability(bool online) {
    MqttMessage message;
    message.topic = availabilityTopic();
    message.payload = online ? "online" : "offline";
    message.qos = 1;
    message.retain = true;
    publish(message);
}

std::string MqttPublisher::buildTelemetryJson(const registers::SensorReading& reading,
                                              const std::vector<std::uint16_t>& rawRegisters, std::uint8_t unitId,
                                              std::chrono::system_clock::time_point timestamp,
                                              std::uint64_t sequence) const {
    namespace status = registers::status;

    std::ostringstream json;
    json.imbue(std::locale::classic());  // always '.' as decimal separator, regardless of OS locale
    json << std::fixed;

    json << "{\"deviceId\":\"" << escapeJson(config_.deviceId) << '"'                        //
         << ",\"gatewayId\":\"" << escapeJson(config_.clientId) << '"'                       //
         << ",\"sequence\":" << sequence                                                     //
         << ",\"timestamp\":\"" << timeutil::formatIso8601Utc(timestamp) << '"'               //
         << ",\"source\":{\"protocol\":\"modbus-tcp\",\"unitId\":" << static_cast<unsigned>(unitId)
         << ",\"function\":\"read-holding-registers\",\"startAddress\":" << registers::kBlockStart
         << ",\"registerCount\":" << rawRegisters.size() << '}';

    json << ",\"measurements\":{";
    appendMeasurement(json, "temperature", reading.temperatureCelsius, "celsius", 1);
    json << ',';
    appendMeasurement(json, "humidity", reading.humidityPercent, "percent", 1);
    json << ',';
    appendMeasurement(json, "power", reading.powerWatts, "watt", 1);
    json << ',';
    appendMeasurement(json, "voltage", reading.voltageVolts, "volt", 1);
    json << '}';

    json << ",\"status\":{\"running\":" << std::boolalpha << reading.hasStatus(status::kRunning)
         << ",\"overTemperature\":" << reading.hasStatus(status::kOverTemperature)
         << ",\"highLoad\":" << reading.hasStatus(status::kHighLoad) << std::noboolalpha
         << ",\"heartbeat\":" << reading.heartbeat << ",\"firmware\":\"" << (reading.firmwareVersion >> 8) << '.'
         << (reading.firmwareVersion & 0xFFu) << "\"}";

    json << ",\"raw\":[";
    for (std::size_t i = 0; i < rawRegisters.size(); ++i) {
        json << (i == 0 ? "" : ",") << rawRegisters[i];
    }
    json << "]}";

    return json.str();
}

std::vector<std::uint8_t> MqttPublisher::encodeRemainingLength(std::size_t length) {
    if (length > kMaxRemainingLength) {
        throw std::length_error("MQTT remaining length exceeds 268435455 bytes");
    }
    std::vector<std::uint8_t> encoded;
    do {
        auto digit = static_cast<std::uint8_t>(length % 128u);
        length /= 128u;
        if (length > 0) {
            digit = static_cast<std::uint8_t>(digit | 0x80u);  // continuation bit
        }
        encoded.push_back(digit);
    } while (length > 0);
    return encoded;
}

std::vector<std::uint8_t> MqttPublisher::encodePublishPacket(const MqttMessage& message, std::uint16_t packetId) {
    validateTopicName(message.topic);
    if (message.qos > 2) {
        throw std::invalid_argument("MQTT QoS must be 0, 1 or 2");
    }
    if (message.qos > 0 && packetId == 0) {
        throw std::invalid_argument("packet identifier must be non-zero when QoS > 0");
    }

    // Variable header = topic length (2) + topic + [packet id (2) if QoS > 0]
    const std::size_t variableHeaderSize = 2 + message.topic.size() + (message.qos > 0 ? 2u : 0u);
    const std::size_t remainingLength = variableHeaderSize + message.payload.size();
    const std::vector<std::uint8_t> remainingLengthBytes = encodeRemainingLength(remainingLength);

    // Fixed header byte 1: type (4 bits) | DUP (1) | QoS (2) | RETAIN (1)
    const auto fixedHeader = static_cast<std::uint8_t>(kPublishPacketType | (message.qos << 1) |
                                                       (message.retain ? 0x01u : 0x00u));

    std::vector<std::uint8_t> packet;
    packet.reserve(1 + remainingLengthBytes.size() + remainingLength);
    packet.push_back(fixedHeader);
    packet.insert(packet.end(), remainingLengthBytes.begin(), remainingLengthBytes.end());

    bytes::appendUint16BigEndian(packet, static_cast<std::uint16_t>(message.topic.size()));
    packet.insert(packet.end(), message.topic.begin(), message.topic.end());
    if (message.qos > 0) {
        bytes::appendUint16BigEndian(packet, packetId);
    }
    packet.insert(packet.end(), message.payload.begin(), message.payload.end());
    return packet;
}

}  // namespace ipg
