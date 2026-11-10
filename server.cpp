#include "helper.h"

#include <iostream>
#include <string>
#include <fstream>

#include <cstdio>
#include <stdexcept>
#include <sstream>
using namespace std;

template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

    public:
    Stack() : top(nullptr), count(0)
        {
        }

    Stack(const Stack&) = delete;
    Stack& operator=(const Stack&) = delete;

    ~Stack()
    {
        while (top != nullptr)
        {
            Node* oldTop = top;
            top = top->next;
            delete oldTop;
        }
    }

    bool isEmpty() const
    {
        return top == nullptr;
    }

    void push(const T& value)
    {
        if (count >= 64)
        {
            throw std::overflow_error("Stack depth limit reached");
        }

        Node* newNode = new Node{value, top};
        top = newNode;
        ++count;
    }
    T& peek()
    {
        if (isEmpty())
        {
            throw std::underflow_error("Cannot peek at an empty stack");
        }

        return top->data;
    }
    T pop()
    {
        if (isEmpty())
        {
            throw std::underflow_error("Cannot pop an empty stack");
        }

        T value = top->data;
        Node* oldTop = top;

        top = oldTop->next;
        delete oldTop;
        --count;

        return value;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {

        Node* current = top;
        int32_t written = 0;

        while (current != nullptr && written < maxLen)
        {
            out[written] = current->data;
            current = current->next;
            ++written;
        }

        return written;
    }
};

struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};

class Timeline
{
    TimelineNode* head;
    TimelineNode* tail;
    int32_t stepCount;

public:
    Timeline()
        : head(nullptr), tail(nullptr), stepCount(0)
    {
    }

    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    ~Timeline()
    {
        TimelineNode* current = head;

        while (current != nullptr)
        {
            TimelineNode* nextNode = current->next;

            delete current->data;
            delete current;

            current = nextNode;
        }
    }

    void record(Snapshot* snapshot)
    {
        if (snapshot == nullptr)
        {
            throw runtime_error("Cannot record a null snapshot");
        }

        TimelineNode* newNode =
            new TimelineNode{snapshot, nullptr, tail};

        if (head == nullptr)
        {
            head = newNode;
        }
        else
        {
            tail->next = newNode;
        }

        tail = newNode;
        ++stepCount;
    }

    TimelineNode* begin()
    {
        return head;
    }

    int32_t getStepCount()
    {
        return stepCount;
    }
};
void writeInt32(FILE* file, int32_t value);
void writeInt64(FILE* file, int64_t value);
void writeHeader(FILE* file, const TTDBHeader& header)
{
    if (fwrite(header.magic, 1, 4, file) != 4)
    {
        throw runtime_error("Cannot write file signature");
    }

    writeInt32(file, header.version);
    writeInt32(file, header.stepCount);
    writeInt64(file, header.indexOffset);
}

bool readSourceLine(ifstream &in, string &out)
{
    while (getline(in, out))
    {
        if (out.find_first_not_of(" \t\r\n") != string::npos)
        {
            return true;
        }
    }

    return false;
}
string firstWord(const string &line)
{
    istringstream input(line);
    string word;
    input >> word;
    return word;
}
string secondWord(const string &line)
{
    istringstream input(line);
    string first;
    string second;

    input >> first >> second;
    return second;
}
bool validateProgram(const char *sourcePath)
{
    ifstream input(sourcePath);

    if (!input)
    {
        cerr << "Validation error: cannot open "
             << sourcePath << '\n';
        return false;
    }

    bool insideFunction = false;
    string activeFunction;
    string line;

    while (readSourceLine(input, line))
    {
        string keyword = firstWord(line);

        if (keyword.rfind("//", 0) == 0)
        {
            continue;
        }

        if (keyword == "func")
        {
            if (insideFunction)
            {
                cerr << "Validation error: nested function inside "
                     << activeFunction << '\n';
                return false;
            }

            string functionName = secondWord(line);

            if (functionName.empty())
            {
                cerr << "Validation error: missing function name\n";
                return false;
            }

            insideFunction = true;
            activeFunction = functionName;
        }
        else if (keyword == "func_end")
        {
            if (!insideFunction)
            {
                cerr << "Validation error: unmatched func_end\n";
                return false;
            }

            insideFunction = false;
            activeFunction.clear();
        }
        else if (!insideFunction)
        {
            cerr << "Validation error: instruction outside a function\n";
            return false;
        }
    }

    if (input.bad())
    {
        cerr << "Validation error: failed while reading source\n";
        return false;
    }

    if (insideFunction)
    {
        cerr << "Validation error: missing func_end for "
             << activeFunction << '\n';
        return false;
    }

    return true;
}

