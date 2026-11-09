#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstddef>
#include <stdexcept>
#include <string>

namespace ttdb
{
inline std::runtime_error socketError(const char* action, int code)
{
    return std::runtime_error(
        std::string(action) + " failed (Winsock " + std::to_string(code) + ")");
}

// Create this once in main(), before opening any sockets.
class Winsock
{
public:
    Winsock()
    {
        WSADATA data{};
        int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0)
        {
            throw socketError("WSAStartup", result);
        }
    }

    ~Winsock()
    {
        WSACleanup();
    }

    Winsock(const Winsock&) = delete;
    Winsock& operator=(const Winsock&) = delete;
};

inline sockaddr_in ipv4Address(const char* ip, int port)
{
    if (port < 1 || port > 65535)
    {
        throw std::runtime_error("Port must be between 1 and 65535");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));

    if (InetPtonA(AF_INET, ip, &address.sin_addr) != 1)
    {
        throw std::runtime_error(std::string("Invalid IPv4 address: ") + ip);
    }

    return address;
}

inline SOCKET openTcpSocket()
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
    {
        throw socketError("socket", WSAGetLastError());
    }
    return sock;
}

inline SOCKET connectTcp(const char* ip, int port)
{
    sockaddr_in address = ipv4Address(ip, port);
    SOCKET sock = openTcpSocket();

    if (connect(sock, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR)
    {
        int code = WSAGetLastError();
        closesocket(sock);
        throw socketError("connect", code);
    }

    return sock;
}

inline SOCKET listenTcp(const char* ip, int port, int backlog)
{
    sockaddr_in address = ipv4Address(ip, port);
    SOCKET sock = openTcpSocket();

    if (bind(sock, reinterpret_cast<const sockaddr*>(&address),
             sizeof(address)) == SOCKET_ERROR)
    {
        int code = WSAGetLastError();
        closesocket(sock);
        throw socketError("bind", code);
    }

    if (listen(sock, backlog) == SOCKET_ERROR)
    {
        int code = WSAGetLastError();
        closesocket(sock);
        throw socketError("listen", code);
    }

    return sock;
}

inline void sendAll(SOCKET sock, const char* data, std::size_t length)
{
    while (length > 0)
    {
        // Winsock send() takes an int length, so send bounded chunks.
        int chunk = length > 65536 ? 65536 : static_cast<int>(length);
        int sent = send(sock, data, chunk, 0);

        if (sent == SOCKET_ERROR)
        {
            throw socketError("send", WSAGetLastError());
        }
        if (sent == 0)
        {
            throw std::runtime_error("send made no progress");
        }

        data += sent;
        length -= sent;
    }
}

inline void finishSending(SOCKET sock)
{
    // Signal end of the message while leaving the receive side open.
    if (shutdown(sock, SD_SEND) == SOCKET_ERROR)
    {
        throw socketError("shutdown", WSAGetLastError());
    }
}
}
