// industrial-protocol-gateway
//
// Command line entry point. Starts an in-process virtual Modbus TCP device
// and a gateway engine that polls it and republishes the data as MQTT/JSON.

#include "GatewayEngine.hpp"
#include "Logger.hpp"
#include "MqttPublisher.hpp"
#include "VirtualModbusServer.hpp"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_shutdownSignal = 0;

extern "C" void onShutdownSignal(int /*signal*/) { g_shutdownSignal = 1; }

constexpr std::string_view kComponent = "Main";

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = ipg::modbus::kDefaultTcpPort;
    std::uint8_t unitId = 1;
    std::chrono::milliseconds pollInterval{1000};
    std::uint64_t maxCycles = 0;  // 0 = run until Ctrl+C
    std::string deviceId = "compressor-01";
    bool startEmbeddedServer = true;
    bool verbose = false;
    bool showHelp = false;
};

void printUsage() {
    std::cout << "Usage: industrial_gateway [options]\n"
                 "\n"
                 "Options:\n"
                 "  --host <address>     Modbus device address           (default 127.0.0.1)\n"
                 "  --port <number>      Modbus TCP port                 (default 5020)\n"
                 "  --unit-id <0-255>    Modbus unit identifier          (default 1)\n"
                 "  --interval <ms>      Poll interval, 50-60000 ms      (default 1000)\n"
                 "  --cycles <n>         Stop after n telemetry messages (default 0 = run forever)\n"
                 "  --device-id <name>   Device id used in MQTT topics   (default compressor-01)\n"
                 "  --no-server          Do not start the embedded virtual device;\n"
                 "                       poll an external Modbus TCP server instead\n"
                 "  -v, --verbose        Debug logging, incl. raw Modbus and MQTT bytes\n"
                 "  -h, --help           Show this help\n";
}

unsigned long long parseUnsigned(const std::string& option, const std::string& text, unsigned long long minimum,
                                 unsigned long long maximum) {
    unsigned long long value = 0;
    std::size_t consumed = 0;
    try {
        if (text.empty() || text.front() == '-') {
            throw std::invalid_argument("negative");
        }
        value = std::stoull(text, &consumed, 10);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid value for " + option + ": '" + text + "'");
    }
    if (consumed != text.size() || value < minimum || value > maximum) {
        throw std::invalid_argument("value for " + option + " must be between " + std::to_string(minimum) +
                                    " and " + std::to_string(maximum));
    }
    return value;
}

Options parseArguments(int argc, char* argv[]) {
    Options options;
    const std::vector<std::string> args(argv + 1, argv + argc);

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const auto nextValue = [&]() -> const std::string& {
            if (i + 1 >= args.size()) {
                throw std::invalid_argument("missing value for " + arg);
            }
            return args[++i];
        };

        if (arg == "-h" || arg == "--help") {
            options.showHelp = true;
        } else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--no-server") {
            options.startEmbeddedServer = false;
        } else if (arg == "--host") {
            options.host = nextValue();
        } else if (arg == "--port") {
            options.port = static_cast<std::uint16_t>(parseUnsigned(arg, nextValue(), 1, 65535));
        } else if (arg == "--unit-id") {
            options.unitId = static_cast<std::uint8_t>(parseUnsigned(arg, nextValue(), 0, 255));
        } else if (arg == "--interval") {
            options.pollInterval = std::chrono::milliseconds(parseUnsigned(arg, nextValue(), 50, 60000));
        } else if (arg == "--cycles") {
            options.maxCycles = parseUnsigned(arg, nextValue(), 0, 1'000'000'000);
        } else if (arg == "--device-id") {
            options.deviceId = nextValue();
        } else {
            throw std::invalid_argument("unknown option '" + arg + "'");
        }
    }
    return options;
}

void printBanner() {
    std::cout << "==============================================================\n"
                 "  industrial-protocol-gateway  |  Modbus TCP  ->  MQTT / JSON\n"
                 "==============================================================\n";
}

void printSummary(const ipg::GatewayStatistics& stats, const ipg::VirtualModbusServer* server) {
    std::cout << "--------------------------------------------------------------\n"
              << "  Poll cycles        : " << stats.pollCycles << '\n'
              << "  Successful reads   : " << stats.successfulReads << '\n'
              << "  Failed reads       : " << stats.failedReads << '\n'
              << "  Telemetry messages : " << stats.telemetryPublished << '\n'
              << "  Reconnects         : " << stats.reconnects << '\n';
    if (server != nullptr) {
        std::cout << "  Requests served    : " << server->requestsServed() << " (virtual device)\n";
    }
    std::cout << "--------------------------------------------------------------\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    using namespace ipg;

    Options options;
    try {
        options = parseArguments(argc, argv);
    } catch (const std::invalid_argument& e) {
        std::cerr << "error: " << e.what() << "\nRun with --help for usage.\n";
        return 2;
    }
    if (options.showHelp) {
        printUsage();
        return EXIT_SUCCESS;
    }

    Logger::setMinimumLevel(options.verbose ? LogLevel::Debug : LogLevel::Info);
    std::signal(SIGINT, onShutdownSignal);
    std::signal(SIGTERM, onShutdownSignal);

    try {
        printBanner();

        // Southbound side: the simulated field device (optional).
        std::optional<VirtualModbusServer> server;
        std::uint16_t devicePort = options.port;
        if (options.startEmbeddedServer) {
            ServerConfig serverConfig;
            serverConfig.bindAddress = options.host;
            serverConfig.port = options.port;
            server.emplace(serverConfig);
            server->start();
            devicePort = server->port();
        }

        // Northbound side: MQTT publisher with a console transport.
        PublisherConfig publisherConfig;
        publisherConfig.deviceId = options.deviceId;
        MqttPublisher publisher(publisherConfig, std::make_unique<ConsoleMqttTransport>(options.verbose));

        // The orchestrator in between.
        GatewayConfig gatewayConfig;
        gatewayConfig.modbus.host = options.host;
        gatewayConfig.modbus.port = devicePort;
        gatewayConfig.unitId = options.unitId;
        gatewayConfig.pollInterval = options.pollInterval;

        GatewayEngine engine(gatewayConfig, std::move(publisher));
        engine.start();
        Logger::info(kComponent, "gateway running - press Ctrl+C to stop");

        while (g_shutdownSignal == 0 && engine.isRunning()) {
            if (options.maxCycles > 0 && engine.statistics().telemetryPublished >= options.maxCycles) {
                Logger::info(kComponent, "reached " + std::to_string(options.maxCycles) + " telemetry messages");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g_shutdownSignal != 0) {
            Logger::info(kComponent, "shutdown signal received");
        }

        // Orderly shutdown: consumer first, then producer.
        engine.stop();
        if (server) {
            server->stop();
        }
        printSummary(engine.statistics(), server ? &*server : nullptr);
        return EXIT_SUCCESS;
    } catch (const std::exception& e) {
        Logger::error(kComponent, std::string("fatal: ") + e.what());
        return EXIT_FAILURE;
    }
}
