#include <iostream>
#include <filesystem>
#include <algorithm>
#include <functional>
#include <vector>
#include <string>
#include <fstream>
#include <mutex>
#include <thread>
#include <map>
#include <set>
#include <queue>
#include <chrono>
#include <clocale>
#include <cctype>
#include <cstdio>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ncurses.h>
#include "nlohmann/json.hpp"

using namespace std;
using json = nlohmann::json;
namespace fs = std::filesystem;

bool MainRunning = true;

struct QueueNode {
    string sender="";
    string action="";
    vector<string> parameters = {};
};

struct QueueOtherNode {
    string from = "";
    string sender="";
    string action="";
    vector<string> parameters = {};
};

queue<QueueNode> MainQueue;
queue<QueueNode> MainThreadQueue;
queue<QueueNode> FileManagerQueue; 
queue<QueueNode> TextEditorQueue;  
queue<QueueOtherNode> OtherQueue;
mutex QueueMutex; 

fs::path MotherPath;
fs::path StartCwd; 
int max_x,max_y;
WINDOW* FileManagerWindow;
WINDOW* TextEditorWindow;
WINDOW* ShellWindow;     
WINDOW* LeftBar;
WINDOW* StatusBarWindow; 

void WriteTUI();

// ============================================================
//  UTF-8 helpers
// ============================================================
vector<string> Utf8Split(const string& s) {
    vector<string> result;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        int len = 1;
        if ((c & 0x80) == 0x00) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) len = 1; 
        result.push_back(s.substr(i, len));
        i += len;
    }
    return result;
}

string ReadUtf8Char(int firstByte) {
    unsigned char c = (unsigned char)firstByte;
    int len = 1;
    if ((c & 0x80) == 0x00) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    string s(1, (char)c);
    for (int i = 1; i < len; i++) {
        int b = getch();
        if (b == ERR) break;
        s += (char)b;
    }
    return s;
}

// ============================================================
//  Editor / UI state & Undo System
// ============================================================
enum class EditorMode { NORMAL, INSERT, COMMAND };
enum class FocusPanel { EDITOR, FILEMANAGER, TERMINAL };

struct EditorBuffer {
    vector<string> lines = {""};
    fs::path filepath;
    bool hasFile = false;
    bool modified = false;
    int cursorLine = 0;
    int cursorCol = 0;   
    int topLine = 0;     
    vector<string> clipboard;
};

// --- Undo System ---
struct EditorSnapshot {
    vector<string> lines;
    int cursorLine;
    int cursorCol;
};
vector<EditorSnapshot> UndoHistory;

EditorBuffer CurBuf;
EditorMode CurMode = EditorMode::NORMAL;
FocusPanel CurFocus = FocusPanel::EDITOR;
char PendingKey = 0; 
string CmdLineInput = "";
bool ShowLineNumbers = false; 
bool ShowCursorLine = false;  

vector<string> CmdHistory;   
int CmdHistoryPos = -1;

struct FileEntry { string name; bool isDir; };
fs::path CurrentDir;
vector<FileEntry> FMEntries;
int FMSelected = 0;
int FMTop = 0;
fs::file_time_type LastFMWriteTime; // ไว้เช็คไฟล์อัปเดต

struct TerminalState {
    vector<string> outputLines = {
        "ThaiVim Terminal - Type a shell command and press Enter",
        "(Run commands from the directory currently opened in File Manager)"
    };
    string inputLine = "";
    vector<string> history;
    int historyPos = -1;
    int scrollOffset = 0;
    bool running = false;
};
TerminalState Term;
mutex TermMutex; 

// ============================================================
//  Save Undo State
// ============================================================
void SaveUndo() {
    if (UndoHistory.size() > 50) UndoHistory.erase(UndoHistory.begin());
    UndoHistory.push_back({CurBuf.lines, CurBuf.cursorLine, CurBuf.cursorCol});
}

// ============================================================
//  Syntax highlighting
// ============================================================
enum HRole { HR_NONE = 0, HR_KEYWORD, HR_TYPE, HR_STRING, HR_COMMENT, HR_NUMBER, HR_PREPROC, HR_COUNT };

struct HighlightSpan { int start; int length; int role; }; 

struct SyntaxDef {
    string name;
    string lineComment;
    string blockStart, blockEnd;
    vector<string> stringDelims; 
    bool preprocessor = false;   
    bool numbers = true;
    set<string> keywords;
    set<string> types;
};

map<string, SyntaxDef> SyntaxDefs;   
map<string, string> ExtToSyntax;     
string CurrentSyntaxName = "";       

struct LineHLCache {
    size_t hash = 0;
    bool valid = false;
    bool startState = false; 
    bool endState = false;   
    vector<HighlightSpan> spans;
};
vector<LineHLCache> HLCache; 

struct Theme { string name = "dark"; map<string, string> colors; };
Theme CurTheme;

map<string, int> ColorNameToConst = {
    {"black", COLOR_BLACK}, {"red", COLOR_RED}, {"green", COLOR_GREEN}, {"yellow", COLOR_YELLOW},
    {"blue", COLOR_BLUE}, {"magenta", COLOR_MAGENTA}, {"cyan", COLOR_CYAN}, {"white", COLOR_WHITE}
};

std::string getOSName() {
    #if defined(_WIN32) || defined(_WIN64)
        return "Windows";
    #elif defined(__APPLE__) || defined(__MACH__)
        #include <TargetConditionals.h>
        #if TARGET_OS_IPHONE
            return "iOS";
        #elif TARGET_OS_MAC
            return "macOS";
        #else
            return "Apple Device";
        #endif
    #elif defined(__linux__)
        return "Linux";
    #elif defined(__FreeBSD__)
        return "FreeBSD";
    #elif defined(__unix__) || defined(__unix)
        return "Unix";
    #else
        return "Unknown OS";
    #endif
}

int ResolveColorName(const string& name, int fallback) {
    auto it = ColorNameToConst.find(name);
    return it != ColorNameToConst.end() ? it->second : fallback;
}

void LoadSyntaxDefs() {
    SyntaxDefs.clear();
    ExtToSyntax.clear();
    fs::path syntaxDir = MotherPath / "syntax";
    fs::path cfgPath = syntaxDir / "config.json";
    if (!fs::exists(cfgPath)) return;
    try {
        ifstream cf(cfgPath);
        json cfg; cf >> cfg;
        if (cfg.contains("extensions")) {
            for (auto it = cfg["extensions"].begin(); it != cfg["extensions"].end(); ++it) {
                string ext = it.key();
                for (auto& c : ext) c = (char)tolower((unsigned char)c);
                ExtToSyntax[ext] = it.value().get<string>();
            }
        }
    } catch (...) {
        return; 
    }

    set<string> names;
    for (auto& kv : ExtToSyntax) names.insert(kv.second);

    for (auto& name : names) {
        fs::path p = syntaxDir / (name + ".json");
        if (!fs::exists(p)) continue;
        try {
            ifstream f(p);
            json j; f >> j;
            SyntaxDef def;
            def.name = j.value("name", name);
            def.lineComment = j.value("line_comment", "");
            def.blockStart = j.value("block_comment_start", "");
            def.blockEnd = j.value("block_comment_end", "");
            def.preprocessor = j.value("preprocessor", false);
            def.numbers = j.value("numbers", true);
            if (j.contains("string_delimiters"))
                for (auto& s : j["string_delimiters"]) def.stringDelims.push_back(s.get<string>());
            if (j.contains("keywords"))
                for (auto& s : j["keywords"]) def.keywords.insert(s.get<string>());
            if (j.contains("types"))
                for (auto& s : j["types"]) def.types.insert(s.get<string>());
            SyntaxDefs[name] = def;
        } catch (...) {
        }
    }
}

