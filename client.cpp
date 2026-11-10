#include "helper.h"
#include <vector>
#include <sstream>

string LB_HOST = "127.0.0.1";
int LB_PORT = 5000;
const int MAX_JUMP = 1000000000;

struct TdbgReader
{
    FILE* file;
    TTDBHeader header;
    vector<int64_t> index;
};

struct DebugSession
{
    bool active;
    TdbgReader reader;
    int64_t currentStep;
    string filePath;
};

void readBytes(FILE* file, void* data, size_t size, int64_t& remaining)
{
    if (size > static_cast<uint64_t>(remaining) ||
        fread(data, 1, size, file) != size)
        throw runtime_error("Incomplete debugger record");
    remaining -= size;
}

int32_t readInt32(FILE* file, int64_t& remaining)
{
    int32_t value;
    readBytes(file, &value, sizeof(value), remaining);
    return value;
}

int64_t readInt64(FILE* file, int64_t& remaining)
{
    int64_t value;
    readBytes(file, &value, sizeof(value), remaining);
    return value;
}

string readString(FILE* file, int64_t& remaining)
{
    int32_t size = readInt32(file, remaining);
    if (size < 0 || size > remaining ||
        static_cast<uint64_t>(size) > MAX_SOURCE_BYTES)
        throw runtime_error("Invalid debugger string length");
    string text(size, '\0');
    if (size > 0)
        readBytes(file, &text[0], size, remaining);
    return text;
}

int32_t readCount(FILE* file, int64_t& remaining, int maximum)
{
    int32_t count = readInt32(file, remaining);
    if (count < 0 || count > maximum)
        throw runtime_error("Invalid debugger count");
    return count;
}

void closeTdbg(TdbgReader& reader)
{
    if (reader.file != nullptr)
        fclose(reader.file);
    reader.file = nullptr;
    reader.index.clear();
}

Snapshot* readSnapshotAt(TdbgReader& reader, int32_t step)
{
    if (reader.file == nullptr || step < 0 ||
        step >= reader.header.stepCount)
        return nullptr;

    Snapshot* snapshot = nullptr;
    try
    {
        int64_t start = reader.index[step];
        int64_t end = step + 1 < reader.header.stepCount
            ? reader.index[step + 1] : reader.header.indexOffset;
        int64_t remaining = end - start;
        if (remaining < 4 ||
            fseek(reader.file, static_cast<long>(start), SEEK_SET) != 0)
            throw runtime_error("Invalid snapshot boundary");

        snapshot = new Snapshot{};
        snapshot->stackDepth =
            readCount(reader.file, remaining, MAX_STACK_DEPTH);
        for (int i = 0; i < snapshot->stackDepth; ++i)
        {
            Frame& frame = snapshot->callStack[i];
            frame.func_name = readString(reader.file, remaining);
            frame.argc = readCount(reader.file, remaining, MAX_VARS_PER_FRAME);
            for (int j = 0; j < frame.argc; ++j)
            {
                frame.argv[j].name = readString(reader.file, remaining);
                frame.argv[j].value = readInt32(reader.file, remaining);
            }
            frame.returnLine = readInt32(reader.file, remaining);
            frame.localCount =
                readCount(reader.file, remaining, MAX_VARS_PER_FRAME);
            for (int j = 0; j < frame.localCount; ++j)
            {
                frame.locals[j].name = readString(reader.file, remaining);
                frame.locals[j].value = readInt32(reader.file, remaining);
            }
        }
        if (remaining != 0)
            throw runtime_error("Snapshot does not match its index");
        return snapshot;
    }
    catch (const exception&)
    {
        delete snapshot;
        return nullptr;
    }
}

