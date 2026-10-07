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
#include <unistd.h>
#include <sys/socket.h>
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

    if (textSize < 0 || textSize > MAX_SOURCE_BYTES)
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

    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;

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