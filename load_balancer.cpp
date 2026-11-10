#include "helper.h"
#include <windows.h>
#include <iostream>
#include <string>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <ctime>
#include <thread>
#include <mutex>

using namespace std;

const int LB_PORT = 5000;
const int MAX_PENDING_CLIENTS = 8;
const int STARTUP_MAX_ATTEMPTS = 50;
const int STARTUP_POLL_INTERVAL_MS = 100;

string serverHost = "127.0.0.1";
int serverPort = 5001;
volatile sig_atomic_t flag = 1;

template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* front;
    Node* rear;
    int count;

public:
    Queue() : front(nullptr), rear(nullptr), count(0) {}
    Queue(const Queue&) = delete;
    Queue& operator=(const Queue&) = delete;

    ~Queue()
    {
        while (front != nullptr)
        {
            Node* old = front;
            front = front->next;
            delete old;
        }
    }

    void enqueue(const T& value)
    {
        Node* node = new Node{value, nullptr};
        if (rear == nullptr)
            front = node;
        else
            rear->next = node;
        rear = node;
        ++count;
    }

    T dequeue()
    {
        if (isEmpty())
            throw underflow_error("Queue is empty");
        T value = front->data;
        Node* old = front;
        front = front->next;
        if (front == nullptr)
            rear = nullptr;
        delete old;
        --count;
        return value;
    }

    bool isEmpty() const { return count == 0; }
    int size() const { return count; }
};

struct ClientInfo
{
    SOCKET sock;
    string address;
    int port;
    time_t arrivedAt;
};

Queue<ClientInfo> pendingClients;
mutex queueMutex;

bool acceptorRunning = true;

bool keepAccepting()
{
    lock_guard<mutex> lock(queueMutex);
    return acceptorRunning;
}

void handler(int signalNumber)
{
    (void)signalNumber;
    flag = 0;
}

SOCKET connectToServer(const char* host, int port)
{
    try
    {
        return connectToHost(host, port);
    }
    catch (const exception&)
    {
        return INVALID_SOCKET;
    }
}

bool startServer(int port)
{
    FILE* file = fopen("server.exe", "rb");
    if (file == nullptr)
        return false;
    fclose(file);

    string command = "start \"\" /B \".\\server.exe\" " + to_string(port);
    return system(command.c_str()) == 0;
}

SOCKET connectToServerWithWakeup()
{
    SOCKET server = connectToServer(serverHost.c_str(), serverPort);
    if (server != INVALID_SOCKET)
        return server;

    if (serverHost != "127.0.0.1")
        return INVALID_SOCKET;
    if (!startServer(serverPort))
        return INVALID_SOCKET;

    time_t started = time(nullptr);
    for (int attempt = 0; attempt < STARTUP_MAX_ATTEMPTS && flag; ++attempt)
    {
        if (difftime(time(nullptr), started) >= 5)
            break;
        Sleep(STARTUP_POLL_INTERVAL_MS);
        server = connectToServer(serverHost.c_str(), serverPort);
        if (server != INVALID_SOCKET)
            return server;
    }
    return INVALID_SOCKET;
}

void relayOneClient(ClientInfo& client)
{
    SOCKET server = connectToServerWithWakeup();
    if (server == INVALID_SOCKET)
    {
        sendError(client.sock, "backend server failed to start");
        return;
    }
    try
    {
        setSocketTimeout(server, 30000);
        relayStream(client.sock, server);
    }
    catch (const exception& error)
    {
        closesocket(server);
        sendError(client.sock, error.what());
        return;
    }
    try
    {
        relayStream(server, client.sock);
    }
    catch (...)
    {

        closesocket(server);
        throw;
    }
    closesocket(server);
}

void acceptLoop(SOCKET listenSocket)
{
    while (keepAccepting())
    {

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listenSocket, &readable);
        timeval wait{0, 100000};
        int ready = select(0, &readable, nullptr, nullptr, &wait);
        if (ready == 0)
            continue;
        if (ready == SOCKET_ERROR)
        {
            cerr << "Listener error: " << WSAGetLastError() << '\n';
            return;
        }

        sockaddr_in address{};
        int addressSize = sizeof(address);
        SOCKET socket = accept(listenSocket,
            reinterpret_cast<sockaddr*>(&address), &addressSize);
        if (socket == INVALID_SOCKET)
        {
            cerr << "Accept error: " << WSAGetLastError() << '\n';
            continue;
        }

        bool queued = false;
        try
        {
            setSocketTimeout(socket, 30000);
            char ip[INET_ADDRSTRLEN]{};
            InetNtopA(AF_INET, &address.sin_addr, ip, sizeof(ip));
            ClientInfo client{socket, ip, ntohs(address.sin_port), time(nullptr)};
            {
                lock_guard<mutex> lock(queueMutex);
                if (acceptorRunning && pendingClients.size() < MAX_PENDING_CLIENTS)
                {
                    pendingClients.enqueue(client);
                    queued = true;
                }
            }
            if (!queued)
            {
                setSocketTimeout(socket, 1000);
                sendError(socket, "server busy, try again later");
            }
        }
        catch (const exception& error)
        {
            cerr << error.what() << '\n';
        }
        if (!queued)
            closesocket(socket);
    }
}

void dispatcherLoop()
{
    while (flag)
    {
        ClientInfo client{};
        bool foundClient = false;
        {
            lock_guard<mutex> lock(queueMutex);
            if (!pendingClients.isEmpty())
            {
                client = pendingClients.dequeue();
                foundClient = true;
            }
        }
        if (!foundClient)
        {
            Sleep(100);
            continue;
        }
        if (difftime(time(nullptr), client.arrivedAt) >= 30)
        {
            cout << "Dropped expired client " << client.port << '\n';
            closesocket(client.sock);
            continue;
        }
        try
        {
            cout << "Serving client " << client.port << '\n';
            relayOneClient(client);
        }
        catch (const exception& error)
        {
            cerr << "Relay error: " << error.what() << '\n';
        }
        closesocket(client.sock);
    }
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        cerr << "Usage: load_balancer.exe <server_ip> <server_port>\n";
        return 1;
    }
    bool socketsStarted = false;
    try
    {
        serverHost = argv[1];
        string portText = argv[2];
        size_t parsed = 0;
        serverPort = stoi(portText, &parsed);
        if (parsed != portText.size() || serverPort < 1 || serverPort > 65535)
            throw runtime_error("Invalid server port");

        startWinsock();
        socketsStarted = true;
        signal(SIGINT, handler);
        signal(SIGBREAK, handler);
        SOCKET listenSocket = createListenSocket(LB_PORT);
        cout << "Load balancer listening on port " << LB_PORT << endl;
        thread acceptor;
        try
        {
            acceptor = thread(acceptLoop, listenSocket);
            dispatcherLoop();
        }
        catch (...)
        {
            {
                lock_guard<mutex> lock(queueMutex);
                acceptorRunning = false;
            }
            if (acceptor.joinable())
                acceptor.join();
            closesocket(listenSocket);
            while (!pendingClients.isEmpty())
                closesocket(pendingClients.dequeue().sock);
            throw;
        }
        {
            lock_guard<mutex> lock(queueMutex);
            acceptorRunning = false;
        }
        acceptor.join();
        closesocket(listenSocket);
        while (!pendingClients.isEmpty())
            closesocket(pendingClients.dequeue().sock);
        WSACleanup();
        cout << "Load balancer stopped.\n";
        return 0;
    }
    catch (const exception& error)
    {
        if (socketsStarted)
            WSACleanup();
        cerr << error.what() << '\n';
        return 1;
    }
}
