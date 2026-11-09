#include "TcpSocket.hpp"

#include <algorithm>
#include <memory>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace ipg::net {
namespace {

// ---------------------------------------------------------------------------
// Thin platform abstraction. Everything OS specific lives in this block so
// the rest of the file reads the same on Windows and POSIX.
// ---------------------------------------------------------------------------
#ifdef _WIN32
using OsSocket = SOCKET;
using SockLen = int;
using IoLength = int;
constexpr int kSendFlags = 0;
#else
using OsSocket = int;
using SockLen = socklen_t;
using IoLength = std::size_t;
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;  // never raise SIGPIPE on a broken connection
#else
constexpr int kSendFlags = 0;
#endif
#endif

constexpr std::size_t kMaxIoChunk = 64 * 1024;

OsSocket toOs(NativeSocket handle) noexcept { return static_cast<OsSocket>(handle); }
NativeSocket fromOs(OsSocket handle) noexcept { return static_cast<NativeSocket>(handle); }

int lastSocketError() noexcept {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

bool isInterrupted(int code) noexcept {
#ifdef _WIN32
    return code == WSAEINTR;
#else
    return code == EINTR;
#endif
}

bool isTimeout(int code) noexcept {
#ifdef _WIN32
    return code == WSAETIMEDOUT || code == WSAEWOULDBLOCK;
#else
    return code == EAGAIN || code == EWOULDBLOCK || code == ETIMEDOUT;
#endif
}

void closeNative(NativeSocket handle) noexcept {
#ifdef _WIN32
    ::closesocket(toOs(handle));
#else
    ::close(toOs(handle));
#endif
}

std::string describeError(int code) {
#ifdef _WIN32
    // FormatMessage returns text in the OS display language (and ANSI code
    // page), which would make logs unreadable on non-English systems. Keep
    // the common cases deterministic and in English.
    const char* text = nullptr;
    switch (code) {
        case WSAEADDRINUSE:
            text = "address already in use";
            break;
        case WSAEADDRNOTAVAIL:
            text = "address not available";
            break;
        case WSAECONNREFUSED:
            text = "connection refused";
            break;
        case WSAECONNRESET:
            text = "connection reset by peer";
            break;
        case WSAECONNABORTED:
            text = "connection aborted";
            break;
        case WSAETIMEDOUT:
            text = "connection timed out";
            break;
        case WSAENETUNREACH:
            text = "network unreachable";
            break;
        case WSAEHOSTUNREACH:
            text = "host unreachable";
            break;
        case WSAEACCES:
            text = "permission denied";
            break;
        case WSAENOTCONN:
            text = "socket is not connected";
            break;
        case WSAESHUTDOWN:
            text = "socket has been shut down";
            break;
        case WSANOTINITIALISED:
            text = "Winsock not initialised";
            break;
        default:
            text = "socket error";
            break;
    }
    return std::string(text) + " (code " + std::to_string(code) + ")";
#else
    return std::system_category().message(code) + " (code " + std::to_string(code) + ")";
#endif
}

[[noreturn]] void throwSocketError(const std::string& context) {
    const int code = lastSocketError();
    if (isTimeout(code)) {
        throw TimeoutError(context + ": operation timed out", code);
    }
    throw NetworkError(context + ": " + describeError(code), code);
}

timeval toTimeval(std::chrono::milliseconds timeout) noexcept {
    const auto count = std::max<long long>(0, timeout.count());
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(count / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((count % 1000) * 1000);
    return tv;
}

struct AddrInfoDeleter {
    void operator()(addrinfo* info) const noexcept {
        if (info != nullptr) {
            ::freeaddrinfo(info);
        }
    }
};
using AddrInfoPtr = std::unique_ptr<addrinfo, AddrInfoDeleter>;

AddrInfoPtr resolveIpv4(const std::string& host, std::uint16_t port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = passive ? AI_PASSIVE : 0;

    const std::string service = std::to_string(port);
    const char* node = host.empty() ? nullptr : host.c_str();

    addrinfo* raw = nullptr;
    const int rc = ::getaddrinfo(node, service.c_str(), &hints, &raw);
    AddrInfoPtr result(raw);  // owns the list even if we throw below
    if (rc != 0 || !result) {
        throw NetworkError("cannot resolve '" + host + ":" + service + "' (getaddrinfo error " +
                               std::to_string(rc) + ")",
                           rc);
    }
    return result;
}

}  // namespace

// ---------------------------------------------------------------------------
// NetworkContext
// ---------------------------------------------------------------------------
NetworkContext::NetworkContext() {
#ifdef _WIN32
    WSADATA data{};
    const int rc = ::WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        throw NetworkError("WSAStartup failed: " + describeError(rc), rc);
    }
#endif
}

NetworkContext::~NetworkContext() {
#ifdef _WIN32
    ::WSACleanup();
#endif
}

// ---------------------------------------------------------------------------
// TcpSocket
// ---------------------------------------------------------------------------
TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(std::exchange(other.handle_, kInvalidSocket)) {}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, kInvalidSocket);
    }
    return *this;
}