string DetermineSyntaxForFile(const fs::path& path) {
    string ext = path.extension().string();
    for (auto& c : ext) c = (char)tolower((unsigned char)c);
    auto it = ExtToSyntax.find(ext);
    if (it != ExtToSyntax.end() && SyntaxDefs.count(it->second)) return it->second;
    return "";
}

bool StartsWithAt(const string& s, size_t pos, const string& pat) {
    if (pat.empty() || pos + pat.size() > s.size()) return false;
    return s.compare(pos, pat.size(), pat) == 0;
}

vector<HighlightSpan> ComputeSpans(const string& line, const SyntaxDef& def,
                                    bool startInBlockComment, bool& outEndInBlockComment) {
    vector<HighlightSpan> spans;
    size_t n = line.size();
    size_t i = 0;
    outEndInBlockComment = false;

    if (startInBlockComment) {
        if (!def.blockEnd.empty()) {
            size_t endPos = line.find(def.blockEnd, 0);
            if (endPos == string::npos) {
                spans.push_back({0, (int)n, HR_COMMENT});
                outEndInBlockComment = true;
                return spans;
            }
            size_t stop = endPos + def.blockEnd.size();
            spans.push_back({0, (int)stop, HR_COMMENT});
            i = stop;
        } else {
            spans.push_back({0, (int)n, HR_COMMENT});
            outEndInBlockComment = true;
            return spans;
        }
    }

    if (def.preprocessor) {
        size_t j = i;
        while (j < n && isspace((unsigned char)line[j])) j++;
        if (j < n && line[j] == '#') {
            spans.push_back({(int)i, (int)(n - i), HR_PREPROC});
            return spans;
        }
    }

    while (i < n) {
        if (!def.lineComment.empty() && StartsWithAt(line, i, def.lineComment)) {
            spans.push_back({(int)i, (int)(n - i), HR_COMMENT});
            break;
        }
        if (!def.blockStart.empty() && StartsWithAt(line, i, def.blockStart)) {
            size_t searchFrom = i + def.blockStart.size();
            size_t endPos = def.blockEnd.empty() ? string::npos : line.find(def.blockEnd, searchFrom);
            if (endPos == string::npos) {
                spans.push_back({(int)i, (int)(n - i), HR_COMMENT});
                outEndInBlockComment = true;
                break;
            }
            size_t stop = endPos + def.blockEnd.size();
            spans.push_back({(int)i, (int)(stop - i), HR_COMMENT});
            i = stop;
            continue;
        }
        char c = line[i];
        bool isStringDelim = false;
        for (auto& d : def.stringDelims) {
            if (d.size() == 1 && c == d[0]) { isStringDelim = true; break; }
        }
        if (isStringDelim) {
            size_t start = i; char q = c; i++;
            while (i < n) {
                if (line[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (line[i] == q) { i++; break; }
                i++;
            }
            spans.push_back({(int)start, (int)(i - start), HR_STRING});
            continue;
        }
        if (def.numbers && isdigit((unsigned char)c)) {
            size_t start = i; i++;
            while (i < n && (isalnum((unsigned char)line[i]) || line[i] == '.')) i++;
            spans.push_back({(int)start, (int)(i - start), HR_NUMBER});
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i; i++;
            while (i < n && (isalnum((unsigned char)line[i]) || line[i] == '_')) i++;
            string word = line.substr(start, i - start);
            if (def.keywords.count(word)) spans.push_back({(int)start, (int)(i - start), HR_KEYWORD});
            else if (def.types.count(word)) spans.push_back({(int)start, (int)(i - start), HR_TYPE});
            continue;
        }
        i++;
    }
    return spans;
}

void EnsureHighlightValid(int uptoLine) {
    if (CurrentSyntaxName.empty() || !SyntaxDefs.count(CurrentSyntaxName)) return;
    if ((int)HLCache.size() != (int)CurBuf.lines.size()) HLCache.resize(CurBuf.lines.size());
    const SyntaxDef& def = SyntaxDefs[CurrentSyntaxName];
    bool state = false;
    int limit = min((int)CurBuf.lines.size() - 1, uptoLine);
    for (int i = 0; i <= limit; i++) {
        auto& c = HLCache[i];
        size_t h = hash<string>{}(CurBuf.lines[i]);
        if (c.valid && c.hash == h && c.startState == state) {
            state = c.endState; 
            continue;
        }
        c.hash = h;
        c.startState = state;
        c.spans = ComputeSpans(CurBuf.lines[i], def, state, c.endState);
        c.valid = true;
        state = c.endState;
    }
}

void DrawHighlightedLine(WINDOW* win, int row, int col, const string& line, int maxChars,
                          const vector<HighlightSpan>& spans, int extraAttr = 0) {
    if (maxChars <= 0) return;
    size_t n = line.size();
    size_t pos = 0, si = 0;
    int used = 0, x = col;
    while (pos < n && used < maxChars) {
        int role = HR_NONE;
        size_t segEnd = n;
        if (si < spans.size() && (size_t)spans[si].start == pos) {
            role = spans[si].role;
            segEnd = pos + spans[si].length;
            si++;
        } else if (si < spans.size() && (size_t)spans[si].start > pos) {
            segEnd = spans[si].start;
        }
        string seg = line.substr(pos, segEnd - pos);
        auto chars = Utf8Split(seg);
        int remain = maxChars - used;
        if ((int)chars.size() > remain) {
            string clipped;
            for (int k = 0; k < remain; k++) clipped += chars[k];
            seg = clipped;
            chars.resize(remain);
        }
        if (!seg.empty()) {
            int attr = (role != HR_NONE) ? (COLOR_PAIR(20 + role) | (role == HR_COMMENT ? A_DIM : 0)) : 0;
            attr |= extraAttr;
            if (attr) wattron(win, attr);
            mvwaddnstr(win, row, x, seg.c_str(), (int)seg.size());
            if (attr) wattroff(win, attr);
        }
        x += (int)chars.size();
        used += (int)chars.size();
        pos = segEnd;
    }
}

void ApplyTheme(const Theme& t) {
    int bg = ResolveColorName(t.colors.count("background") ? t.colors.at("background") : "", COLOR_BLACK);
    int fg = ResolveColorName(t.colors.count("foreground") ? t.colors.at("foreground") : "", COLOR_WHITE);
    auto get = [&](const string& key, int fallback) {
        return ResolveColorName(t.colors.count(key) ? t.colors.at(key) : "", fallback);
    };
    init_pair(30, fg, bg);
    init_pair(20 + HR_KEYWORD,    get("keyword", COLOR_BLUE), bg);
    init_pair(20 + HR_TYPE,       get("type", COLOR_CYAN), bg);
    init_pair(20 + HR_STRING,     get("string", COLOR_GREEN), bg);
    init_pair(20 + HR_COMMENT,    get("comment", fg), bg);
    init_pair(20 + HR_NUMBER,     get("number", COLOR_MAGENTA), bg);
    init_pair(20 + HR_PREPROC,    get("preprocessor", COLOR_YELLOW), bg);
}

void ApplyThemeToWindows() {
    WINDOW* wins[] = { LeftBar, FileManagerWindow, TextEditorWindow, ShellWindow, StatusBarWindow };
    for (auto* w : wins) {
        if (!w) continue;
        wbkgd(w, COLOR_PAIR(30));
    }
}

bool LoadAndApplyTheme(const string& name) {
    fs::path p = MotherPath / "themes" / (name + ".json");
    if (!fs::exists(p)) return false;
    try {
        ifstream f(p);
        json j; f >> j;
        Theme t;
        t.name = j.value("name", name);
        if (j.contains("colors"))
            for (auto it = j["colors"].begin(); it != j["colors"].end(); ++it)
                t.colors[it.key()] = it.value().get<string>();
        CurTheme = t;
        ApplyTheme(CurTheme);
        ApplyThemeToWindows();
        return true;
    } catch (...) {
        return false;
    }
}

// ============================================================
//  Setup / layout
// ============================================================
void SetupTerminal() {
    initscr();
    start_color();
    raw(); // เปลี่ยนเป็น raw() เพื่อจับสัญญาณ Ctrl+C, Ctrl+S, Ctrl+Q ได้โดยตรง
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);
    timeout(50); 

    getmaxyx(stdscr, max_y, max_x);

    LoadSyntaxDefs();
    WriteTUI();
    LoadAndApplyTheme("dark"); 
}

void WriteTUI() {
    int leftbar_w = max_x / 20;
    int filemgr_w = max_x / 4;
    int main_w = max_x - leftbar_w - filemgr_w;

    int content_h = max_y - 1;
    if (content_h < 3) content_h = 3;
    int editor_h = (content_h * 3) / 4;
    int shell_h = content_h - editor_h;

    LeftBar           = newwin(content_h, leftbar_w, 0, 0);
    FileManagerWindow = newwin(content_h, filemgr_w, 0, leftbar_w);
    TextEditorWindow  = newwin(editor_h, main_w, 0, leftbar_w + filemgr_w);
    ShellWindow       = newwin(shell_h, main_w, editor_h, leftbar_w + filemgr_w);
    StatusBarWindow   = newwin(1, max_x, max_y - 1, 0);

    init_pair(1, COLOR_RED, COLOR_BLACK);
    init_pair(2, COLOR_GREEN, COLOR_BLACK);
    init_pair(3, COLOR_BLUE, COLOR_BLACK);

    box(LeftBar, 0, 0);
    mvwprintw(LeftBar, 0, 1, "Menu");

    box(FileManagerWindow, 0, 0);
    mvwprintw(FileManagerWindow, 0, 1, "Files");

    box(TextEditorWindow, 0, 0);
    mvwprintw(TextEditorWindow, 0, 1, "Editor");

    box(ShellWindow, 0, 0);
    mvwprintw(ShellWindow, 0, 1, "Shell");

    wrefresh(LeftBar);
    wrefresh(FileManagerWindow);
    wrefresh(TextEditorWindow);
    wrefresh(ShellWindow);
    wrefresh(StatusBarWindow);

    ApplyThemeToWindows(); 
}

void ClearUP_Program() {
    endwin();
}

void HandleResize() {
    endwin();
    refresh();
    getmaxyx(stdscr, max_y, max_x);
    delwin(LeftBar);
    delwin(FileManagerWindow);
    delwin(TextEditorWindow);
    delwin(ShellWindow);
    delwin(StatusBarWindow);
    clear();
    WriteTUI(); 
}

void CheckTerminalResizeFallback() {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0 && ws.ws_row > 0 && (ws.ws_col != max_x || ws.ws_row != max_y)) {
            resizeterm(ws.ws_row, ws.ws_col);
            HandleResize();
        }
    }
}