int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{

    if (f == nullptr)
    {
        throw runtime_error("Cannot use an unopened file");
    }

    long recordPosition = ftell(f);

    if (recordPosition == -1L)
    {
        throw runtime_error("Cannot get file position");
    }

    int32_t textSize = static_cast<int32_t>(text.size());

    if (fwrite(&offsetField, sizeof(int64_t), 1, f) != 1)
    {
        throw runtime_error("Cannot write offset");
    }

    if (fwrite(&textSize, sizeof(int32_t), 1, f) != 1)
    {
        throw runtime_error("Cannot write text size");
    }

    if (fwrite(text.c_str(), 1, textSize, f) !=
        static_cast<size_t>(textSize))
    {
        throw runtime_error("Cannot write text");
    }

    return recordPosition;
}
int64_t readResolveRecord(FILE *f, string &outText)
{

    if (f == nullptr)
    {
        throw runtime_error("Cannot use an unopened file");
    }

    int64_t offsetField;
    int32_t textSize;

    if (fread(&offsetField, sizeof(int64_t), 1, f) != 1)
    {
        throw runtime_error("Cannot read offset");
    }

    if (fread(&textSize, sizeof(int32_t), 1, f) != 1)
    {
        throw runtime_error("Cannot read text size");
    }

    if (textSize < 0 || static_cast<uint64_t>(textSize) > MAX_SOURCE_BYTES)
    {
        throw runtime_error("Invalid text size");
    }

    outText.resize(textSize);

    if (textSize > 0)
    {
        if (fread(&outText[0], 1, textSize, f) !=
            static_cast<size_t>(textSize))
        {
            throw runtime_error("Cannot read complete text");
        }
    }

    return offsetField;
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream source(sourcePath);

    if (!source)
    {
        throw runtime_error("Cannot open source file");
    }

    FILE* output = fopen(resolveBinPath, "w+b");

    if (output == nullptr)
    {
        throw runtime_error("Cannot open resolve file");
    }

    int64_t mainOffset = -1;
    string line;

    try
    {
        while (getline(source, line))
        {
            string keyword = firstWord(line);

            int64_t recordPosition =
                writeResolveRecord(output, -1, line);

            if (keyword == "func")
            {
                string functionName = secondWord(line);

                if (functionName.empty())
                {
                    throw runtime_error("Missing function name");
                }

                if (funcCount >= MAX_FUNCS)
                {
                    throw runtime_error("Too many functions");
                }

                for (int32_t i = 0; i < funcCount; ++i)
                {
                    if (funcArray[i].funcName == functionName)
                    {
                        throw runtime_error(
                            "Duplicate function: " + functionName);
                    }
                }

                funcArray[funcCount].funcName = functionName;

                funcArray[funcCount].byteOffsetInResolveBin =
                    recordPosition;

                ++funcCount;

                if (functionName == "main")
                {
                    mainOffset = recordPosition;
                }
            } else if (keyword == "call")
            {
                string targetName = secondWord(line);

                if (targetName.empty())
                {
                    throw runtime_error("Missing call target");
                }

                if (patchCount >= MAX_PATCHES)
                {
                    throw runtime_error("Too many calls to resolve");
                }

                patches[patchCount].byteOffsetOfOffsetField =
                    recordPosition;

                patches[patchCount].targetFuncName = targetName;

                ++patchCount;
            }
        }

        if (source.bad())
        {
            throw runtime_error("Cannot finish reading source");
        }

        if (mainOffset == -1)
        {
            throw runtime_error("No main function found");
        }
        for (int32_t i = 0; i < patchCount; ++i)
        {
            int64_t targetOffset = -1;

            for (int32_t j = 0; j < funcCount; ++j)
            {
                if (funcArray[j].funcName ==
                    patches[i].targetFuncName)
                {
                    targetOffset =
                        funcArray[j].byteOffsetInResolveBin;

                    break;
                }
            }

            if (targetOffset == -1)
            {
                throw runtime_error(
                    "Undefined function: " +
                    patches[i].targetFuncName);
            }

            long patchPosition = static_cast<long>(
                patches[i].byteOffsetOfOffsetField);

            if (fseek(output, patchPosition, SEEK_SET) != 0)
            {
                throw runtime_error("Cannot seek to call record");
            }

            if (fwrite(&targetOffset, sizeof(int64_t), 1, output) != 1)
            {
                throw runtime_error("Cannot patch call destination");
            }
        }
    }
    catch (...)
    {
        fclose(output);
        throw;
    }

    if (fclose(output) != 0)
    {
        throw runtime_error("Cannot finish writing resolve file");
    }

    return mainOffset;
}

