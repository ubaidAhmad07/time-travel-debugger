#pragma once

#include "winsock_helpers.h"

#include <cstdint>
#include <istream>
#include <ostream>
#include <sstream>

namespace ttdb
{
constexpr unsigned char OK = 0;
constexpr unsigned char ERROR_REPLY = 1;
constexpr std::size_t BUFFER_SIZE = 64 * 1024;

class Crc32
{
    std::uint32_t state = 0xFFFFFFFFu;

public:
    void update(unsigned char byte)
    {
        state ^= byte;
        for (int bit = 0; bit < 8; ++bit)
        {
            state = (state & 1u)
                ? (state >> 1) ^ 0xEDB88320u
                : state >> 1;
        }
    }

    void update(const char* bytes, std::size_t count)
    {
        for (std::size_t i = 0; i < count; ++i)
        {
            update(static_cast<unsigned char>(bytes[i]));
        }
    }

    std::uint32_t value() const
    {
        return state ^ 0xFFFFFFFFu;
    }
};

// Keep the newest four bytes aside until the message ends.
class TrailerChecker
{
    Crc32 crc;
    unsigned char tail[4]{};
    std::size_t held = 0;

public:
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

    std::uint32_t verify() const
    {
        if (held != 4)
        {
            throw std::runtime_error("Missing checksum trailer");
        }

        std::uint32_t expected = 0;
        for (unsigned char byte : tail)
        {
            expected = (expected << 8) | byte;
        }

        if (crc.value() != expected)
        {
            throw std::runtime_error("Checksum mismatch");
        }
        return expected;
    }
};

inline void sendTrailer(SOCKET sock, std::uint32_t crc)
{
    // Most significant byte first: network byte order.
    unsigned char bytes[4] = {
        static_cast<unsigned char>(crc >> 24),
        static_cast<unsigned char>(crc >> 16),
        static_cast<unsigned char>(crc >> 8),
        static_cast<unsigned char>(crc)
    };
    sendAll(sock, reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

inline void setTransferTimeout(SOCKET sock, DWORD milliseconds)
{
    const char* value = reinterpret_cast<const char*>(&milliseconds);
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, value,
                   sizeof(milliseconds)) == SOCKET_ERROR ||
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, value,
                   sizeof(milliseconds)) == SOCKET_ERROR)
    {
        throw socketError("setsockopt timeout", WSAGetLastError());
    }
}

inline void sendMessage(SOCKET sock, unsigned char status, std::istream& body)
{
    Crc32 crc;
    crc.update(status);
    sendAll(sock, reinterpret_cast<const char*>(&status), 1);

    char buffer[BUFFER_SIZE];
    while (body.read(buffer, sizeof(buffer)) || body.gcount() > 0)
    {
        std::size_t count = static_cast<std::size_t>(body.gcount());
        crc.update(buffer, count);
        sendAll(sock, buffer, count);
    }

    if (body.bad() || !body.eof())
    {
        throw std::runtime_error("Cannot read message payload");
    }

    sendTrailer(sock, crc.value());
    finishSending(sock);
}

inline void sendErrorMessage(SOCKET sock, const std::string& message)
{
    std::istringstream body(message);
    sendMessage(sock, ERROR_REPLY, body);
}

// Pass status + payload to the consumer in chunks. The consumer must treat
// the data as provisional until this function returns a verified CRC.
template <typename Consumer>
std::uint32_t receiveChecked(
    SOCKET sock, std::size_t maxPayloadBytes, Consumer consume)
{
    TrailerChecker checker;
    std::size_t payloadBytes = 0;
    bool sawStatus = false;
    char input[BUFFER_SIZE];
    char output[BUFFER_SIZE];

    while (true)
    {
        int count = recv(sock, input, sizeof(input), 0);
        if (count == SOCKET_ERROR)
        {
            throw socketError("recv", WSAGetLastError());
        }
        if (count == 0)
        {
            break;
        }

        std::size_t used = 0;
        for (int i = 0; i < count; ++i)
        {
            unsigned char byte = 0;
            if (!checker.push(static_cast<unsigned char>(input[i]), byte))
            {
                continue;
            }

            if (!sawStatus)
            {
                sawStatus = true;
            }
            else
            {
                if (payloadBytes == maxPayloadBytes)
                {
                    throw std::runtime_error("Message payload is too large");
                }
                ++payloadBytes;
            }

            output[used++] = static_cast<char>(byte);
        }

        if (used > 0)
        {
            consume(output, used);
        }
    }

    if (!sawStatus)
    {
        throw std::runtime_error("Message has no status byte");
    }
    return checker.verify();
}

inline unsigned char receiveMessage(
    SOCKET sock, std::ostream& body, std::size_t maxPayloadBytes)
{
    unsigned char status = 0;
    bool first = true;

    receiveChecked(sock, maxPayloadBytes,
        [&](const char* bytes, std::size_t count)
        {
            if (first)
            {
                status = static_cast<unsigned char>(bytes[0]);
                if (status != OK && status != ERROR_REPLY)
                {
                    throw std::runtime_error("Invalid message status");
                }
                first = false;
                ++bytes;
                --count;
            }

            body.write(bytes, count);
            if (!body)
            {
                throw std::runtime_error("Cannot save message payload");
            }
        });

    return status;
}
}