// ============================================================
//  Buffer manipulation
// ============================================================
void ClampCursorCol(EditorBuffer& buf) {
    int len = (int)Utf8Split(buf.lines[buf.cursorLine]).size();
    if (buf.cursorCol > len) buf.cursorCol = len;
    if (buf.cursorCol < 0) buf.cursorCol = 0;
}

void MoveCursorDown(EditorBuffer& buf) {
    if (buf.cursorLine < (int)buf.lines.size() - 1) buf.cursorLine++;
    ClampCursorCol(buf);
}

void MoveCursorUp(EditorBuffer& buf) {
    if (buf.cursorLine > 0) buf.cursorLine--;
    ClampCursorCol(buf);
}

void InsertCharAtCursor(EditorBuffer& buf, const string& utf8char) {
    auto chars = Utf8Split(buf.lines[buf.cursorLine]);
    chars.insert(chars.begin() + buf.cursorCol, utf8char);
    string newline;
    for (auto& c : chars) newline += c;
    buf.lines[buf.cursorLine] = newline;
    buf.cursorCol++;
    buf.modified = true;
}

void DeleteCharAtCursor(EditorBuffer& buf) {
    auto chars = Utf8Split(buf.lines[buf.cursorLine]);
    if (buf.cursorCol < (int)chars.size()) {
        chars.erase(chars.begin() + buf.cursorCol);
        string newline;
        for (auto& c : chars) newline += c;
        buf.lines[buf.cursorLine] = newline;
        buf.modified = true;
    }
}

void BackspaceAtCursor(EditorBuffer& buf) {
    if (buf.cursorCol > 0) {
        auto chars = Utf8Split(buf.lines[buf.cursorLine]);
        chars.erase(chars.begin() + (buf.cursorCol - 1));
        string newline;
        for (auto& c : chars) newline += c;
        buf.lines[buf.cursorLine] = newline;
        buf.cursorCol--;
        buf.modified = true;
    } else if (buf.cursorLine > 0) {
        int prevLen = (int)Utf8Split(buf.lines[buf.cursorLine - 1]).size();
        buf.lines[buf.cursorLine - 1] += buf.lines[buf.cursorLine];
        buf.lines.erase(buf.lines.begin() + buf.cursorLine);
        buf.cursorLine--;
        buf.cursorCol = prevLen;
        buf.modified = true;
    }
}

