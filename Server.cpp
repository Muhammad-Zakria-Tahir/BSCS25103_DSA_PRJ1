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
#include <cstdio>
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
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    Stack() {
        top = nullptr;
        count = 0;
    }

    ~Stack()
    {
        while (!isEmpty())
            pop();
    }

    void push(const T& val)
    {
        if (count >= MAX_STACK_DEPTH) return;

        Node* node = new Node();
        node->data = val;
        node->next = top;
        top = node;
        count++;
    }

    T pop()
    {
        if (isEmpty()) throw underflow_error("Stack is empty.");

        Node* temp = top;
        T val = temp->data;
        top = top->next;
        delete temp;
        count--;
        return val;
    }

    T& peek()
    {
        if (isEmpty()) throw underflow_error("Stack is empty.");
        return top->data;
    }

    bool isEmpty()
    {
        return top == nullptr;
    }

    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int32_t totalSnaps;

        if (count > maxLen) {
            totalSnaps = maxLen;
        }
        else {
            totalSnaps = count;
        }

        Node* current = top;
        int32_t index = totalSnaps - 1;

        while (current != nullptr && index >= 0)
        {
            out[index] = current->data;
            current = current->next;
            index--;
        }

        return totalSnaps;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
}
struct TimelineNode
{
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
Timeline()
{
    head = tail = nullptr;
    stepCount = 0;
}
~Timeline()
{
    TimelineNode* current = head;
    while (current != nullptr)
    {
        TimelineNode* next = current->next;
        delete current->data;
        delete current;
        current = next;
    }
}
void record(Snapshot* s)
{
    TimelineNode* node = new TimelineNode();
    node->data = s;
    node->next = nullptr;
    node->prev = tail;
    if (head == nullptr) 
        head = node;
    if (tail != nullptr) 
        tail->next = node;
    tail = node;
    stepCount++;
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
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepcount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
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
bool readSourceLine(ifstream& in, string& out)
{
    string temp;

    while (getline(in, temp)) {
        int start = 0;
        int end = temp.length() - 1;

        while (start <= end && (temp[start] == ' ' || temp[start] == '\t' || temp[start] == '\n')) {
            start++;
        }

        while (end >= start && (temp[end] == ' ' || temp[end] == '\t' || temp[end] == '\n')) {
            end--;
        }

        if (start <= end) {
            out = "";
            for (int i = start; i <= end; ++i) {
                out += temp[i];
            }
            return true;
        }
    }

    return false;
}
string firstWord(const string& line)
{
    istringstream input(line);
    string word;
    input >> word;
    return word;
}

string secondWord(const string& line)
{
    istringstream input(line);
    string temp;
    string word;
    input >> temp >> word;
    return word;
}

bool validateProgram(const char* sourcePath)
{
    ifstream file(sourcePath);
    if (!file.is_open()) {
        return false;
    }

    string line;
    bool inFunction = false;

    while (readSourceLine(file, line)) {
        string first = firstWord(line);

        if (first == "func") {

            if (inFunction) {
                file.close();
                return false;
            }
            inFunction = true;
        }
        else if (first == "func_end") {

            if (!inFunction) {
                file.close();
                return false;
            }
            inFunction = false;
        }
    }

    file.close();

    return !inFunction;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    int64_t start = ftell(f);
    int32_t len = text.length();

    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&len, sizeof(int32_t), 1, f);
    fwrite((text.size() == 0 ? nullptr : &text[0]), 1, len, f);

    return start;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offsetfield = 0;
    int32_t strlen = 0;

    if (fread(&offsetfield, sizeof(int64_t), 1, f) != 1) return -1;
    if (fread(&strlen, sizeof(int32_t), 1, f) != 1) return -1;

    string buffer;
    buffer.resize(strlen);
    if (strlen > 0) fread(&buffer[0], 1, strlen, f);
    outText = buffer;

    return offsetfield;
}

int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream in(sourcePath);
    FILE* fout = fopen(resolveBinPath, "wb+");
    if (!in.is_open() || !fout) return -1;

    string line;
    int64_t mainofs = -1;

    while (readSourceLine(in, line))
    {
        string first = firstWord(line);
        int64_t Start = ftell(fout);

        if (first == "func")
        {
            string second = secondWord(line);
            funcArray[funcCount++] = { second, Start };
            if (second == "main")  mainofs = Start;

            writeResolveRecord(fout, 0, line);
        }
        else if (first == "call")
        {
            string FuncName = secondWord(line);
            patches[patchCount++] = { Start, FuncName };
            writeResolveRecord(fout, 0, line);
        }
        else
        {
            writeResolveRecord(fout, 0, line);
        }
    }

    if (mainofs == -1)
    {
        fclose(fout);
        return -1;
    }

    for (int i = 0; i < patchCount; i++)
    {
        int64_t targetofs = -1;
        for (int j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                targetofs = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }

        if (targetofs == -1)
        {
            fclose(fout);
            return -1;
        }

        fseek(fout, patches[i].byteOffsetOfOffsetField, SEEK_SET);
        fwrite(&targetofs, sizeof(int64_t), 1, fout);
    }

    in.close();
    fclose(fout);
    return mainofs;
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

bool iskeyword(string word) {
    return (word == "func" || word == "func_end" || word == "call" || word == "set");
}
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    istringstream input(line);
    string word;
    int32_t count = 0;

    if (input >> word && count < maxTokens)
    {
        if (!iskeyword(word)) {
            throw runtime_error("Keyword not Supported\n");
        }
        tokens[count++] = { KEYWORD, word };
    }
    if (input >> word && count < maxTokens)
    {
        tokens[count++] = { IDENTIFIER, word };
    }
    while (input >> word && count < maxTokens)
    {
        tokens[count++] = { PARAM, word };
    }
    return count;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* snap = new Snapshot();
    snap->stackDepth = callStack.depth();
    callStack.snapshot_into(snap->callStack, MAX_STACK_DEPTH);
    return snap;
}

int32_t getValue(Frame& frame, const string& Name)
{
    for (int i = 0; i < frame.argc; i++)
        if (frame.argv[i].name == Name)
            return frame.argv[i].value;

    for (int i = 0; i < frame.localCount; i++)
        if (frame.locals[i].name == Name)
            return frame.locals[i].value;

    return 0;
}

void setValue(Frame& frame, const string& Name, int32_t val)
{
    for (int i = 0; i < frame.argc; i++)
        if (frame.argv[i].name == Name)
        {
            frame.argv[i].value = val;
            return;
        }

    for (int i = 0; i < frame.localCount; i++)
        if (frame.locals[i].name == Name)
        {
            frame.locals[i].value = val;
            return;
        }

    if (frame.localCount < MAX_VARS_PER_FRAME)
        frame.locals[frame.localCount++] = { Name, val };

}
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
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
