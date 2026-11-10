#include "ttdb_protocol.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

constexpr int MAX_VARS_PER_FRAME = 16;
constexpr int MAX_STACK_DEPTH = 64;
constexpr std::int64_t HEADER_SIZE = 20;
constexpr std::size_t MAX_REPLY_BYTES = 256u * 1024u * 1024u;
constexpr std::int32_t MAX_NAME_BYTES = 15 * 1024 * 1024;

struct Variable
{
    std::string name;
    std::int32_t value;
};

struct Frame
{
    std::string func_name;
    std::int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    std::int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    std::int32_t localCount;
};

struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    std::int32_t stackDepth;
};

struct TdbgReader
{
    std::ifstream file;
    std::int64_t indexOffset = 0;
    std::vector<std::int64_t> index;
};

struct DebugSession
{
    TdbgReader reader;
    std::size_t currentStep = 0;
    std::filesystem::path downloadedFile;

    ~DebugSession()
    {
        if (reader.file.is_open())
            reader.file.close();
        if (!downloadedFile.empty())
        {
            std::error_code ignored;
            std::filesystem::remove(downloadedFile, ignored);
        }
    }
};

void readBytes(std::ifstream& file, char* destination,
               std::streamsize size, std::int64_t& remaining)
{
    if (size < 0 || size > remaining || !file.read(destination, size))
        throw std::runtime_error("Incomplete .tdbg record");
    remaining -= size;
}

template <typename T>
T readNumber(std::ifstream& file, std::int64_t& remaining)
{
    // Our Phase 1 Windows writer stores integers in little-endian order.
    T value{};
    readBytes(file, reinterpret_cast<char*>(&value), sizeof(value), remaining);
    return value;
}

std::string readName(std::ifstream& file, std::int64_t& remaining)
{
    std::int32_t size = readNumber<std::int32_t>(file, remaining);
    if (size < 0 || size > remaining || size > MAX_NAME_BYTES)
        throw std::runtime_error("Invalid name length in .tdbg file");
    std::string name(static_cast<std::size_t>(size), '\0');
    readBytes(file, name.data(), size, remaining);
    return name;
}

int readCount(std::ifstream& file, std::int64_t& remaining, int maximum)
{
    int count = readNumber<std::int32_t>(file, remaining);
    if (count < 0 || count > maximum)
        throw std::runtime_error("Invalid count in .tdbg file");
    return count;
}

void readSnapshotAt(TdbgReader& reader, std::size_t step, Snapshot& snapshot)
{
    if (step >= reader.index.size())
        throw std::runtime_error("Step is outside the session");

    std::int64_t start = reader.index[step];
    std::int64_t end = step + 1 < reader.index.size()
        ? reader.index[step + 1] : reader.indexOffset;
    std::int64_t remaining = end - start;
    std::ifstream& file = reader.file;
    file.clear();
    if (!file.seekg(start))
        throw std::runtime_error("Cannot seek to snapshot");

    snapshot.stackDepth = readCount(file, remaining, MAX_STACK_DEPTH);
    for (int i = 0; i < snapshot.stackDepth; ++i)
    {
        Frame& frame = snapshot.callStack[i];
        frame.func_name = readName(file, remaining);
        frame.argc = readCount(file, remaining, MAX_VARS_PER_FRAME);
        for (int j = 0; j < frame.argc; ++j)
        {
            frame.argv[j].name = readName(file, remaining);
            frame.argv[j].value = readNumber<std::int32_t>(file, remaining);
        }
        frame.returnLine = readNumber<std::int32_t>(file, remaining);
        frame.localCount = readCount(file, remaining, MAX_VARS_PER_FRAME);
        for (int j = 0; j < frame.localCount; ++j)
        {
            frame.locals[j].name = readName(file, remaining);
            frame.locals[j].value = readNumber<std::int32_t>(file, remaining);
        }
    }
    if (remaining != 0)
        throw std::runtime_error("Snapshot does not match its index boundary");
}