void NewLine(EditorBuffer& buf) {
    auto chars = Utf8Split(buf.lines[buf.cursorLine]);
    string before, after;
    for (int i = 0; i < (int)chars.size(); i++) {
        if (i < buf.cursorCol) before += chars[i]; else after += chars[i];
    }
    buf.lines[buf.cursorLine] = before;
    buf.lines.insert(buf.lines.begin() + buf.cursorLine + 1, after);
    buf.cursorLine++;
    buf.cursorCol = 0;
    buf.modified = true;
}

bool LoadFileIntoBuffer(EditorBuffer& buf, const fs::path& path) {
    ifstream f(path);
    if (!f.is_open()) return false;
    UndoHistory.clear(); // ล้างประวัติ Undo เมื่อโหลดไฟล์ใหม่
    buf.lines.clear();
    string line;
    while (getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        buf.lines.push_back(line);
    }
    if (buf.lines.empty()) buf.lines.push_back("");
    buf.filepath = path;
    buf.hasFile = true;
    buf.modified = false;
    buf.cursorLine = 0;
    buf.cursorCol = 0;
    buf.topLine = 0;
    CurrentSyntaxName = DetermineSyntaxForFile(path); 
    HLCache.clear();                                  
    return true;
}

bool SaveBufferToFile(EditorBuffer& buf) {
    if (!buf.hasFile) return false;
    ofstream f(buf.filepath, ios::trunc);
    if (!f.is_open()) return false;
    for (auto& l : buf.lines) f << l << "\n";
    buf.modified = false;
    return true;
}

void SaveSession() {
    json j;
    j["last_file"] = CurBuf.hasFile ? CurBuf.filepath.string() : "";
    j["cursor_line"] = CurBuf.cursorLine;
    j["cursor_col"] = CurBuf.cursorCol;
    j["show_line_numbers"] = ShowLineNumbers;
    j["show_cursor_line"] = ShowCursorLine;
    j["theme"] = CurTheme.name;
    ofstream f(MotherPath / ".thaivim_session.json");
    if (f.is_open()) f << j.dump(2);
}

void LoadSession() {
    fs::path sp = MotherPath / ".thaivim_session.json";
    if (!fs::exists(sp)) return;
    ifstream f(sp);
    if (!f.is_open()) return;
    try {
        json j; f >> j;
        string lastFile = j.value("last_file", "");
        if (!lastFile.empty() && fs::exists(lastFile)) {
            LoadFileIntoBuffer(CurBuf, lastFile);
            CurBuf.cursorLine = j.value("cursor_line", 0);
            if (CurBuf.cursorLine < 0 || CurBuf.cursorLine >= (int)CurBuf.lines.size()) CurBuf.cursorLine = 0;
            CurBuf.cursorCol = j.value("cursor_col", 0);
            ClampCursorCol(CurBuf);
        }
        ShowLineNumbers = j.value("show_line_numbers", false);
        ShowCursorLine = j.value("show_cursor_line", false);
        string themeName = j.value("theme", string("dark"));
        LoadAndApplyTheme(themeName);
    } catch (...) {}
}

// ============================================================
//  File manager
// ============================================================
void RefreshFileManager() {
    FMEntries.clear();
    FMEntries.push_back({"..", true});
    vector<FileEntry> dirs, files;
    try {
        LastFMWriteTime = fs::last_write_time(CurrentDir); // จำเวลาล่าสุดของโฟลเดอร์
        for (auto& entry : fs::directory_iterator(CurrentDir)) {
            FileEntry fe;
            fe.name = entry.path().filename().string();
            fe.isDir = entry.is_directory();
            if (fe.isDir) dirs.push_back(fe); else files.push_back(fe);
        }
    } catch (...) {}
    
    sort(dirs.begin(), dirs.end(), [](const FileEntry& a, const FileEntry& b){ return a.name < b.name; });
    sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b){ return a.name < b.name; });
    for (auto& d : dirs) FMEntries.push_back(d);
    for (auto& f : files) FMEntries.push_back(f);
    if(FMSelected >= (int)FMEntries.size()) FMSelected = max(0, (int)FMEntries.size() - 1);
}

void CheckDirectoryChanges() {
    try {
        if (fs::exists(CurrentDir)) {
            auto currentTime = fs::last_write_time(CurrentDir);
            if (currentTime != LastFMWriteTime) {
                RefreshFileManager();
            }
        }
    } catch (...) {}
}

string ShellEscapeSingleQuoted(const string& s) {
    string out;
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out;
}

void RunTerminalCommand(const string& cmd) {
    {
        lock_guard<mutex> lock(TermMutex);
        Term.outputLines.push_back("$ " + cmd);
        Term.running = true;
    }
    fs::path cwd = CurrentDir; 
    thread([cmd, cwd]() {
        string fullCmd = "cd '" + ShellEscapeSingleQuoted(cwd.string()) + "' && (" + cmd + ") 2>&1";
        FILE* pipe = popen(fullCmd.c_str(), "r");
        if (!pipe) {
            lock_guard<mutex> lock(TermMutex);
            Term.outputLines.push_back("[error] Failed to execute command");
            Term.running = false;
            return;
        }
        char buf[4096];
        string lineBuf;
        while (fgets(buf, sizeof(buf), pipe)) {
            lineBuf += buf;
            size_t pos;
            while ((pos = lineBuf.find('\n')) != string::npos) {
                string line = lineBuf.substr(0, pos);
                lineBuf.erase(0, pos + 1);
                lock_guard<mutex> lock(TermMutex);
                Term.outputLines.push_back(line);
            }
        }
        if (!lineBuf.empty()) {
            lock_guard<mutex> lock(TermMutex);
            Term.outputLines.push_back(lineBuf);
        }
        int rc = pclose(pipe);
        lock_guard<mutex> lock(TermMutex);
        Term.outputLines.push_back("[Command finished, exit code " + to_string(WEXITSTATUS(rc)) + "]");
        Term.running = false;
    }).detach();
}

// ============================================================
//  Rendering
// ============================================================
void RenderLeftBarWindow() {
    werase(LeftBar);
    box(LeftBar, 0, 0);
    mvwprintw(LeftBar, 0, 1, "Menu");
    wrefresh(LeftBar);
}

void RenderFileManager() {
    werase(FileManagerWindow);
    box(FileManagerWindow, 0, 0);
    string dirName = CurrentDir.filename().string();
    if (dirName.empty()) dirName = CurrentDir.string();
    mvwprintw(FileManagerWindow, 0, 1, " Files: %s", dirName.c_str());

    int h, w;
    getmaxyx(FileManagerWindow, h, w);
    int usableH = h - 2;
    if (usableH < 1) usableH = 1;
    if (FMSelected < FMTop) FMTop = FMSelected;
    if (FMSelected >= FMTop + usableH) FMTop = FMSelected - usableH + 1;

    for (int i = 0; i < usableH; i++) {
        int idx = FMTop + i;
        if (idx >= (int)FMEntries.size()) break;
        bool sel = (idx == FMSelected);
        if (sel) wattron(FileManagerWindow, A_REVERSE);
        string name = FMEntries[idx].name;
        if (FMEntries[idx].isDir) name += "/";
        mvwaddnstr(FileManagerWindow, i + 1, 1, name.c_str(), w - 2);
        if (sel) wattroff(FileManagerWindow, A_REVERSE);
    }
    wrefresh(FileManagerWindow);
}