enum TtdbTokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TtdbTokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{

    istringstream input(line);
    string word;
    int32_t count = 0;

    while (input >> word)
    {

        if (count == 0 && word.rfind("//", 0) == 0)
        {
            return 0;
        }

        if (count >= maxTokens)
        {
            throw runtime_error("Too many tokens in instruction");
        }

        tokens[count].text = word;

        if (count == 0)
        {
            tokens[count].type = KEYWORD;
        }
        else if (count == 1)
        {
            tokens[count].type = IDENTIFIER;
        }
        else
        {
            tokens[count].type = PARAM;
        }

        ++count;
    }

    return count;
}
Variable* findVariable(Frame& frame, const string& name)
{
    for (int32_t i = 0; i < frame.argc; ++i)
    {
        if (frame.argv[i].name == name)
        {
            if (frame.callerBindings[i] != nullptr)
            {
                return frame.callerBindings[i];
            }

            return &frame.argv[i];
        }
    }

    for (int32_t i = 0; i < frame.localCount; ++i)
    {
        if (frame.locals[i].name == name)
        {
            return &frame.locals[i];
        }
    }

    return nullptr;
}
int32_t getValue(Frame& frame, const string& operand)
{
    Variable* variable = findVariable(frame, operand);

    if (variable != nullptr)
    {
        return variable->value;
    }

    istringstream input(operand);
    int32_t number;
    char extra;

    if (!(input >> number))
    {
        throw runtime_error(
            "Expected an integer or existing variable: " + operand);
    }

    if (input >> extra)
    {
        throw runtime_error("Invalid integer: " + operand);
    }

    return number;
}
void executeSet(Frame& frame, const Token tokens[], int32_t tokenCount)
{
    if (tokenCount != 3 || tokens[0].text != "set")
    {
        throw runtime_error("Expected: set variable value");
    }

    string variableName = tokens[1].text;

    int32_t value = getValue(frame, tokens[2].text);

    Variable* variable = findVariable(frame, variableName);

    if (variable != nullptr)
    {
        variable->value = value;
        return;
    }

    if (frame.localCount >= MAX_VARS_PER_FRAME)
    {
        throw runtime_error("Too many local variables");
    }

    frame.locals[frame.localCount].name = variableName;
    frame.locals[frame.localCount].value = value;

    ++frame.localCount;
}
void executeArithmetic(Frame& frame, const Token tokens[], int32_t tokenCount)
{
    if (tokenCount != 3)
    {
        throw runtime_error("Expected: arithmetic_instruction variable value");
    }

    string operation = tokens[0].text;

    if (operation != "add" &&
        operation != "sub" &&
        operation != "mul" &&
        operation != "div")
    {
        throw runtime_error(
            "Unknown arithmetic instruction: " + operation);
    }

    Variable* destination =
        findVariable(frame, tokens[1].text);

    if (destination == nullptr)
    {
        throw runtime_error(
            "Unknown destination variable: " + tokens[1].text);
    }

    int32_t left = destination->value;
    int32_t right = getValue(frame, tokens[2].text);
    int32_t result = 0;

    if (operation == "add")
    {
        result = left + right;
    }
    else if (operation == "sub")
    {
        result = left - right;
    }
    else if (operation == "mul")
    {
        result = left * right;
    }
    else
    {
        if (right == 0)
        {
            throw runtime_error("Division by zero");
        }

        result = left / right;
    }

    destination->value = result;
}
Frame createCallFrame(Frame& caller, const Token callTokens[], int32_t callCount, const Token functionTokens[], int32_t functionCount, int32_t returnPosition)
{
    if (callCount < 2 || callTokens[0].text != "call")
    {
        throw runtime_error("Invalid call instruction");
    }

    if (functionCount < 2 ||
        functionTokens[0].text != "func")
    {
        throw runtime_error("Call target is not a function");
    }

    if (callTokens[1].text != functionTokens[1].text)
    {
        throw runtime_error("Call target name does not match");
    }

    int32_t argumentCount = callCount - 2;
    int32_t parameterCount = functionCount - 2;

    if (argumentCount != parameterCount)
    {
        throw runtime_error("Argument count does not match");
    }

    if (parameterCount > MAX_VARS_PER_FRAME)
    {
        throw runtime_error("Too many function parameters");
    }

    Frame callee{};
    callee.func_name = functionTokens[1].text;
    callee.argc = parameterCount;
    callee.returnLine = returnPosition;

    for (int32_t i = 0; i < parameterCount; ++i)
    {
        string parameterName = functionTokens[i + 2].text;
        string argumentName = callTokens[i + 2].text;

        for (int32_t j = 0; j < i; ++j)
        {
            if (callee.argv[j].name == parameterName)
            {
                throw runtime_error("Duplicate parameter name");
            }
        }

        Variable* argument = findVariable(caller, argumentName);

        if (argument == nullptr)
        {
            throw runtime_error(
                "Call argument must be an existing variable: " +
                argumentName);
        }

        callee.argv[i].name = parameterName;
        callee.argv[i].value = argument->value;
        callee.callerBindings[i] = argument;
    }

    return callee;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* snapshot = new Snapshot{};

    snapshot->stackDepth = callStack.snapshot_into(
        snapshot->callStack,
        MAX_STACK_DEPTH);

    for (int32_t i = 0; i < snapshot->stackDepth; ++i)
    {
        Frame& frame = snapshot->callStack[i];

        for (int32_t j = 0; j < frame.argc; ++j)
        {
            if (frame.callerBindings[j] != nullptr)
            {
                frame.argv[j].value =
                    frame.callerBindings[j]->value;
            }

            frame.callerBindings[j] = nullptr;
        }
    }

    return snapshot;
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{

    if (mainOffset < 0)
    {
        throw runtime_error("Invalid main offset");
    }

    FILE* file = fopen(resolveBinPath, "rb");

    if (file == nullptr)
    {
        throw runtime_error("Cannot open resolve file");
    }

    try
    {
        if (fseek(file, static_cast<long>(mainOffset),
                  SEEK_SET) != 0)
        {
            throw runtime_error("Cannot seek to main");
        }

        string line;
        Token tokens[MAX_TOKENS];

        readResolveRecord(file, line);

        int32_t tokenCount =
            tokenizeLine(line, tokens, MAX_TOKENS);

        if (tokenCount != 2 ||
            tokens[0].text != "func" ||
            tokens[1].text != "main")
        {
            throw runtime_error(
                "Expected a main function without parameters");
        }

        Stack<Frame> callStack;

        Frame mainFrame{};
        mainFrame.func_name = "main";
        mainFrame.returnLine = -1;

        callStack.push(mainFrame);

        timeline.record(buildSnapshot(callStack));

        while (!callStack.isEmpty())
        {

            int64_t targetOffset =
                readResolveRecord(file, line);

            tokenCount =
                tokenizeLine(line, tokens, MAX_TOKENS);

            if (tokenCount == 0)
            {
                continue;
            }

            string instruction = tokens[0].text;
            Frame& activeFrame = callStack.peek();

            if (instruction == "set")
            {
                executeSet(
                    activeFrame, tokens, tokenCount);
            }
            else if (instruction == "add" ||
                     instruction == "sub" ||
                     instruction == "mul" ||
                     instruction == "div")
            {
                executeArithmetic(
                    activeFrame, tokens, tokenCount);
            }
            else if (instruction == "call")
            {
                if (targetOffset < 0)
                {
                    throw runtime_error("Unresolved function call");
                }

                long returnPosition = ftell(file);

                if (returnPosition == -1L)
                {
                    throw runtime_error(
                        "Cannot get return position");
                }

                if (fseek(file, static_cast<long>(targetOffset),
                          SEEK_SET) != 0)
                {
                    throw runtime_error(
                        "Cannot seek to called function");
                }

                string functionLine;
                readResolveRecord(file, functionLine);

                Token functionTokens[MAX_TOKENS];

                int32_t functionCount = tokenizeLine(
                    functionLine, functionTokens, MAX_TOKENS);

                Frame callee = createCallFrame(
                    activeFrame,
                    tokens,
                    tokenCount,
                    functionTokens,
                    functionCount,
                    static_cast<int32_t>(returnPosition));

                callStack.push(callee);

                cout << "Calling "
                     << callee.func_name << '\n';

            }
            else if (instruction == "func_end")
            {
                if (tokenCount != 1)
                {
                    throw runtime_error(
                        "func_end takes no arguments");
                }

                if (callStack.depth() == 1)
                {
                    cout << "Final variables in main:\n";

                    for (int32_t i = 0;
                         i < activeFrame.localCount;
                         ++i)
                    {
                        cout << activeFrame.locals[i].name
                             << " = "
                             << activeFrame.locals[i].value
                             << '\n';
                    }
                }

                Frame finishedFrame = callStack.pop();

                if (!callStack.isEmpty())
                {
                    if (finishedFrame.returnLine < 0)
                    {
                        throw runtime_error(
                            "Invalid return position");
                    }

                    if (fseek(file, finishedFrame.returnLine,
                              SEEK_SET) != 0)
                    {
                        throw runtime_error(
                            "Cannot return to caller");
                    }

                    cout << "Returning to "
                         << callStack.peek().func_name << '\n';
                }
            }
            else
            {
                throw runtime_error(
                    "Unexpected instruction: " + instruction);
            }
            timeline.record(buildSnapshot(callStack));
        }
    }
    catch (...)
    {
        fclose(file);
        throw;
    }

    if (fclose(file) != 0)
    {
        throw runtime_error("Cannot close resolve file");
    }
}

void writeInt32(FILE* file, int32_t value)
{
    if (fwrite(&value, sizeof(int32_t), 1, file) != 1)
    {
        throw runtime_error("Cannot write 32-bit integer");
    }
}

void writeInt64(FILE* file, int64_t value)
{
    if (fwrite(&value, sizeof(int64_t), 1, file) != 1)
    {
        throw runtime_error("Cannot write 64-bit integer");
    }
}

void writeString(FILE* file, const string& text)
{

    if (text.size() > MAX_SOURCE_BYTES)
    {
        throw runtime_error("String is too long");
    }

    int32_t length = static_cast<int32_t>(text.size());

    writeInt32(file, length);

    if (fwrite(text.c_str(), 1, text.size(), file) != text.size())
    {
        throw runtime_error("Cannot write string");
    }
}

void writeSnapshot(FILE* file, const Snapshot& snapshot)
{
    if (snapshot.stackDepth < 0 ||
        snapshot.stackDepth > MAX_STACK_DEPTH)
    {
        throw runtime_error("Invalid snapshot depth");
    }

    writeInt32(file, snapshot.stackDepth);

    for (int32_t i = 0; i < snapshot.stackDepth; ++i)
    {
        const Frame& frame = snapshot.callStack[i];

        if (frame.argc < 0 ||
            frame.argc > MAX_VARS_PER_FRAME ||
            frame.localCount < 0 ||
            frame.localCount > MAX_VARS_PER_FRAME)
        {
            throw runtime_error("Invalid frame variable count");
        }

        writeString(file, frame.func_name);

        writeInt32(file, frame.argc);

        for (int32_t j = 0; j < frame.argc; ++j)
        {
            writeString(file, frame.argv[j].name);
            writeInt32(file, frame.argv[j].value);
        }

        writeInt32(file, frame.returnLine);

        writeInt32(file, frame.localCount);

        for (int32_t j = 0; j < frame.localCount; ++j)
        {
            writeString(file, frame.locals[j].name);
            writeInt32(file, frame.locals[j].value);
        }
    }
}

void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    int32_t stepCount = timeline.getStepCount();

    if (stepCount < 0)
    {
        throw runtime_error("Invalid timeline step count");
    }

    FILE* file = fopen(tdbgPath, "wb");

    if (file == nullptr)
    {
        throw runtime_error("Cannot open debugger output file");
    }

    int64_t* index = nullptr;

    try
    {
        TTDBHeader header{};
        header.magic[0] = 'T';
        header.magic[1] = 'T';
        header.magic[2] = 'D';
        header.magic[3] = 'B';

        header.version = 1;
        header.stepCount = stepCount;

        header.indexOffset = 0;

        writeHeader(file, header);

        if (stepCount > 0)
        {
            index = new int64_t[stepCount];
        }

        TimelineNode* current = timeline.begin();
        int32_t step = 0;

        while (current != nullptr)
        {
            if (step >= stepCount)
            {
                throw runtime_error("Timeline count mismatch");
            }

            if (current->data == nullptr)
            {
                throw runtime_error("Timeline contains null snapshot");
            }

            long position = ftell(file);

            if (position == -1L)
            {
                throw runtime_error("Cannot get snapshot position");
            }

            index[step] = position;

            writeSnapshot(file, *current->data);

            current = current->next;
            ++step;
        }

        if (step != stepCount)
        {
            throw runtime_error("Timeline count mismatch");
        }

        long indexPosition = ftell(file);

        if (indexPosition == -1L)
        {
            throw runtime_error("Cannot get index position");
        }

        header.indexOffset = indexPosition;

        for (int32_t i = 0; i < stepCount; ++i)
        {
            writeInt64(file, index[i]);
        }

        if (fseek(file, 0, SEEK_SET) != 0)
        {
            throw runtime_error("Cannot return to header");
        }

        writeHeader(file, header);
    }
    catch (...)
    {
        delete[] index;
        fclose(file);
        throw;
    }

    delete[] index;

    if (fclose(file) != 0)
    {
        throw runtime_error("Cannot finish debugger output file");
    }
}