void TcpSocket::close() noexcept {
    if (isValid()) {
        closeNative(handle_);
        handle_ = kInvalidSocket;
    }
}

void TcpSocket::requireValid(const char* operation) const {
    if (!isValid()) {
        throw NetworkError(std::string(operation) + ": socket is not open");
    }
}

TcpSocket TcpSocket::connectTo(const std::string& host, std::uint16_t port) {
    const AddrInfoPtr addresses = resolveIpv4(host, port, false);

    int lastCode = 0;
    for (const addrinfo* candidate = addresses.get(); candidate != nullptr; candidate = candidate->ai_next) {
        TcpSocket socket(fromOs(::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol)));
        if (!socket.isValid()) {
            lastCode = lastSocketError();
            continue;
        }
        if (::connect(toOs(socket.handle_), candidate->ai_addr, static_cast<SockLen>(candidate->ai_addrlen)) == 0) {
            socket.setNoDelay();
            return socket;
        }
        lastCode = lastSocketError();
    }
    throw NetworkError("connect to " + host + ":" + std::to_string(port) + " failed: " + describeError(lastCode),
                       lastCode);
}

TcpSocket TcpSocket::listenOn(const std::string& host, std::uint16_t port, int backlog) {
    const AddrInfoPtr addresses = resolveIpv4(host, port, true);
    const addrinfo* address = addresses.get();

    TcpSocket listener(fromOs(::socket(address->ai_family, address->ai_socktype, address->ai_protocol)));
    if (!listener.isValid()) {
        throwSocketError("socket");
    }

    const int enable = 1;
#ifdef _WIN32
    // On Windows SO_REUSEADDR would let another process steal the port;
    // SO_EXCLUSIVEADDRUSE is the safe equivalent.
    ::setsockopt(toOs(listener.handle_), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&enable),
                 sizeof(enable));
#else
    ::setsockopt(toOs(listener.handle_), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
#endif

    if (::bind(toOs(listener.handle_), address->ai_addr, static_cast<SockLen>(address->ai_addrlen)) != 0) {
        throwSocketError("bind " + host + ":" + std::to_string(port));
    }
    if (::listen(toOs(listener.handle_), backlog) != 0) {
        throwSocketError("listen");
    }
    return listener;
}

TcpSocket TcpSocket::accept() {
    requireValid("accept");
    TcpSocket client(fromOs(::accept(toOs(handle_), nullptr, nullptr)));
    if (!client.isValid()) {
        throwSocketError("accept");
    }
    client.setNoDelay();
    return client;
}

void TcpSocket::setNoDelay() noexcept {
    const int enable = 1;
    // Best effort: failure only costs latency, never correctness.
    ::setsockopt(toOs(handle_), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enable), sizeof(enable));
}

void TcpSocket::setIoTimeout(std::chrono::milliseconds timeout) {
    requireValid("setIoTimeout");
#ifdef _WIN32
    const DWORD value = static_cast<DWORD>(std::max<long long>(0, timeout.count()));
    const char* option = reinterpret_cast<const char*>(&value);
    const SockLen optionSize = sizeof(value);
#else
    const timeval value = toTimeval(timeout);
    const void* option = &value;
    const SockLen optionSize = sizeof(value);
#endif
    if (::setsockopt(toOs(handle_), SOL_SOCKET, SO_RCVTIMEO, option, optionSize) != 0 ||
        ::setsockopt(toOs(handle_), SOL_SOCKET, SO_SNDTIMEO, option, optionSize) != 0) {
        throwSocketError("setsockopt(timeout)");
    }
}