int DigitCount(int n) {
    if (n < 10) return 1;
    int d = 0;
    while (n > 0) { d++; n /= 10; }
    return d;
}

void RenderTextEditor() {
    werase(TextEditorWindow);
    box(TextEditorWindow, 0, 0);
    string title = " Editor";
    if (CurBuf.hasFile) title += " - " + CurBuf.filepath.filename().string();
    if (CurBuf.modified) title += " [+]";
    if (!CurrentSyntaxName.empty()) title += " (" + CurrentSyntaxName + ")";
    mvwprintw(TextEditorWindow, 0, 1, "%s", title.c_str());

    int h, w;
    getmaxyx(TextEditorWindow, h, w);
    int usableH = h - 2;
    if (usableH < 1) usableH = 1;

    if (CurBuf.cursorLine < CurBuf.topLine) CurBuf.topLine = CurBuf.cursorLine;
    if (CurBuf.cursorLine >= CurBuf.topLine + usableH) CurBuf.topLine = CurBuf.cursorLine - usableH + 1;

    int lineDigits = max(4, DigitCount((int)CurBuf.lines.size()));
    int gutterW = ShowLineNumbers ? (lineDigits + 1) : 0; 

    EnsureHighlightValid(CurBuf.topLine + usableH); 

    for (int i = 0; i < usableH; i++) {
        int lineIdx = CurBuf.topLine + i;
        if (lineIdx >= (int)CurBuf.lines.size()) break;
        bool isCursorLine = (CurFocus == FocusPanel::EDITOR && ShowCursorLine && lineIdx == CurBuf.cursorLine);

        if (ShowLineNumbers) {
            string numStr = to_string(lineIdx + 1);
            int numAttr = (lineIdx == CurBuf.cursorLine) ? A_BOLD : 0;
            if (numAttr) wattron(TextEditorWindow, numAttr);
            mvwprintw(TextEditorWindow, i + 1, 1, "%*s ", lineDigits, numStr.c_str());
            if (numAttr) wattroff(TextEditorWindow, numAttr);
        }

        int maxChars = max(0, w - 2 - gutterW);
        int extraAttr = isCursorLine ? A_REVERSE : 0;

        if (isCursorLine) {
            mvwchgat(TextEditorWindow, i + 1, 1 + gutterW, maxChars, A_REVERSE, 0, NULL);
        }

        if (!CurrentSyntaxName.empty() && lineIdx < (int)HLCache.size() && HLCache[lineIdx].valid) {
            DrawHighlightedLine(TextEditorWindow, i + 1, 1 + gutterW, CurBuf.lines[lineIdx], maxChars, HLCache[lineIdx].spans, extraAttr);
        } else {
            if (extraAttr) wattron(TextEditorWindow, extraAttr);
            mvwaddnstr(TextEditorWindow, i + 1, 1 + gutterW, CurBuf.lines[lineIdx].c_str(), maxChars);
            if (extraAttr) wattroff(TextEditorWindow, extraAttr);
        }
    }

    if (CurFocus == FocusPanel::EDITOR) {
        int screenY = CurBuf.cursorLine - CurBuf.topLine + 1;
        int screenX = 1 + gutterW + CurBuf.cursorCol; 
        wmove(TextEditorWindow, screenY, screenX);
    }
    wrefresh(TextEditorWindow);
}

void RenderTerminalWindow() {
    werase(ShellWindow);
    box(ShellWindow, 0, 0);
    string title = " Shell / Terminal";
    if (Term.running) title += " [Running...]";
    mvwprintw(ShellWindow, 0, 1, "%s", title.c_str());

    int h, w;
    getmaxyx(ShellWindow, h, w);
    if (h < 4) { wrefresh(ShellWindow); return; }
    int usableH = h - 3;     
    int promptRow = h - 2;   

    {
        lock_guard<mutex> lock(TermMutex);
        int total = (int)Term.outputLines.size();
        int start = max(0, total - usableH - Term.scrollOffset);
        int end = min(total, start + usableH);
        int row = 1;
        for (int i = start; i < end; i++) {
            mvwaddnstr(ShellWindow, row++, 1, Term.outputLines[i].c_str(), w - 2);
        }
    }

    mvwhline(ShellWindow, promptRow - 1, 1, ACS_HLINE, w - 2);
    string prompt = "$ " + Term.inputLine;
    mvwaddnstr(ShellWindow, promptRow, 1, prompt.c_str(), w - 2);

    if (CurFocus == FocusPanel::TERMINAL) {
        int cx = 1 + (int)prompt.size();
        if (cx > w - 2) cx = w - 2;
        wmove(ShellWindow, promptRow, cx);
    }
    wrefresh(ShellWindow);
}

void RenderStatusBar(const string& statusMsg) {
    werase(StatusBarWindow);
    string modeStr;
    switch (CurMode) {
        case EditorMode::NORMAL:  modeStr = "NORMAL";  break;
        case EditorMode::INSERT:  modeStr = "INSERT";  break;
        case EditorMode::COMMAND: modeStr = "COMMAND"; break;
    }
    string focusStr;
    switch (CurFocus) {
        case FocusPanel::EDITOR:      focusStr = "Editor";       break;
        case FocusPanel::FILEMANAGER: focusStr = "File Manager"; break;
        case FocusPanel::TERMINAL:    focusStr = "Terminal";     break;
    }

    string left;
    if (CurMode == EditorMode::COMMAND) {
        left = CmdLineInput; 
    } else {
        left = "-- " + modeStr + " -- | Focus: " + focusStr +
               " | Ln " + to_string(CurBuf.cursorLine + 1) + ", Col " + to_string(CurBuf.cursorCol + 1) +
               " | Theme: " + CurTheme.name;
    }
    int w = getmaxx(StatusBarWindow);
    mvwaddnstr(StatusBarWindow, 0, 0, left.c_str(), w);

    if (!statusMsg.empty() && CurMode != EditorMode::COMMAND) {
        int startX = w - (int)statusMsg.size() - 1;
        if (startX > (int)left.size() + 2) mvwaddnstr(StatusBarWindow, 0, startX, statusMsg.c_str(), w - startX);
    }
    wrefresh(StatusBarWindow);
}

void RenderAll(const string& statusMsg) {
    RenderLeftBarWindow();
    RenderFileManager();
    RenderTextEditor();
    RenderTerminalWindow();
    RenderStatusBar(statusMsg);

    curs_set(CurFocus == FocusPanel::FILEMANAGER ? 0 : 1);
    WINDOW* focusedWin = TextEditorWindow;
    if (CurFocus == FocusPanel::FILEMANAGER) focusedWin = FileManagerWindow;
    else if (CurFocus == FocusPanel::TERMINAL) focusedWin = ShellWindow;
    wrefresh(focusedWin);
}

map<string, function<void(vector<string>&, string&)>> Commands;

