#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

using namespace std;

const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2;
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024;
const int IO_BUFFER_SIZE = 64 * 1024;

struct Variable
{
    string name;
    int32_t value;
};

struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
    Variable* callerBindings[MAX_VARS_PER_FRAME];
};

struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

struct TTDBHeader
{
    char magic[4];
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin;
};

struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};

inline void startWinsock()
{
    WSADATA data{};
    int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0)
        throw runtime_error("Cannot initialize Winsock");
}

inline void setSocketTimeout(SOCKET sock, DWORD milliseconds)
{
    const char* value = reinterpret_cast<const char*>(&milliseconds);
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, value,
                   sizeof(milliseconds)) == SOCKET_ERROR ||
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, value,
                   sizeof(milliseconds)) == SOCKET_ERROR)
        throw runtime_error("Cannot set socket timeout");
}

inline SOCKET connectToHost(const char* host, int port)
{
    if (port < 1 || port > 65535)
        throw runtime_error("Invalid port");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    if (InetPtonA(AF_INET, host, &address.sin_addr) != 1)
        throw runtime_error("Invalid IPv4 address");

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
        throw runtime_error("Cannot create socket");
    if (connect(sock, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR)
    {
        closesocket(sock);
        throw runtime_error("Cannot connect to host");
    }
    return sock;
}

inline SOCKET createListenSocket(int port)
{
    if (port < 1 || port > 65535)
        throw runtime_error("Invalid port");
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET)
        throw runtime_error("Cannot create listening socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) == SOCKET_ERROR ||
        listen(sock, 16) == SOCKET_ERROR)
    {
        closesocket(sock);
        throw runtime_error("Cannot bind or listen on port");
    }
    return sock;
}

inline void sendAll(SOCKET sock, const char* data, size_t length)
{
    while (length > 0)
    {
        int chunk = length > IO_BUFFER_SIZE
            ? IO_BUFFER_SIZE : static_cast<int>(length);
        int sent = send(sock, data, chunk, 0);
        if (sent <= 0)
            throw runtime_error("Cannot send socket data");
        data += sent;
        length -= sent;
    }
}

inline void finishSending(SOCKET sock)
{
    if (shutdown(sock, SD_SEND) == SOCKET_ERROR)
        throw runtime_error("Cannot finish socket upload");
}

class Crc32
{
    uint32_t state;

public:
    Crc32() : state(0xFFFFFFFFu) {}

    void update(unsigned char byte)
    {
        state ^= byte;
        for (int bit = 0; bit < 8; ++bit)
        {
            if (state & 1u)
                state = (state >> 1) ^ 0xEDB88320u;
            else
                state >>= 1;
        }
    }

    void update(const char* bytes, size_t length)
    {
        for (size_t i = 0; i < length; ++i)
            update(static_cast<unsigned char>(bytes[i]));
    }

    uint32_t value() const
    {
        return state ^ 0xFFFFFFFFu;
    }
};

class TrailerChecker
{
    Crc32 crc;
    unsigned char tail[4];
    int held;

public:
    TrailerChecker() : tail{}, held(0) {}

    bool push(unsigned char incoming, unsigned char& released)
    {
        if (held < 4)
        {
            tail[held++] = incoming;
            return false;
        }
        released = tail[0];
        tail[0] = tail[1];
        tail[1] = tail[2];
        tail[2] = tail[3];
        tail[3] = incoming;
        crc.update(released);
        return true;
    }

    uint32_t verify() const
    {
        if (held != 4)
            throw runtime_error("Missing checksum trailer");
        uint32_t expected = 0;
        for (int i = 0; i < 4; ++i)
            expected = (expected << 8) | tail[i];
        if (crc.value() != expected)
            throw runtime_error("Checksum mismatch");
        return expected;
    }
};

inline void sendTrailer(SOCKET sock, uint32_t checksum)
{
    unsigned char bytes[4];
    bytes[0] = static_cast<unsigned char>(checksum >> 24);
    bytes[1] = static_cast<unsigned char>(checksum >> 16);
    bytes[2] = static_cast<unsigned char>(checksum >> 8);
    bytes[3] = static_cast<unsigned char>(checksum);
    sendAll(sock, reinterpret_cast<const char*>(bytes), 4);
}

inline void sendFile(SOCKET sock, FILE* file, unsigned char status)
{
    if (file == nullptr || status > 1)
        throw runtime_error("Invalid file message");
    Crc32 crc;
    crc.update(status);
    sendAll(sock, reinterpret_cast<const char*>(&status), 1);
    char buffer[IO_BUFFER_SIZE];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        crc.update(buffer, count);
        sendAll(sock, buffer, count);
    }
    if (ferror(file))
        throw runtime_error("Cannot read outgoing file");
    sendTrailer(sock, crc.value());
    finishSending(sock);
}

inline void sendError(SOCKET sock, const string& message)
{
    unsigned char status = 1;
    Crc32 crc;
    crc.update(status);
    crc.update(message.data(), message.size());
    sendAll(sock, reinterpret_cast<const char*>(&status), 1);
    sendAll(sock, message.data(), message.size());
    sendTrailer(sock, crc.value());
    finishSending(sock);
}

inline unsigned char receiveFile(SOCKET sock, FILE* file, uint64_t maxBytes)
{
    if (file == nullptr)
        throw runtime_error("Invalid receiving file");
    TrailerChecker checker;
    int status = -1;
    uint64_t total = 0;
    char input[IO_BUFFER_SIZE];
    char output[IO_BUFFER_SIZE];

    while (true)
    {
        int received = recv(sock, input, sizeof(input), 0);
        if (received == SOCKET_ERROR)
            throw runtime_error("Socket receive failed");
        if (received == 0)
            break;
        int used = 0;
        for (int i = 0; i < received; ++i)
        {
            unsigned char byte;
            if (!checker.push(static_cast<unsigned char>(input[i]), byte))
                continue;
            if (status == -1)
            {
                status = byte;
                if (status > 1)
                    throw runtime_error("Invalid message status");
            }
            else
            {
                if (maxBytes != 0 && total >= maxBytes)
                    throw runtime_error("Incoming file is too large");
                output[used++] = static_cast<char>(byte);
                ++total;
            }
        }
        if (fwrite(output, 1, used, file) != static_cast<size_t>(used))
            throw runtime_error("Cannot save received data");
    }
    if (status == -1)
        throw runtime_error("Message has no status byte");
    checker.verify();
    return static_cast<unsigned char>(status);
}

inline void relayStream(SOCKET from, SOCKET to)
{
    TrailerChecker checker;
    char input[IO_BUFFER_SIZE];
    char output[IO_BUFFER_SIZE];
    bool hasStatus = false;
    while (true)
    {
        int received = recv(from, input, sizeof(input), 0);
        if (received == SOCKET_ERROR)
            throw runtime_error("Socket receive failed");
        if (received == 0)
            break;
        int used = 0;
        for (int i = 0; i < received; ++i)
        {
            unsigned char byte;
            if (checker.push(static_cast<unsigned char>(input[i]), byte))
            {
                output[used++] = static_cast<char>(byte);
                hasStatus = true;
            }
        }
        if (used > 0)
            sendAll(to, output, used);
    }
    if (!hasStatus)
        throw runtime_error("Message has no status byte");
    sendTrailer(to, checker.verify());
    finishSending(to);
}
