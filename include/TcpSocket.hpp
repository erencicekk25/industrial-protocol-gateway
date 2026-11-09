#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ipg::net {

#ifdef _WIN32
using NativeSocket = std::uintptr_t;  // matches Winsock's SOCKET
inline constexpr NativeSocket kInvalidSocket = ~NativeSocket{0};
#else
using NativeSocket = int;
inline constexpr NativeSocket kInvalidSocket = -1;
#endif

/// Base class for every socket-level failure. Carries the OS error code.
class NetworkError : public std::runtime_error {
public:
    explicit NetworkError(const std::string& message, int systemCode = 0)
        : std::runtime_error(message), systemCode_(systemCode) {}

    [[nodiscard]] int systemCode() const noexcept { return systemCode_; }

private:
    int systemCode_;
};

/// A send/receive did not complete within the configured timeout.
class TimeoutError : public NetworkError {
public:
    using NetworkError::NetworkError;
};

/// The peer performed an orderly shutdown of the connection.
class ConnectionClosedError : public NetworkError {
public:
    using NetworkError::NetworkError;
};

/// RAII guard for the platform networking stack.
///
/// On Windows this pairs WSAStartup/WSACleanup (which are reference counted
/// by the OS, so several contexts may coexist). On POSIX it is a no-op.
/// Any object that owns sockets should also own a NetworkContext declared
/// *before* its sockets, so the stack outlives them.
class NetworkContext {
public:
    NetworkContext();
    ~NetworkContext();

    NetworkContext(const NetworkContext&) = delete;
    NetworkContext& operator=(const NetworkContext&) = delete;
    NetworkContext(NetworkContext&&) = delete;
    NetworkContext& operator=(NetworkContext&&) = delete;
};

/// Move-only RAII owner of a blocking IPv4 TCP socket.
///
/// The handle is closed exactly once: in close() or in the destructor.
/// All I/O functions throw NetworkError (or a subclass) on failure.
class TcpSocket {
public:
    TcpSocket() noexcept = default;
    ~TcpSocket();

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    /// Resolves host (IPv4) and connects. TCP_NODELAY is enabled because
    /// Modbus is a small request/response protocol and Nagle only adds latency.
    [[nodiscard]] static TcpSocket connectTo(const std::string& host, std::uint16_t port);

    /// Creates a listening socket. Port 0 asks the OS for an ephemeral port.
    [[nodiscard]] static TcpSocket listenOn(const std::string& host, std::uint16_t port, int backlog = 8);

    [[nodiscard]] bool isValid() const noexcept { return handle_ != kInvalidSocket; }
    [[nodiscard]] NativeSocket native() const noexcept { return handle_; }
    void close() noexcept;

    [[nodiscard]] TcpSocket accept();

    /// Applies the same timeout to blocking send and receive calls.
    void setIoTimeout(std::chrono::milliseconds timeout);

    void sendAll(const std::uint8_t* data, std::size_t size);

    /// Blocks until exactly `size` bytes are received.
    /// Throws ConnectionClosedError if the peer closes first, TimeoutError on timeout.
    void receiveExact(std::uint8_t* buffer, std::size_t size);

    [[nodiscard]] bool waitReadable(std::chrono::milliseconds timeout) const;

    [[nodiscard]] std::uint16_t localPort() const;
    [[nodiscard]] std::string peerAddress() const;

private:
    explicit TcpSocket(NativeSocket handle) noexcept : handle_(handle) {}

    void requireValid(const char* operation) const;
    void setNoDelay() noexcept;

    NativeSocket handle_ = kInvalidSocket;
};

/// Waits until at least one of the given sockets becomes readable (or the
/// timeout expires) and returns the indices of the readable ones.
/// Null or invalid entries are ignored. A listening socket is "readable"
/// when a connection is waiting to be accepted.
[[nodiscard]] std::vector<std::size_t> waitForReadable(const std::vector<const TcpSocket*>& sockets,
                                                       std::chrono::milliseconds timeout);

}  // namespace ipg::net