string FocusName(FocusPanel f) {
    switch (f) {
        case FocusPanel::EDITOR: return "Editor";
        case FocusPanel::FILEMANAGER: return "File Manager";
        case FocusPanel::TERMINAL: return "Terminal";
    }
    return "";
}

void CycleFocus() {
    if (CurFocus == FocusPanel::EDITOR) CurFocus = FocusPanel::FILEMANAGER;
    else if (CurFocus == FocusPanel::FILEMANAGER) CurFocus = FocusPanel::TERMINAL;
    else CurFocus = FocusPanel::EDITOR;
}

void RegisterCommands() {
    Commands["hello"] = [](vector<string>& args, string& statusMsg) {
        statusMsg = "Hello from ThaiVim";
        LoadSession();
    };
    Commands["w"] = [](vector<string>& args, string& statusMsg) {
        if (!args.empty()) { CurBuf.filepath = args[0]; CurBuf.hasFile = true; }
        if (SaveBufferToFile(CurBuf)) statusMsg = "File saved: " + CurBuf.filepath.string();
        else statusMsg = "Save failed (ยังไม่ระบุไฟล์ - ลอง :w <path>)";
    };
    Commands["q"] = [](vector<string>&, string& statusMsg) {
        if (CurBuf.modified) statusMsg = "มีการแก้ไขที่ยังไม่บันทึก! ใช้ :q! หรือ :wq";
        else { SaveSession(); MainRunning = false; }
    };
    Commands["q!"] = [](vector<string>&, string&) { SaveSession(); MainRunning = false; };
    auto wqFn = [](vector<string>&, string&) { SaveBufferToFile(CurBuf); SaveSession(); MainRunning = false; };
    Commands["wq"] = wqFn;
    Commands["x"]  = wqFn;
    auto editFn = [](vector<string>& args, string& statusMsg) {
        if (args.empty()) { statusMsg = "ใช้งาน: :e <path>"; return; }
        if (LoadFileIntoBuffer(CurBuf, args[0])) statusMsg = "Opened file: " + args[0];
        else statusMsg = "Failed to open file: " + args[0];
    };
    Commands["e"] = Commands["edit"] = editFn;
    auto termFn = [](vector<string>&, string& statusMsg) {
        CurFocus = FocusPanel::TERMINAL;
        statusMsg = "Focus: Terminal (ESC หรือ Tab กลับ Editor)";
    };
    Commands["term"] = Commands["sh"] = termFn;
    Commands["set"] = [](vector<string>& args, string& statusMsg) {
        if (args.empty()) { statusMsg = "ใช้งาน: :set number | :set nonumber | :set cursorline | :set nocursorline | :set theme <dark|light|custom>"; return; }
        if (args[0] == "number") ShowLineNumbers = true;
        else if (args[0] == "nonumber") ShowLineNumbers = false;
        else if (args[0] == "cursorline") { ShowCursorLine = true; statusMsg = "เปิด cursorline"; }
        else if (args[0] == "nocursorline") { ShowCursorLine = false; statusMsg = "ปิด cursorline"; }
        else if (args[0] == "theme") {
            if (args.size() < 2) { statusMsg = "ใช้งาน: :set theme <dark|light|custom|ชื่อธีมของคุณ>"; return; }
            if (LoadAndApplyTheme(args[1])) statusMsg = "เปลี่ยน theme เป็น: " + args[1];
            else statusMsg = "ไม่พบธีม: themes/" + args[1] + ".json";
        }
        else statusMsg = "ไม่รู้จักตัวเลือก: " + args[0];
    };
    Commands["help"] = [](vector<string>&, string& statusMsg) {
        statusMsg = ":w :q :q! :wq/:x :e<path> :NN(บรรทัด) :!cmd :term :set number/nonumber :set cursorline/nocursorline :set theme <dark|light|custom> | Ctrl+S save, Ctrl+Q quit, Ctrl+O open, Ctrl+N new, Ctrl+W close";
    };
}

void ParseAndExecuteCommand(const string& rawCmd, string& statusMsg) {
    if (rawCmd.empty()) return;

    if (rawCmd[0] == '!') {
        string shellCmd = rawCmd.substr(1);
        if (!shellCmd.empty()) {
            RunTerminalCommand(shellCmd);
            CurFocus = FocusPanel::TERMINAL;
            statusMsg = "รันคำสั่งใน Terminal: " + shellCmd;
        }
        return;
    }

    bool allDigits = all_of(rawCmd.begin(), rawCmd.end(), [](unsigned char c){ return isdigit(c); });
    if (allDigits) {
        try {
            long parsed = stol(rawCmd);
            int ln = (int)parsed - 1;
            if (ln < 0) ln = 0;
            if (ln >= (int)CurBuf.lines.size()) ln = (int)CurBuf.lines.size() - 1;
            CurBuf.cursorLine = ln;
            ClampCursorCol(CurBuf);
        } catch (const std::exception&) {
            statusMsg = "เลขบรรทัดไม่ถูกต้อง: " + rawCmd;
        }
        return;
    }

    vector<string> tokens;
    string cur;
    for (char c : rawCmd) {
        if (c == ' ') { if (!cur.empty()) { tokens.push_back(cur); cur.clear(); } }
        else cur += c;
    }
    if (!cur.empty()) tokens.push_back(cur);
    if (tokens.empty()) return;

    string name = tokens[0];
    vector<string> args(tokens.begin() + 1, tokens.end());
    auto it = Commands.find(name);
    if (it != Commands.end()) it->second(args, statusMsg);
    else statusMsg = "Unknown command: " + name + " (พิมพ์ :help ดูคำสั่งทั้งหมด)";
}