bool openTdbg(const char* path, TdbgReader& reader)
{
    reader.file = fopen(path, "rb");
    if (reader.file == nullptr)
        return false;

    try
    {
        if (fseek(reader.file, 0, SEEK_END) != 0)
            throw runtime_error("Cannot read debugger size");
        long fileSize = ftell(reader.file);
        if (fileSize < 20 || fseek(reader.file, 0, SEEK_SET) != 0)
            throw runtime_error("Invalid debugger size");

        int64_t remaining = 20;
        readBytes(reader.file, reader.header.magic, 4, remaining);
        reader.header.version = readInt32(reader.file, remaining);
        reader.header.stepCount = readInt32(reader.file, remaining);
        reader.header.indexOffset = readInt64(reader.file, remaining);

        if (string(reader.header.magic, 4) != "TTDB" ||
            reader.header.version != 1)
            throw runtime_error("Invalid debugger header");
        int32_t steps = reader.header.stepCount;
        int64_t indexOffset = reader.header.indexOffset;
        if (steps < 1 || indexOffset < 20 || indexOffset > fileSize ||
            fileSize - indexOffset != 8LL * steps ||
            indexOffset - 20 < 4LL * steps)
            throw runtime_error("Invalid debugger index");

        if (fseek(reader.file, static_cast<long>(indexOffset), SEEK_SET) != 0)
            throw runtime_error("Cannot read debugger index");
        remaining = fileSize - indexOffset;
        reader.index.resize(steps);
        for (int i = 0; i < steps; ++i)
        {
            int64_t offset = readInt64(reader.file, remaining);
            if ((i == 0 && offset != 20) ||
                (i > 0 && offset <= reader.index[i - 1]) ||
                offset >= indexOffset)
                throw runtime_error("Invalid snapshot offset");
            reader.index[i] = offset;
        }
        for (int i = 0; i < steps; ++i)
        {
            Snapshot* snapshot = readSnapshotAt(reader, i);
            if (snapshot == nullptr)
                throw runtime_error("Invalid snapshot data");
            delete snapshot;
        }
        return true;
    }
    catch (const exception&)
    {
        closeTdbg(reader);
        return false;
    }
}

void initSession(DebugSession& session)
{
    session.active = false;
    session.reader.file = nullptr;
    session.reader.header = {};
    session.reader.index.clear();
    session.currentStep = 0;
    session.filePath.clear();
}

void closeSession(DebugSession& session)
{
    closeTdbg(session.reader);
    session.active = false;
    session.currentStep = 0;
}

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& snapshot)
{
    cout << "Step " << step + 1 << '/' << totalSteps
         << " | stack depth: " << snapshot.stackDepth << '\n';
    if (snapshot.stackDepth == 0)
        cout << "Call stack is empty.\n";
    for (int i = 0; i < snapshot.stackDepth; ++i)
    {
        const Frame& frame = snapshot.callStack[i];
        cout << "  " << frame.func_name << '\n';
        for (int j = 0; j < frame.argc; ++j)
            cout << "    parameter " << frame.argv[j].name
                 << " = " << frame.argv[j].value << '\n';
        for (int j = 0; j < frame.localCount; ++j)
            cout << "    local " << frame.locals[j].name
                 << " = " << frame.locals[j].value << '\n';
    }
}

bool showCurrent(DebugSession& session)
{
    if (!session.active)
    {
        cout << "No active session. Use start <source path>.\n";
        return false;
    }
    Snapshot* snapshot = readSnapshotAt(
        session.reader, static_cast<int32_t>(session.currentStep));
    if (snapshot == nullptr)
    {
        cout << "Damaged .tdbg file.\n";
        return false;
    }
    printSnapshot(static_cast<int32_t>(session.currentStep),
                  session.reader.header.stepCount, *snapshot);
    delete snapshot;
    return true;
}

bool moveBy(DebugSession& session, int32_t delta)
{
    if (!session.active)
        return showCurrent(session);
    int64_t target = session.currentStep + delta;
    if (target < 0 || target >= session.reader.header.stepCount)
    {
        cout << "Jump is outside the session.\n";
        return false;
    }
    int64_t previous = session.currentStep;
    session.currentStep = target;
    if (!showCurrent(session))
    {
        session.currentStep = previous;
        return false;
    }
    return true;
}

bool requestDebugFile(const char* sourcePath, const char* outputPath,
                      string& outError)
{
    FILE* source = nullptr;
    FILE* output = nullptr;
    SOCKET sock = INVALID_SOCKET;
    try
    {
        source = fopen(sourcePath, "rb");
        if (source == nullptr)
            throw runtime_error("Cannot open source file");
        output = fopen(outputPath, "wb");
        if (output == nullptr)
            throw runtime_error("Cannot open download file");
        sock = connectToHost(LB_HOST.c_str(), LB_PORT);
        setSocketTimeout(sock, 30000);
        sendFile(sock, source, 0);
        fclose(source);
        source = nullptr;
        unsigned char status = receiveFile(sock, output, 0);
        int result = fclose(output);
        output = nullptr;
        closesocket(sock);
        sock = INVALID_SOCKET;
        if (result != 0)
            throw runtime_error("Cannot finish saving download");
        if (status == 1)
        {
            output = fopen(outputPath, "rb");
            if (output == nullptr)
                throw runtime_error("Cannot read server error");
            char text[4096];
            size_t count = fread(text, 1, sizeof(text), output);
            outError.assign(text, count);
            fclose(output);
            output = nullptr;
            remove(outputPath);
            return false;
        }
        return true;
    }
    catch (const exception& error)
    {
        if (source != nullptr)
            fclose(source);
        if (output != nullptr)
            fclose(output);
        if (sock != INVALID_SOCKET)
            closesocket(sock);
        remove(outputPath);
        outError = error.what();
        return false;
    }
}

