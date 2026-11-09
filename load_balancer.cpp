#include "ttdb_protocol.h"

#include <iostream>
#include <string>

constexpr int LISTEN_PORT = 5000;
constexpr int LISTEN_BACKLOG = 16;
constexpr std::size_t MAX_UPLOAD_BYTES = 15u * 1024u * 1024u;
constexpr std::size_t MAX_REPLY_BYTES = 256u * 1024u * 1024u;

// Relay the status and payload as they arrive. Forward the checksum
// trailer only if it matches, so the next receiver can verify the stream.
void relayChecked(SOCKET from, SOCKET to, std::size_t maxPayloadBytes)
{
    std::uint32_t verifiedCrc =
        ttdb::receiveChecked(from, maxPayloadBytes,
            [&](const char* bytes, std::size_t count)
            {
                ttdb::sendAll(to, bytes, count);
            });

    ttdb::sendTrailer(to, verifiedCrc);
    ttdb::finishSending(to);
}

void relayOneClient(SOCKET client, const char* serverIp, int serverPort)
{
    SOCKET backend;
    try
    {
        backend = ttdb::connectTcp(serverIp, serverPort);
    }
    catch (const std::exception& error)
    {
        std::cerr << "Backend connection: " << error.what() << '\n';
        ttdb::sendErrorMessage(client, "Backend server unavailable");
        return;
    }

    try
    {
        ttdb::setTransferTimeout(backend, 30000);
        relayChecked(client, backend, MAX_UPLOAD_BYTES);
    }
    catch (const std::exception& error)
    {
        // The backend did not receive a verified trailer. It must not
        // interpret this upload. The client has not received a reply yet.
        closesocket(backend);
        ttdb::sendErrorMessage(client, error.what());
        return;
    }

    try
    {
        relayChecked(backend, client, MAX_REPLY_BYTES);
    }
    catch (const std::exception& error)
    {
        // A reply might already have started. Do not add an error frame
        // to it. Close without a trailer so the client rejects the reply.
        closesocket(backend);
        throw;
    }

    closesocket(backend);
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: load_balancer.exe <server_ip> <server_port>\n";
        return 1;
    }

    try
    {
        const char* serverIp = argv[1];
        std::string portText = argv[2];
        std::size_t parsed = 0;
        int serverPort = std::stoi(portText, &parsed);
        if (parsed != portText.size() || serverPort < 1 || serverPort > 65535)
        {
            throw std::runtime_error("Invalid backend port");
        }

        ttdb::Winsock winsock;
        // Validate the address before accepting clients.
        ttdb::ipv4Address(serverIp, serverPort);
        SOCKET listener = ttdb::listenTcp("127.0.0.1", LISTEN_PORT, LISTEN_BACKLOG);
        std::cout << "Load balancer listening on 127.0.0.1:"
                  << LISTEN_PORT << " -> "
                  << serverIp << ':' << serverPort << std::endl;

        while (true)
        {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
            {
                int code = WSAGetLastError();
                closesocket(listener);
                throw ttdb::socketError("accept", code);
            }

            try
            {
                ttdb::setTransferTimeout(client, 30000);
                relayOneClient(client, serverIp, serverPort);
            }
            catch (const std::exception& error)
            {
                std::cerr << "Relay error: " << error.what() << '\n';
            }
            closesocket(client);
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << "Load balancer error: " << error.what() << '\n';
        return 1;
    }
}