void runDebugPipeline(const char* sourcePath,
                      const char* resolvePath,
                      const char* tdbgPath)
{
    if (!validateProgram(sourcePath))
        throw runtime_error("Invalid source program");
    int64_t mainOffset = resolveProgram(sourcePath, resolvePath);
    if (mainOffset < 0)
        throw runtime_error("Cannot resolve main");
    Timeline timeline;
    executeProgram(resolvePath, mainOffset, timeline);
    writeTdbg(timeline, tdbgPath);
}

void handleUpload(SOCKET client)
{
    FILE* upload = nullptr;
    FILE* response = nullptr;
    try
    {
        upload = fopen("network_source.bin", "wb");
        if (upload == nullptr)
            throw runtime_error("Cannot create source file");
        unsigned char status = receiveFile(client, upload, MAX_SOURCE_BYTES);
        int result = fclose(upload);
        upload = nullptr;
        if (result != 0)
            throw runtime_error("Cannot finish saving source");
        if (status != 0)
            throw runtime_error("Expected a source upload");
        runDebugPipeline("network_source.bin", "network_resolve.bin",
                         "network_session.tdbg");
        response = fopen("network_session.tdbg", "rb");
        if (response == nullptr)
            throw runtime_error("Cannot open debugger file");
    }
    catch (const exception& error)
    {
        if (upload != nullptr)
            fclose(upload);
        remove("network_source.bin");
        sendError(client, error.what());
        return;
    }
    try
    {
        sendFile(client, response, 0);
    }
    catch (...)
    {
        fclose(response);
        throw;
    }
    fclose(response);
}