void TcpSocket::sendAll(const std::uint8_t* data, std::size_t size) {
    requireValid("send");
    std::size_t sent = 0;
    while (sent < size) {
        const auto chunk = static_cast<IoLength>(std::min(size - sent, kMaxIoChunk));
        const auto rc = ::send(toOs(handle_), reinterpret_cast<const char*>(data + sent), chunk, kSendFlags);
        if (rc < 0) {
            if (isInterrupted(lastSocketError())) {
                continue;
            }
            throwSocketError("send");
        }
        sent += static_cast<std::size_t>(rc);
    }
}

void TcpSocket::receiveExact(std::uint8_t* buffer, std::size_t size) {
    requireValid("recv");
    std::size_t received = 0;
    while (received < size) {
        const auto chunk = static_cast<IoLength>(std::min(size - received, kMaxIoChunk));
        const auto rc = ::recv(toOs(handle_), reinterpret_cast<char*>(buffer + received), chunk, 0);
        if (rc == 0) {
            throw ConnectionClosedError("connection closed by peer");
        }
        if (rc < 0) {
            if (isInterrupted(lastSocketError())) {
                continue;
            }
            throwSocketError("recv");
        }
        received += static_cast<std::size_t>(rc);
    }
}

bool TcpSocket::waitReadable(std::chrono::milliseconds timeout) const {
    return !waitForReadable({this}, timeout).empty();
}

std::uint16_t TcpSocket::localPort() const {
    requireValid("getsockname");
    sockaddr_in address{};
    SockLen length = sizeof(address);
    if (::getsockname(toOs(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        throwSocketError("getsockname");
    }
    return ntohs(address.sin_port);
}

std::string TcpSocket::peerAddress() const {
    if (!isValid()) {
        return "<closed>";
    }
    sockaddr_in address{};
    SockLen length = sizeof(address);
    if (::getpeername(toOs(handle_), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return "<unknown>";
    }
    char text[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)) == nullptr) {
        return "<unknown>";
    }
    return std::string(text) + ":" + std::to_string(ntohs(address.sin_port));
}

// ---------------------------------------------------------------------------
// select() based readiness check
// ---------------------------------------------------------------------------
std::vector<std::size_t> waitForReadable(const std::vector<const TcpSocket*>& sockets,
                                         std::chrono::milliseconds timeout) {
    fd_set readSet;
    FD_ZERO(&readSet);

    bool hasSocket = false;
#ifndef _WIN32
    int highestHandle = -1;
#endif
    for (const TcpSocket* socket : sockets) {
        if (socket == nullptr || !socket->isValid()) {
            continue;
        }
#ifndef _WIN32
        if (socket->native() >= FD_SETSIZE) {
            throw NetworkError("socket descriptor exceeds FD_SETSIZE");
        }
        highestHandle = std::max(highestHandle, socket->native());
#endif
        FD_SET(toOs(socket->native()), &readSet);
        hasSocket = true;
    }

    std::vector<std::size_t> ready;
    if (!hasSocket) {
        // Winsock rejects select() with empty sets; behave like a plain wait.
        std::this_thread::sleep_for(timeout);
        return ready;
    }

    timeval tv = toTimeval(timeout);
#ifdef _WIN32
    const int rc = ::select(0, &readSet, nullptr, nullptr, &tv);
#else
    const int rc = ::select(highestHandle + 1, &readSet, nullptr, nullptr, &tv);
#endif
    if (rc < 0) {
        if (isInterrupted(lastSocketError())) {
            return ready;
        }
        throwSocketError("select");
    }
    if (rc == 0) {
        return ready;
    }

    for (std::size_t index = 0; index < sockets.size(); ++index) {
        const TcpSocket* socket = sockets[index];
        if (socket != nullptr && socket->isValid() && FD_ISSET(toOs(socket->native()), &readSet)) {
            ready.push_back(index);
        }
    }
    return ready;
}

}  // namespace ipg::net