// ============================================================
//  Key handling
// ============================================================
void HandleInsertKey(int ch, string& statusMsg) {
    if (ch == 27) { 
        CurMode = EditorMode::NORMAL;
        if (CurBuf.cursorCol > 0) CurBuf.cursorCol--;
        statusMsg = "-- NORMAL --";
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { BackspaceAtCursor(CurBuf); return; }
    if (ch == '\n' || ch == KEY_ENTER || ch == 13) { SaveUndo(); NewLine(CurBuf); return; }
    if (ch == KEY_LEFT) { if (CurBuf.cursorCol > 0) CurBuf.cursorCol--; return; }
    if (ch == KEY_RIGHT) {
        int len = (int)Utf8Split(CurBuf.lines[CurBuf.cursorLine]).size();
        if (CurBuf.cursorCol < len) CurBuf.cursorCol++;
        return;
    }
    if (ch == KEY_UP) { MoveCursorUp(CurBuf); return; }
    if (ch == KEY_DOWN) { MoveCursorDown(CurBuf); return; }
    if (ch == KEY_RESIZE || ch == ERR) return;
    
    // Tab ใน INSERT mode จะทำการเติม Space (Soft-Tab) เพื่อให้ง่ายแบบ VS Code
    if (ch == '\t') {
        SaveUndo();
        for (int i = 0; i < 4; i++) InsertCharAtCursor(CurBuf, " ");
        return;
    }
    if (ch < 32) return; 

    string utf8char = ReadUtf8Char(ch); 
    InsertCharAtCursor(CurBuf, utf8char);
}

void HandleCommandKey(int ch, string& statusMsg) {
    if (ch == ERR || ch == KEY_RESIZE) return;
    if (ch == 27) { CurMode = EditorMode::NORMAL; CmdLineInput = ""; CmdHistoryPos = -1; return; }
    if (ch == '\n' || ch == KEY_ENTER || ch == 13) {
        string cmd = CmdLineInput.size() > 1 ? CmdLineInput.substr(1) : "";
        if (!cmd.empty()) CmdHistory.push_back(cmd);
        CmdHistoryPos = -1;
        ParseAndExecuteCommand(cmd, statusMsg);
        CmdLineInput = "";
        if (MainRunning) CurMode = EditorMode::NORMAL;
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        if (CmdLineInput.size() > 1) CmdLineInput.pop_back();
        else { CurMode = EditorMode::NORMAL; CmdLineInput = ""; }
        return;
    }
    if (ch == KEY_UP) { 
        if (!CmdHistory.empty() && CmdHistoryPos < (int)CmdHistory.size() - 1) {
            CmdHistoryPos++;
            CmdLineInput = ":" + CmdHistory[CmdHistory.size() - 1 - CmdHistoryPos];
        }
        return;
    }
    if (ch == KEY_DOWN) {
        if (CmdHistoryPos > 0) { CmdHistoryPos--; CmdLineInput = ":" + CmdHistory[CmdHistory.size() - 1 - CmdHistoryPos]; }
        else if (CmdHistoryPos == 0) { CmdHistoryPos = -1; CmdLineInput = ":"; }
        return;
    }
    if (ch >= 32 && ch < 127) CmdLineInput += (char)ch;
}

void HandleNormalKey(int ch, string& statusMsg) {
    if (ch == ERR || ch == KEY_RESIZE) return;
    int lineLen = (int)Utf8Split(CurBuf.lines[CurBuf.cursorLine]).size();

    if (PendingKey != 0) {
        char p = PendingKey;
        PendingKey = 0;
        if (p == 'd' && ch == 'd') {
            SaveUndo();
            CurBuf.clipboard = { CurBuf.lines[CurBuf.cursorLine] };
            if (CurBuf.lines.size() > 1) {
                CurBuf.lines.erase(CurBuf.lines.begin() + CurBuf.cursorLine);
                if (CurBuf.cursorLine >= (int)CurBuf.lines.size()) CurBuf.cursorLine = (int)CurBuf.lines.size() - 1;
            } else {
                CurBuf.lines[0] = "";
            }
            CurBuf.cursorCol = 0;
            CurBuf.modified = true;
            statusMsg = "Line deleted (dd)";
        } else if (p == 'y' && ch == 'y') {
            CurBuf.clipboard = { CurBuf.lines[CurBuf.cursorLine] };
            statusMsg = "Line copied (yy)";
        } else if (p == 'g' && ch == 'g') {
            CurBuf.cursorLine = 0;
            CurBuf.cursorCol = 0;
        } else {
            HandleNormalKey(ch, statusMsg);
            return;
        }
        ClampCursorCol(CurBuf);
        return;
    }

    switch (ch) {
        case 'h': case KEY_LEFT:  if (CurBuf.cursorCol > 0) CurBuf.cursorCol--; break;
        case 'l': case KEY_RIGHT: if (CurBuf.cursorCol < lineLen) CurBuf.cursorCol++; break;
        case 'j': case KEY_DOWN:  MoveCursorDown(CurBuf); break;
        case 'k': case KEY_UP:    MoveCursorUp(CurBuf); break;
        case '0': CurBuf.cursorCol = 0; break;
        case '$': CurBuf.cursorCol = lineLen; break;
        case 'G': CurBuf.cursorLine = (int)CurBuf.lines.size() - 1; break;
        case 'g': PendingKey = 'g'; break;
        case 'd': PendingKey = 'd'; break;
        case 'y': PendingKey = 'y'; break;
        case 'i': SaveUndo(); CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --"; break;
        case 'a': SaveUndo(); if (lineLen > 0) CurBuf.cursorCol++; CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --"; break;
        case 'A': SaveUndo(); CurBuf.cursorCol = lineLen; CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --"; break;
        case 'I': SaveUndo(); CurBuf.cursorCol = 0; CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --"; break;
        case 'o':
            SaveUndo();
            CurBuf.lines.insert(CurBuf.lines.begin() + CurBuf.cursorLine + 1, "");
            CurBuf.cursorLine++; CurBuf.cursorCol = 0;
            CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --";
            break;
        case 'O':
            SaveUndo();
            CurBuf.lines.insert(CurBuf.lines.begin() + CurBuf.cursorLine, "");
            CurBuf.cursorCol = 0;
            CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --";
            break;
        case 'x': SaveUndo(); DeleteCharAtCursor(CurBuf); break;
        case 'p':
            SaveUndo();
            if (!CurBuf.clipboard.empty()) {
                CurBuf.lines.insert(CurBuf.lines.begin() + CurBuf.cursorLine + 1, CurBuf.clipboard[0]);
                CurBuf.cursorLine++;
                CurBuf.modified = true;
            }
            break;
        case 'P':
            SaveUndo();
            if (!CurBuf.clipboard.empty()) {
                CurBuf.lines.insert(CurBuf.lines.begin() + CurBuf.cursorLine, CurBuf.clipboard[0]);
                CurBuf.modified = true;
            }
            break;
        case ':': CurMode = EditorMode::COMMAND; CmdLineInput = ":"; break;
        case '\t': CycleFocus(); statusMsg = "Focus: " + FocusName(CurFocus); break;
        // --- Windows/VS Code Copy & Paste keys in Normal Mode ---
        case 3: // Ctrl + C
            CurBuf.clipboard = { CurBuf.lines[CurBuf.cursorLine] };
            statusMsg = "Line copied (Ctrl+C)";
            break;
        case 22: // Ctrl + V
            SaveUndo();
            if (!CurBuf.clipboard.empty()) {
                CurBuf.lines.insert(CurBuf.lines.begin() + CurBuf.cursorLine + 1, CurBuf.clipboard[0]);
                CurBuf.cursorLine++;
                CurBuf.modified = true;
                statusMsg = "Pasted (Ctrl+V)";
            }
            break;
        case 24: // Ctrl + X
            SaveUndo();
            CurBuf.clipboard = { CurBuf.lines[CurBuf.cursorLine] };
            if (CurBuf.lines.size() > 1) {
                CurBuf.lines.erase(CurBuf.lines.begin() + CurBuf.cursorLine);
                if (CurBuf.cursorLine >= (int)CurBuf.lines.size()) CurBuf.cursorLine = (int)CurBuf.lines.size() - 1;
            } else {
                CurBuf.lines[0] = "";
            }
            CurBuf.cursorCol = 0;
            CurBuf.modified = true;
            statusMsg = "Cut (Ctrl+X)";
            break;
        default: break;
    }
    ClampCursorCol(CurBuf);
}

void HandleFileManagerKey(int ch, string& statusMsg) {
    if (ch == ERR || ch == KEY_RESIZE) return;
    switch (ch) {
        case 'j': case KEY_DOWN: if (FMSelected < (int)FMEntries.size() - 1) FMSelected++; break;
        case 'k': case KEY_UP:   if (FMSelected > 0) FMSelected--; break;
        case 'r': RefreshFileManager(); statusMsg = "File Manager Refreshed"; break; // Manual Refresh
        case '\n': case KEY_ENTER: case 13: {
            if (FMEntries.empty()) break;
            auto& sel = FMEntries[FMSelected];
            if (sel.isDir) {
                if (sel.name == "..") CurrentDir = CurrentDir.parent_path();
                else CurrentDir = CurrentDir / sel.name;
                RefreshFileManager();
            } else {
                fs::path filePath = CurrentDir / sel.name;
                if (LoadFileIntoBuffer(CurBuf, filePath)) {
                    statusMsg = "Opened file: " + filePath.string();
                    CurFocus = FocusPanel::EDITOR;
                } else {
                    statusMsg = "Failed to open file: " + filePath.string();
                }
            }
            break;
        }
        case '\t': CycleFocus(); statusMsg = "Focus: " + FocusName(CurFocus); break;
        default: break;
    }
}

void HandleTerminalKey(int ch, string& statusMsg) {
    if (ch == ERR || ch == KEY_RESIZE) return;
    if (ch == 27) { CurFocus = FocusPanel::EDITOR; statusMsg = "-- NORMAL --"; return; }
    if (ch == '\t') { CycleFocus(); statusMsg = "Focus: " + FocusName(CurFocus); return; }
    if (ch == '\n' || ch == KEY_ENTER || ch == 13) {
        string cmd = Term.inputLine;
        if (!cmd.empty()) {
            Term.history.push_back(cmd);
            Term.historyPos = -1;
            if (cmd == "clear") {
                lock_guard<mutex> lock(TermMutex);
                Term.outputLines.clear();
            } else {
                RunTerminalCommand(cmd);
            }
        }
        Term.inputLine = "";
        Term.scrollOffset = 0;
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { if (!Term.inputLine.empty()) Term.inputLine.pop_back(); return; }
    if (ch == KEY_UP) {
        if (!Term.history.empty() && Term.historyPos < (int)Term.history.size() - 1) {
            Term.historyPos++;
            Term.inputLine = Term.history[Term.history.size() - 1 - Term.historyPos];
        }
        return;
    }
    if (ch == KEY_DOWN) {
        if (Term.historyPos > 0) { Term.historyPos--; Term.inputLine = Term.history[Term.history.size() - 1 - Term.historyPos]; }
        else { Term.historyPos = -1; Term.inputLine = ""; }
        return;
    }
    if (ch == KEY_PPAGE) { Term.scrollOffset += 5; return; }
    if (ch == KEY_NPAGE) { Term.scrollOffset = max(0, Term.scrollOffset - 5); return; }
    if (ch < 32) return;
    Term.inputLine += ReadUtf8Char(ch); 
}

void HandleKey(int ch, string& statusMsg) {
    if (ch == ERR) return;       
    if (ch == KEY_RESIZE) { HandleResize(); return; }

    // --- Global VS Code / Windows Keybindings ---
    if (ch == 19) { ParseAndExecuteCommand("w", statusMsg); return; } // Ctrl + S (Save)
    if (ch == 17) { ParseAndExecuteCommand("q", statusMsg); return; } // Ctrl + Q (Quit)
    if (ch == 16) { CurMode = EditorMode::COMMAND; CmdLineInput = ":"; statusMsg = "Command Palette (Ctrl+P)"; return; } // Ctrl + P (Command)
    if (ch == 2)  { CurFocus = (CurFocus == FocusPanel::FILEMANAGER) ? FocusPanel::EDITOR : FocusPanel::FILEMANAGER; statusMsg = "Focus: " + FocusName(CurFocus); return; } // Ctrl + B (Toggle Sidebar)
    if (ch == 26 && CurMode != EditorMode::INSERT) { // Ctrl + Z (Undo) ในโหมด Normal
        if (!UndoHistory.empty()) {
            auto snap = UndoHistory.back();
            UndoHistory.pop_back();
            CurBuf.lines = snap.lines;
            CurBuf.cursorLine = snap.cursorLine;
            CurBuf.cursorCol = snap.cursorCol;
            CurBuf.modified = true;
            statusMsg = "Undo (Ctrl+Z)";
        } else {
            statusMsg = "Nothing to undo";
        }
        return;
    }

    if (CurFocus == FocusPanel::FILEMANAGER) { HandleFileManagerKey(ch, statusMsg); return; }
    if (CurFocus == FocusPanel::TERMINAL) { HandleTerminalKey(ch, statusMsg); return; }
    switch (CurMode) {
        case EditorMode::NORMAL:  HandleNormalKey(ch, statusMsg);  break;
        case EditorMode::INSERT:  HandleInsertKey(ch, statusMsg);  break;
        case EditorMode::COMMAND: HandleCommandKey(ch, statusMsg); break;
    }
}

// ============================================================
//  Threads
// ============================================================
void FileManager_thread() {
}

void main_thread() {
    SetupTerminal();
    RegisterCommands();
    CurrentDir = StartCwd; 
    RefreshFileManager();

    string statusMsg = "Welcome to ThaiVim! Use :help for commands, Tab to switch panels";
    LoadSession();

    RenderAll(statusMsg);

    while (MainRunning) {
        int ch = getch(); 
        HandleKey(ch, statusMsg);
        
        CheckDirectoryChanges(); // เช็ค Auto Refresh File Manager ทันทีใน Loop 
        CheckTerminalResizeFallback(); 
        
        if (!MainRunning) break;
        RenderAll(statusMsg);
    }

    ClearUP_Program();

    lock_guard<mutex> lock(QueueMutex);
    QueueNode node;
    node.sender = "main_thread";
    node.action = "QUIT";
    MainQueue.push(node);
}

int main(int argc,char* args[]) {
    setlocale(LC_ALL, ""); 

    try {
        StartCwd = fs::current_path();
    } catch (...) {
        StartCwd = fs::path("."); 
    }

    MotherPath = fs::absolute(fs::canonical(args[0])).parent_path().parent_path();

    thread MainThread(main_thread);
    MainThread.detach();

    while (MainRunning) {
        try {
            QueueNode node;
            bool has = false;
            {
                lock_guard<mutex> lock(QueueMutex);
                if (!MainQueue.empty()) {
                    node = MainQueue.front();
                    MainQueue.pop();
                    has = true;
                }
            }
            if (has) {
                if (node.action == "QUIT") MainRunning = false;
            } else {
                this_thread::sleep_for(chrono::milliseconds(20)); 
            }
        }
        catch (const std::exception& e) {
            ClearUP_Program();
            std::cerr << e.what() << '\n';
            exit(1);
        }
    }
    return 0;
}