int main(int argc, char* argv[])
{
    bool socketsStarted = false;
    SOCKET listener = INVALID_SOCKET;
    try
    {
        if (argc == 1)
        {
            runDebugPipeline("source.bin", "resolve.bin", "session.tdbg");
            return 0;
        }
        if (argc != 2)
            throw runtime_error("Usage: server.exe [port]");
        string portText = argv[1];
        size_t parsed = 0;
        int port = stoi(portText, &parsed);
        if (parsed != portText.size() || port < 1 || port > 65535)
            throw runtime_error("Invalid server port");
        startWinsock();
        socketsStarted = true;
        listener = createListenSocket(port);
        cout << "TTDB server listening on port " << port << endl;
        while (true)
        {
            SOCKET client = accept(listener, nullptr, nullptr);
            if (client == INVALID_SOCKET)
                throw runtime_error("Cannot accept a connection");
            try
            {
                setSocketTimeout(client, 30000);
                handleUpload(client);
            }
            catch (const exception& error)
            {
                cerr << error.what() << '\n';
            }
            closesocket(client);
        }
    }
    catch (const exception& error)
    {
        if (listener != INVALID_SOCKET)
            closesocket(listener);
        if (socketsStarted)
            WSACleanup();
        cerr << error.what() << '\n';
        return 1;
    }
}