bool cmdStart(DebugSession& session, const string& sourcePath)
{
    string prefix = "ttdb_" + to_string(GetCurrentProcessId());
    string firstPath = prefix + "_a.tdbg";
    string secondPath = prefix + "_b.tdbg";
    string newPath = session.filePath == firstPath ? secondPath : firstPath;
    string error;
    if (!requestDebugFile(sourcePath.c_str(), newPath.c_str(), error))
    {
        cout << error << '\n';
        return false;
    }

    TdbgReader candidate{};
    if (!openTdbg(newPath.c_str(), candidate))
    {
        remove(newPath.c_str());
        cout << "Server sent a damaged .tdbg file.\n";
        return false;
    }

    string previousPath = session.filePath;
    closeSession(session);
    session.reader.file = candidate.file;
    session.reader.header = candidate.header;
    session.reader.index.swap(candidate.index);
    candidate.file = nullptr;
    session.filePath = newPath;
    session.currentStep = 0;
    session.active = true;
    if (!previousPath.empty())
        remove(previousPath.c_str());
    return showCurrent(session);
}

bool cmdNext(DebugSession& session) { return moveBy(session, 1); }
bool cmdPrv(DebugSession& session) { return moveBy(session, -1); }
bool cmdJmpNext(DebugSession& session, int32_t n) { return moveBy(session, n); }
bool cmdJmpPrv(DebugSession& session, int32_t n) { return moveBy(session, -n); }

void printHelp()
{
    cout << "start <source path> | next | prv | jmp next <n> | "
            "jmp prv <n> | help | quit\n";
}

bool runCommand(DebugSession& session, const string& line, bool& quitRequested)
{
    istringstream input(line);
    string command;
    if (!(input >> command))
        return true;
    if (command == "ttdb")
        input >> command;
    if (command == "start")
    {
        string path;
        getline(input >> ws, path);
        size_t last = path.find_last_not_of(" \t\r");
        if (last != string::npos)
            path.erase(last + 1);
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        if (path.empty())
        {
            cout << "Usage: start <source path>\n";
            return false;
        }
        return cmdStart(session, path);
    }
    string extra;
    if (command == "jmp")
    {
        string direction;
        int64_t count;
        if (!(input >> direction >> count) || (input >> extra) ||
            count <= 0 || count > MAX_JUMP)
        {
            printHelp();
            return false;
        }
        if (direction == "next")
            return cmdJmpNext(session, static_cast<int32_t>(count));
        if (direction == "prv")
            return cmdJmpPrv(session, static_cast<int32_t>(count));
        printHelp();
        return false;
    }
    if (input >> extra)
    {
        printHelp();
        return false;
    }
    if (command == "next")
        return cmdNext(session);
    if (command == "prv")
        return cmdPrv(session);
    if (command == "help")
    {
        printHelp();
        return true;
    }
    if (command == "quit" || command == "exit")
    {
        quitRequested = true;
        return true;
    }
    cout << "Unknown command.\n";
    printHelp();
    return false;
}

int main(int argc, char* argv[])
{
    if (argc != 3)
    {
        cerr << "Usage: client.exe <lb_ip> <lb_port>\n";
        return 1;
    }
    bool socketsStarted = false;
    DebugSession session;
    initSession(session);
    try
    {
        LB_HOST = argv[1];
        string portText = argv[2];
        size_t parsed = 0;
        LB_PORT = stoi(portText, &parsed);
        if (parsed != portText.size() || LB_PORT < 1 || LB_PORT > 65535)
            throw runtime_error("Invalid load balancer port");
        startWinsock();
        socketsStarted = true;
        printHelp();
        bool quitRequested = false;
        string line;
        while (!quitRequested && cout << "ttdb> " && getline(cin, line))
        {
            try
            {
                runCommand(session, line, quitRequested);
            }
            catch (const exception& error)
            {
                cout << error.what() << '\n';
            }
        }
        closeSession(session);
        WSACleanup();
        return 0;
    }
    catch (const exception& error)
    {
        closeSession(session);
        if (socketsStarted)
            WSACleanup();
        cerr << error.what() << '\n';
        return 1;
    }
}