void openTdbg(const std::filesystem::path& path, TdbgReader& reader)
{
    std::ifstream& file = reader.file;
    file.open(path, std::ios::binary | std::ios::ate);
    if (!file)
        throw std::runtime_error("Cannot open .tdbg file");
    std::int64_t fileSize = file.tellg();
    if (fileSize < HEADER_SIZE || !file.seekg(0))
        throw std::runtime_error("Invalid .tdbg file size");

    std::int64_t headerRemaining = HEADER_SIZE;
    char magic[4];
    readBytes(file, magic, sizeof(magic), headerRemaining);
    int version = readNumber<std::int32_t>(file, headerRemaining);
    std::int32_t steps = readNumber<std::int32_t>(file, headerRemaining);
    reader.indexOffset = readNumber<std::int64_t>(file, headerRemaining);

    if (std::string(magic, sizeof(magic)) != "TTDB" || version != 1)
        throw std::runtime_error("Unsupported .tdbg signature or version");

    if (steps < 1 || reader.indexOffset < HEADER_SIZE ||
        reader.indexOffset > fileSize ||
        fileSize - reader.indexOffset != 8LL * steps ||
        reader.indexOffset - HEADER_SIZE < 4LL * steps)
        throw std::runtime_error("Invalid .tdbg index");

    if (!file.seekg(reader.indexOffset))
        throw std::runtime_error("Cannot seek to .tdbg index");
    std::int64_t indexRemaining = fileSize - reader.indexOffset;
    reader.index.reserve(steps);
    for (int i = 0; i < steps; ++i)
    {
        std::int64_t offset = readNumber<std::int64_t>(file, indexRemaining);
        if ((i == 0 && offset != HEADER_SIZE) ||
            (i > 0 && offset <= reader.index.back()) ||
            offset >= reader.indexOffset)
            throw std::runtime_error("Invalid snapshot offset");
        reader.index.push_back(offset);
    }

    // Validate every record before a new session can replace the old one.
    Snapshot snapshot{};
    for (std::size_t i = 0; i < reader.index.size(); ++i)
        readSnapshotAt(reader, i, snapshot);
}

void showStep(DebugSession& session, std::size_t step)
{
    Snapshot snapshot{};
    readSnapshotAt(session.reader, step, snapshot);
    session.currentStep = step;

    std::cout << "Step " << step + 1 << '/' << session.reader.index.size()
              << " | stack depth: " << snapshot.stackDepth << '\n';
    if (snapshot.stackDepth == 0)
        std::cout << "Call stack is empty.\n";
    for (int i = 0; i < snapshot.stackDepth; ++i)
    {
        const Frame& frame = snapshot.callStack[i];
        std::cout << "  " << frame.func_name << '\n';
        for (int j = 0; j < frame.argc; ++j)
            std::cout << "    parameter " << frame.argv[j].name
                      << " = " << frame.argv[j].value << '\n';
        for (int j = 0; j < frame.localCount; ++j)
            std::cout << "    local " << frame.locals[j].name
                      << " = " << frame.locals[j].value << '\n';
    }
}

std::filesystem::path makeTemporaryFile()
{
    std::filesystem::path folder = std::filesystem::temp_directory_path();
    wchar_t path[MAX_PATH];
    if (GetTempFileNameW(folder.c_str(), L"ttd", 0, path) == 0)
        throw std::runtime_error("Cannot create a temporary download file");
    return std::filesystem::path(path);
}

unsigned char downloadSession(const char* host, int port,
                              const std::filesystem::path& sourcePath,
                              const std::filesystem::path& downloadPath)
{
    std::ifstream source(sourcePath, std::ios::binary);
    if (!source)
        throw std::runtime_error("Cannot open source file: " + sourcePath.string());
    std::ofstream reply(downloadPath, std::ios::binary);
    if (!reply)
        throw std::runtime_error("Cannot open temporary download");

    SOCKET socket = ttdb::connectTcp(host, port);
    try
    {
        ttdb::setTransferTimeout(socket, 30000);
        ttdb::sendMessage(socket, ttdb::OK, source);
        unsigned char status = ttdb::receiveMessage(socket, reply, MAX_REPLY_BYTES);
        reply.close();
        if (!reply)
            throw std::runtime_error("Cannot finish writing download");
        closesocket(socket);
        return status;
    }
    catch (...)
    {
        closesocket(socket);
        throw;
    }
}

