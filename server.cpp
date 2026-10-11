// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
//#include <unistd.h>
//#include <sys/socket.h> linux only, not needed for Windows
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <sstream>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever


// ---- Custom data structures

// Stack: back the live Call Stack during execution
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
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
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


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
    }
    void record(Snapshot *s)
    {
        // add record in the timeline
    }
    TimelineNode *begin()
    {
    }
    int32_t getStepCount()
    {
    }
};

// Core structs
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
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



// PASS 0x0: READING source.bin + VALIDITY CHECK
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

        // Support full-line comments used in the brief's examples.
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

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    if (f == nullptr)
    {
        throw runtime_error("Cannot use an unopened file");
    }

    long recordPosition = ftell(f);

    if (recordPosition == -1L)
    {
        throw runtime_error("Cannot get file position");
    }

    // Assumes the input has passed the project's source-size limit.
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
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
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
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
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

            // Find the destination function.
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

            // Move to the call record's offset field.
            long patchPosition = static_cast<long>(
                patches[i].byteOffsetOfOffsetField);

            if (fseek(output, patchPosition, SEEK_SET) != 0)
            {
                throw runtime_error("Cannot seek to call record");
            }

            // Replace only the eight-byte destination field.
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

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    istringstream input(line);
    string word;
    int32_t count = 0;

    while (input >> word)
    {
        // Ignore a line comment.
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
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
       // Snapshot recording is not implemented yet.
           // Timeline recording will be added next.
    (void)timeline;

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

        while (!callStack.isEmpty())
        {
            // For call records, this is the patched destination.
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

                // Reading the call has advanced to the next record.
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

                // Keep call tokens separate from declaration tokens.
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

                // File position is now after the func declaration.
            }
            else if (instruction == "func_end")
            {
                if (tokenCount != 1)
                {
                    throw runtime_error(
                        "func_end takes no arguments");
                }

                // Display main's final state before removing it.
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

                // activeFrame must not be used after the pop.
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

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section


int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}