void startSession(std::unique_ptr<DebugSession>& active, const char* host,
                  int port, const std::filesystem::path& source)
{
    auto candidate = std::make_unique<DebugSession>();
    candidate->downloadedFile = makeTemporaryFile();
    unsigned char status = downloadSession(host, port, source, candidate->downloadedFile);
    if (status == ttdb::ERROR_REPLY)
    {
        std::ifstream reply(candidate->downloadedFile, std::ios::binary);
        char text[4096];
        reply.read(text, sizeof(text));
        throw std::runtime_error(
            "Server error: " + std::string(text, static_cast<std::size_t>(reply.gcount())));
    }

    try
    {
        openTdbg(candidate->downloadedFile, candidate->reader);
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error(
            std::string("Server sent a damaged .tdbg file: ") + error.what());
    }

    showStep(*candidate, 0);
    // Only now release the old reader and its downloaded file.
    active = std::move(candidate);
}

void moveBy(const std::unique_ptr<DebugSession>& active, std::int64_t delta)
{
    if (!active)
        throw std::runtime_error("No active session. Use start <source path>.");
    std::int64_t step = static_cast<std::int64_t>(active->currentStep) + delta;
    if (step < 0 || step >= static_cast<std::int64_t>(active->reader.index.size()))
        throw std::runtime_error("Jump is outside the session");
    showStep(*active, static_cast<std::size_t>(step));
}

void printHelp()
{
    std::cout << "start <source path> | next | prv | jmp next <n> | "
                 "jmp prv <n> | help | quit\n";
}

bool runCommand(std::unique_ptr<DebugSession>& active,
                const char* host, int port, const std::string& line)
{
    std::istringstream input(line);
    std::string command;
    if (!(input >> command))
        return true;
    if (command == "ttdb")
        input >> command;

    if (command == "start")
    {
        if (host == nullptr)
            throw std::runtime_error("start is unavailable in offline mode");
        std::string path;
        std::getline(input >> std::ws, path);
        auto last = path.find_last_not_of(" \t\r");
        if (last != std::string::npos)
            path.erase(last + 1);
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        if (path.empty())
            throw std::runtime_error("Usage: start <source path>");
        startSession(active, host, port, path);
        return true;
    }

    std::string extra;
    if (command == "jmp")
    {
        std::string direction;
        std::int64_t count;
        if (!(input >> direction >> count) || (input >> extra) ||
            (direction != "next" && direction != "prv") ||
            count <= 0 || count > 1000000000)
            throw std::runtime_error("Usage: jmp next <n> or jmp prv <n>");
        moveBy(active, direction == "next" ? count : -count);
        return true;
    }

    if (!(input >> extra))
    {
        if (command == "quit" || command == "exit")
            return false;
        if (command == "next")
            moveBy(active, 1);
        else if (command == "prv")
            moveBy(active, -1);
        else if (command == "help")
            printHelp();
        else
        {
            std::cout << "Unknown command.\n";
            printHelp();
        }
        return true;
    }

    throw std::runtime_error("Unexpected arguments. Type help.");
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        std::cerr << "Usage: client.exe <lb_ip> <lb_port>\n"
                     "       client.exe --inspect <file.tdbg>\n";
        return 1;
    }

    try
    {
        std::unique_ptr<ttdb::Winsock> winsock;
        std::unique_ptr<DebugSession> active;
        const char* host = nullptr;
        int port = 0;

        if (std::string(argv[1]) == "--inspect")
        {
            active = std::make_unique<DebugSession>();
            openTdbg(argv[2], active->reader);
            showStep(*active, 0);
        }
        else
        {
            host = argv[1];
            std::string portText = argv[2];
            std::size_t parsed = 0;
            port = std::stoi(portText, &parsed);
            if (parsed != portText.size())
                throw std::runtime_error("Invalid load balancer port");
            winsock = std::make_unique<ttdb::Winsock>();
            ttdb::ipv4Address(host, port);
        }

        printHelp();
        std::string line;
        while (std::cout << "ttdb> " && std::getline(std::cin, line))
        {
            try
            {
                if (!runCommand(active, host, port, line))
                    break;
            }
            catch (const std::exception& error)
            {
                std::cerr << error.what() << '\n';
            }
        }
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Client error: " << error.what() << '\n';
        return 1;
    }
}
