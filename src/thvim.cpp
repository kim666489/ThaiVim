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
queue<QueueNode> FileManagerQueue; // Reserved for future async tasks (e.g. scanning large directories)
queue<QueueNode> TextEditorQueue;  // Reserved for future async tasks (e.g. auto-save)
queue<QueueOtherNode> OtherQueue;
mutex QueueMutex; // Prevent race conditions when accessing MainQueue from multiple threads

fs::path MotherPath;
fs::path StartCwd; // ไดเรกทอรีที่ user อยู่ตอนสั่งรัน thaivim (ก่อนโปรแกรมจะแตะ path ใดๆ)
int max_x,max_y;
WINDOW* FileManagerWindow;
WINDOW* TextEditorWindow;
WINDOW* ShellWindow;     // Keep original name from prototype (title 'Shell'), now a real terminal panel
WINDOW* LeftBar;
WINDOW* StatusBarWindow; // Single-line status bar at the bottom of the screen (new)

void WriteTUI();

// ============================================================
//  UTF-8 helpers (จำเป็นสำหรับข้อความภาษาไทยที่ 1 ตัวอักษร = 2-3 byte)
//  หมายเหตุ: เวอร์ชันนี้ยังไม่คำนวณ "ความกว้างจอ" ของสระ/วรรณยุกต์แบบ
//  zero-width อย่างสมบูรณ์ (สระ/วรรณยุกต์ลอยจะถูกนับเป็น 1 ช่องเหมือนตัวอักษรทั่วไป)
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
        if (i + len > s.size()) len = 1; // กันข้อมูลเพี้ยน
        result.push_back(s.substr(i, len));
        i += len;
    }
    return result;
}

// อ่านตัวอักษร UTF-8 หนึ่งตัวจาก input ของ ncurses โดยเริ่มจาก byte แรกที่รับมาแล้ว
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
//  Editor / UI state
// ============================================================
enum class EditorMode { NORMAL, INSERT, COMMAND };
enum class FocusPanel { EDITOR, FILEMANAGER, TERMINAL };

struct EditorBuffer {
    vector<string> lines = {""};
    fs::path filepath;
    bool hasFile = false;
    bool modified = false;
    int cursorLine = 0;
    int cursorCol = 0;   // index เป็น "ตัวอักษร utf8" ไม่ใช่ byte
    int topLine = 0;     // สำหรับ scroll
    vector<string> clipboard;
};

EditorBuffer CurBuf;
EditorMode CurMode = EditorMode::NORMAL;
FocusPanel CurFocus = FocusPanel::EDITOR;
char PendingKey = 0; // สำหรับคำสั่งสองปุ่ม เช่น dd, yy, gg
string CmdLineInput = "";
bool ShowLineNumbers = false; // :set number / :set nonumber
bool ShowCursorLine = false;  // :set cursorline / :set nocursorline

vector<string> CmdHistory;   // ประวัติคำสั่งโหมด ':'
int CmdHistoryPos = -1;

struct FileEntry { string name; bool isDir; };
fs::path CurrentDir;
vector<FileEntry> FMEntries;
int FMSelected = 0;
int FMTop = 0;

// ---- Terminal panel state ----
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
mutex TermMutex; // ป้องกัน outputLines ถูกเข้าถึงพร้อมกันจาก background thread กับ UI thread

// ============================================================
//  Syntax highlighting (โหลดจากไฟล์ .json ใน folder syntax/)
//
//  syntax/config.json   -> map นามสกุลไฟล์ -> ชื่อไฟล์นิยาม syntax (เช่น "cpp")
//  syntax/<name>.json   -> นิยาม keyword / type / comment / string ของภาษานั้น
//
//  เพิ่มภาษาใหม่ทำได้โดยเพิ่มไฟล์ .json ใหม่ใน syntax/ แล้วเติม mapping ใน
//  config.json เท่านั้น ไม่ต้อง compile โปรแกรมใหม่ (ดูรายละเอียดใน README)
//
//  Performance: การ highlight ถูก cache ต่อบรรทัด (HLCache) โดย hash เนื้อหา
//  บรรทัดไว้เทียบ ถ้าบรรทัดไหนไม่เปลี่ยนและ "สถานะเข้า" (เช่น อยู่ใน /* ... */
//  ต่อจากบรรทัดก่อนหรือเปล่า) ก็ยังเหมือนเดิม จะข้ามการคำนวณซ้ำทันที (O(1))
//  ทำให้ไฟล์ใหญ่เลื่อนดู/แก้ไขได้โดยไม่มีการ re-parse ทั้งไฟล์ทุกเฟรม
//  แก้ไขบรรทัดไหน จะ mismatch แค่บรรทัดนั้น (และบรรทัดถัดไปถ้าสถานะเปลี่ยนจริง)
// ============================================================
enum HRole { HR_NONE = 0, HR_KEYWORD, HR_TYPE, HR_STRING, HR_COMMENT, HR_NUMBER, HR_PREPROC, HR_COUNT };

struct HighlightSpan { int start; int length; int role; }; // start/length เป็น byte offset ของบรรทัด

struct SyntaxDef {
    string name;
    string lineComment;
    string blockStart, blockEnd;
    vector<string> stringDelims; // แต่ละตัวควรยาว 1 ตัวอักษร เช่น "\"" หรือ "'"
    bool preprocessor = false;   // true = บรรทัดที่ (หลัง trim ช่องว่าง) ขึ้นต้นด้วย # จะได้สี preprocessor ทั้งบรรทัด
    bool numbers = true;
    set<string> keywords;
    set<string> types;
};

map<string, SyntaxDef> SyntaxDefs;   // key = ชื่อ syntax เช่น "cpp", "python"
map<string, string> ExtToSyntax;     // key = นามสกุลไฟล์ (มีจุดนำหน้า, ตัวพิมพ์เล็ก) -> ชื่อ syntax
string CurrentSyntaxName = "";       // syntax ของไฟล์ที่เปิดอยู่ตอนนี้ ("" = ปิด highlight)

struct LineHLCache {
    size_t hash = 0;
    bool valid = false;
    bool startState = false; // อยู่ใน block comment ตอนเข้าบรรทัดนี้หรือไม่
    bool endState = false;   // อยู่ใน block comment ตอนออกจากบรรทัดนี้หรือไม่ (ส่งต่อบรรทัดถัดไป)
    vector<HighlightSpan> spans;
};
vector<LineHLCache> HLCache; // ขนานกับ CurBuf.lines

// ---- Theme (light / dark / custom) ----
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
        return; // config.json เสีย/parse ไม่ได้ -> ไม่ highlight อะไรเลย แต่โปรแกรมยังรันได้ปกติ
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
            // ไฟล์ json ของภาษานี้เสีย -> ข้าม ไม่ให้ทั้งโปรแกรมพังเพราะภาษาเดียว
        }
    }
}

// หา syntax name จากนามสกุลไฟล์ (คืนค่า "" ถ้าไม่รู้จัก -> ไม่ highlight ไฟล์นี้)
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

// สแกนบรรทัดเดียว (byte-based) -> highlight spans ตาม syntax def ที่กำหนด
// startInBlockComment = เข้ามาตอนที่ยังอยู่ใน /* ... */ ที่ยังไม่ปิดจากบรรทัดก่อนหรือไม่
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

// ทำให้ HLCache[0..uptoLine] ใช้งานได้ (ถูกต้อง+ไม่เก่า) โดย recompute เฉพาะบรรทัด
// ที่ hash เปลี่ยนไปจริงๆ (บรรทัดที่เนื้อหา+สถานะเข้าเหมือนเดิม จะ "reuse" ทันที O(1))
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
            state = c.endState; // ไม่เปลี่ยน -> ข้ามการคำนวณซ้ำ
            continue;
        }
        c.hash = h;
        c.startState = state;
        c.spans = ComputeSpans(CurBuf.lines[i], def, state, c.endState);
        c.valid = true;
        state = c.endState;
    }
}

// วาดบรรทัดพร้อมสีตาม spans ที่คำนวณไว้ จำกัดความกว้างเป็นจำนวน "ตัวอักษร utf8" (ไม่ใช่ byte)
// extraAttr = ใส่ attribute เพิ่ม (เช่น A_REVERSE สำหรับ cursorline) ทับทุก segment โดยไม่ทำลายสี syntax เดิม
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

// อัปเดต init_pair ของทุก role ตาม theme ที่กำหนด (pair 20+role = สี syntax, pair 30 = พื้นหลัง/ตัวอักษรปกติ)
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

// ตั้งพื้นหลังของทุก panel ให้ตรง theme (เรียกใหม่หลัง WriteTUI สร้าง window ใหม่ หรือเปลี่ยน theme)
// ใช้ wbkgd ผูกกับหมายเลข color pair (ไม่ใช่ค่าสีตรงๆ) ดังนั้นถ้า init_pair(30,...) เปลี่ยนภายหลัง
// หน้าต่างเดิมจะแสดงสีใหม่ทันทีในรอบ werase/refresh ถัดไปโดยไม่ต้องสร้างหน้าต่างใหม่
void ApplyThemeToWindows() {
    WINDOW* wins[] = { LeftBar, FileManagerWindow, TextEditorWindow, ShellWindow, StatusBarWindow };
    for (auto* w : wins) {
        if (!w) continue;
        wbkgd(w, COLOR_PAIR(30));
    }
}

// โหลด theme จาก themes/<name>.json แล้ว apply ทันที ("dark" / "light" / "custom" หรือ theme ที่ผู้ใช้เพิ่มเอง)
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
//  Setup / layout (รองรับ real-time resize)
// ============================================================
void SetupTerminal() {
    initscr();
    start_color();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);
    timeout(50); // getch() ไม่ block เกิน 50ms -> ทำให้ terminal output/resize อัปเดตแบบ real-time ได้แม้ไม่มีการกดปุ่ม

    getmaxyx(stdscr, max_y, max_x);

    LoadSyntaxDefs();
    WriteTUI();
    LoadAndApplyTheme("dark"); // ธีมเริ่มต้น (LoadSession จะโหลดธีมที่เคยตั้งไว้ทับอีกที ถ้ามี)
}

void WriteTUI() {
    // คำนวณความกว้าง/สูง สำหรับแบ่งพื้นที่ (เผื่อแถวสุดท้าย 1 บรรทัดไว้ให้ StatusBar)
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

    // กำหนดสี
    init_pair(1, COLOR_RED, COLOR_BLACK);
    init_pair(2, COLOR_GREEN, COLOR_BLACK);
    init_pair(3, COLOR_BLUE, COLOR_BLACK);

    // วาดขอบและชื่อ Window (เนื้อหาจริงจะถูกวาดทับใน RenderAll ทุก loop)
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

    ApplyThemeToWindows(); // ให้พื้นหลังตรง theme ปัจจุบันทันทีที่สร้าง/สร้างใหม่ (resize)
}

void ClearUP_Program() {
    endwin();
}

// เรียกเมื่อขนาด terminal เปลี่ยน (จาก KEY_RESIZE หรือ ioctl poll) -> สร้างหน้าต่างใหม่ตามขนาดจริง
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
    WriteTUI(); // เนื้อหา (CurBuf/FMEntries/Term.outputLines) ยังอยู่ครบ จะถูกวาดใหม่โดย RenderAll() รอบถัดไป
}

// fallback: บาง terminal/SSH session ไม่ส่ง KEY_RESIZE ให้ ncurses เสมอไป จึง poll ขนาดจริงเทียบของเดิมทุก tick
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
    CurrentSyntaxName = DetermineSyntaxForFile(path); // เลือก syntax ตามนามสกุลไฟล์ (ดู syntax/config.json)
    HLCache.clear();                                  // ไฟล์ใหม่ -> เริ่ม cache highlight ใหม่ทั้งหมด
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

// ============================================================
//  Session persistence (ใช้ nlohmann::json ตามที่ include ไว้เดิม)
// ============================================================
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
    } catch (...) {
        // session file เสีย ไม่เป็นไร ข้ามไป
    }
}

// ============================================================
//  File manager
// ============================================================
void RefreshFileManager() {
    FMEntries.clear();
    FMEntries.push_back({"..", true});
    vector<FileEntry> dirs, files;
    try {
        for (auto& entry : fs::directory_iterator(CurrentDir)) {
            FileEntry fe;
            fe.name = entry.path().filename().string();
            fe.isDir = entry.is_directory();
            if (fe.isDir) dirs.push_back(fe); else files.push_back(fe);
        }
    } catch (...) {
        // เข้าโฟลเดอร์ไม่ได้ (สิทธิ์ไม่พอ ฯลฯ)
    }
    sort(dirs.begin(), dirs.end(), [](const FileEntry& a, const FileEntry& b){ return a.name < b.name; });
    sort(files.begin(), files.end(), [](const FileEntry& a, const FileEntry& b){ return a.name < b.name; });
    for (auto& d : dirs) FMEntries.push_back(d);
    for (auto& f : files) FMEntries.push_back(f);
    FMSelected = 0;
    FMTop = 0;
}

// ============================================================
//  Terminal: รันคำสั่ง shell แบบ background thread (ไม่ freeze UI)
// ============================================================
// escape เครื่องหมาย ' ใน path ก่อนเอาไปประกอบเป็นคำสั่ง shell (ป้องกัน path ที่มี
// single quote ทำให้ cd พัง เช่น โฟลเดอร์ชื่อ "John's Files")
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
    fs::path cwd = CurrentDir; // รันคำสั่งในไดเรกทอรีที่ File Manager เปิดอยู่
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

// จำนวนหลักของ n (อย่างน้อย 1 หลัก) ใช้คำนวณความกว้างช่องเลขบรรทัดแบบ dynamic
// เดิม gutter กว้างคงที่ 4 หลัก ("%4d ") ทำให้ไฟล์ที่มีมากกว่า 9999 บรรทัด เลขบรรทัด
// ล้นช่องและบิดเบี้ยว (บั๊ก TUI) ตอนนี้คำนวณความกว้างจากจำนวนบรรทัดทั้งหมดของไฟล์แทน
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

    // ความกว้างเลขบรรทัด: อย่างน้อย 4 หลักเหมือนเดิม แต่ขยายอัตโนมัติถ้าไฟล์มีบรรทัด
    // เยอะกว่านั้น (เช่นไฟล์ 12000 บรรทัด -> 5 หลัก) กันเลขบรรทัดล้น/ทับเนื้อหา
    int lineDigits = max(4, DigitCount((int)CurBuf.lines.size()));
    int gutterW = ShowLineNumbers ? (lineDigits + 1) : 0; // ":set number" (+1 = ช่องว่างคั่น)

    EnsureHighlightValid(CurBuf.topLine + usableH); // เตรียม highlight cache เฉพาะช่วงที่มองเห็น (+กันชนท้ายจอ)

    for (int i = 0; i < usableH; i++) {
        int lineIdx = CurBuf.topLine + i;
        if (lineIdx >= (int)CurBuf.lines.size()) break;
        bool isCursorLine = (CurFocus == FocusPanel::EDITOR && ShowCursorLine && lineIdx == CurBuf.cursorLine);

        if (ShowLineNumbers) {
            string numStr = to_string(lineIdx + 1);
            // เลขบรรทัดของ cursor line ให้เด่นกว่าบรรทัดอื่น (bold) เป็นตัวช่วยมองเห็นตำแหน่ง
            // cursor ได้ง่ายแม้ตอนที่ยังไม่เปิด :set cursorline
            int numAttr = (lineIdx == CurBuf.cursorLine) ? A_BOLD : 0;
            if (numAttr) wattron(TextEditorWindow, numAttr);
            mvwprintw(TextEditorWindow, i + 1, 1, "%*s ", lineDigits, numStr.c_str());
            if (numAttr) wattroff(TextEditorWindow, numAttr);
        }

        int maxChars = max(0, w - 2 - gutterW);
        int extraAttr = isCursorLine ? A_REVERSE : 0;

        if (isCursorLine) {
            // ไฮไลต์พื้นหลังทั้งบรรทัดของ cursor ก่อน (รวมพื้นที่ว่างท้ายบรรทัดด้วย)
            // เหมือน :set cursorline ของ vim/nvim ช่วยให้เห็นตำแหน่งบรรทัดปัจจุบันชัดเจน
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
        int screenX = 1 + gutterW + CurBuf.cursorCol; // ประมาณ 1 คอลัมน์ต่อ 1 ตัวอักษร utf8
        wmove(TextEditorWindow, screenY, screenX);
    }
    wrefresh(TextEditorWindow);
}

// ShellWindow ถูกใช้เป็น Terminal panel จริง (คำสั่ง shell รันจริง ผ่าน RunTerminalCommand)
void RenderTerminalWindow() {
    werase(ShellWindow);
    box(ShellWindow, 0, 0);
    string title = " Shell / Terminal";
    if (Term.running) title += " [Running...]";
    mvwprintw(ShellWindow, 0, 1, "%s", title.c_str());

    int h, w;
    getmaxyx(ShellWindow, h, w);
    if (h < 4) { wrefresh(ShellWindow); return; }
    int usableH = h - 3;     // แถวสำหรับ scrollback
    int promptRow = h - 2;   // แถวสำหรับพิมพ์คำสั่ง

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
        left = CmdLineInput; // แสดง ":cmd" ที่กำลังพิมพ์ทับแถบสถานะ เหมือน vim/nvim
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

// ============================================================
//  ระบบคำสั่งแบบ nvim (command registry) — ":command args..."
//  เพิ่มคำสั่งใหม่ได้ง่ายๆ โดยเพิ่ม Commands["ชื่อคำสั่ง"] = lambda ใน RegisterCommands()
// ============================================================
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
        statusMsg = ":w :q :q! :wq/:x :e<path> :NN(บรรทัด) :!cmd :term :set number/nonumber :set cursorline/nocursorline :set theme <dark|light|custom>";
    };
}

// รองรับ ":!cmd" (รันคำสั่ง shell ทันที) และ ":42" (กระโดดไปบรรทัด 42) นอกเหนือจาก command registry
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
        // บั๊กเดิม: stoi() throw std::out_of_range ถ้าผู้ใช้พิมพ์ ":999999999999999" (เกินช่วง int)
        // แล้วไม่มีใครดัก exception นี้ใน UI thread (main_thread ไม่มี try/catch) ทำให้ทั้งโปรแกรม
        // เรียก std::terminate() และ crash ทันที ตอนนี้ดักไว้แล้วแค่แสดง status message แทน
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
    if (ch == 27) { // ESC
        CurMode = EditorMode::NORMAL;
        if (CurBuf.cursorCol > 0) CurBuf.cursorCol--;
        statusMsg = "-- NORMAL --";
        return;
    }
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { BackspaceAtCursor(CurBuf); return; }
    if (ch == '\n' || ch == KEY_ENTER || ch == 13) { NewLine(CurBuf); return; }
    if (ch == KEY_LEFT) { if (CurBuf.cursorCol > 0) CurBuf.cursorCol--; return; }
    if (ch == KEY_RIGHT) {
        int len = (int)Utf8Split(CurBuf.lines[CurBuf.cursorLine]).size();
        if (CurBuf.cursorCol < len) CurBuf.cursorCol++;
        return;
    }
    if (ch == KEY_UP) { MoveCursorUp(CurBuf); return; }
    if (ch == KEY_DOWN) { MoveCursorDown(CurBuf); return; }
    if (ch == KEY_RESIZE || ch == ERR) return;
    if (ch < 32) return; // ignore ปุ่มควบคุมอื่นๆ

    string utf8char = ReadUtf8Char(ch); // รองรับอักขระไทย (multi-byte)
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
    if (ch == KEY_UP) { // ย้อนดูคำสั่งก่อนหน้า เหมือน nvim
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
            // บั๊กเดิม: ถ้ากด 'd'/'y'/'g' ค้างไว้เป็น pending แล้วกดปุ่มอื่นที่ไม่ตรง (เช่น d
            // แล้วตามด้วย j) ปุ่มที่สองจะถูก "กลืน" หายไปเฉยๆ ไม่ทำอะไรเลย (เดิม return ตรงนี้ทันที)
            // ตอนนี้ให้ประมวลผลปุ่มที่สองต่อแบบปกติเหมือนไม่มี pending มาก่อน (เช่น d แล้ว j
            // จะไม่ลบอะไร แต่ cursor จะเลื่อนลงตามที่ควรจะเป็น)
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
        case 'i': CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --"; break;
        case 'a': if (lineLen > 0) CurBuf.cursorCol++; CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --"; break;
        case 'A': CurBuf.cursorCol = lineLen; CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --"; break;
        case 'I': CurBuf.cursorCol = 0; CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --"; break;
        case 'o':
            CurBuf.lines.insert(CurBuf.lines.begin() + CurBuf.cursorLine + 1, "");
            CurBuf.cursorLine++; CurBuf.cursorCol = 0;
            CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --";
            break;
        case 'O':
            CurBuf.lines.insert(CurBuf.lines.begin() + CurBuf.cursorLine, "");
            CurBuf.cursorCol = 0;
            CurMode = EditorMode::INSERT; statusMsg = "-- INSERT --";
            break;
        case 'x': DeleteCharAtCursor(CurBuf); break;
        case 'p':
            if (!CurBuf.clipboard.empty()) {
                CurBuf.lines.insert(CurBuf.lines.begin() + CurBuf.cursorLine + 1, CurBuf.clipboard[0]);
                CurBuf.cursorLine++;
                CurBuf.modified = true;
            }
            break;
        case 'P':
            if (!CurBuf.clipboard.empty()) {
                CurBuf.lines.insert(CurBuf.lines.begin() + CurBuf.cursorLine, CurBuf.clipboard[0]);
                CurBuf.modified = true;
            }
            break;
        case ':': CurMode = EditorMode::COMMAND; CmdLineInput = ":"; break;
        case '\t': CycleFocus(); statusMsg = "Focus: " + FocusName(CurFocus); break;
        default: break;
    }
    ClampCursorCol(CurBuf);
}

void HandleFileManagerKey(int ch, string& statusMsg) {
    if (ch == ERR || ch == KEY_RESIZE) return;
    switch (ch) {
        case 'j': case KEY_DOWN: if (FMSelected < (int)FMEntries.size() - 1) FMSelected++; break;
        case 'k': case KEY_UP:   if (FMSelected > 0) FMSelected--; break;
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
    Term.inputLine += ReadUtf8Char(ch); // เผื่อกรณีพิมพ์ path/ชื่อไฟล์ภาษาไทย
}

void HandleKey(int ch, string& statusMsg) {
    if (ch == ERR) return;       // timeout tick เฉยๆ ไม่มีปุ่มกด
    if (ch == KEY_RESIZE) { HandleResize(); return; }
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
    // สำรองไว้สำหรับงานพื้นหลังของ File Manager ในอนาคต (เช่น watch การเปลี่ยนแปลงไฟล์)
}

void main_thread() {
    SetupTerminal();
    RegisterCommands();
    CurrentDir = StartCwd; // ล็อก File Manager ให้เปิดที่ path ปัจจุบันซึ่ง user รันคำสั่ง thaivim อยู่ (ไม่ใช่ path ของตัวโปรแกรม)
    RefreshFileManager();

    string statusMsg = "Welcome to ThaiVim! Use :help for commands, Tab to switch panels";
    LoadSession();

    RenderAll(statusMsg);

    while (MainRunning) {
        int ch = getch(); // timeout(50) ทำให้ค่านี้เป็น ERR ได้ถ้าไม่มีการกดปุ่มภายใน 50ms
        HandleKey(ch, statusMsg);
        CheckTerminalResizeFallback(); // เผื่อ terminal ไม่ส่ง KEY_RESIZE (เช่นบาง SSH client)
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
    setlocale(LC_ALL, ""); // จำเป็นมากสำหรับให้ ncursesw แสดงผลภาษาไทย (UTF-8) ได้ถูกต้อง

    // เก็บ path ปัจจุบันที่ user อยู่ตอนสั่งรัน thaivim ไว้ก่อน (ต้องทำเป็นอันดับแรกสุด
    // ก่อนโค้ดส่วนอื่นจะไปยุ่งกับ filesystem) เพื่อให้ File Manager ล็อกไปเปิดที่ path นี้
    // แทนที่จะเปิดตามตำแหน่งไฟล์ executable เหมือนเดิม (MotherPath ยังคงไว้ใช้หา
    // syntax/, themes/ และไฟล์ session ที่อยู่คู่กับตัวโปรแกรมเท่านั้น)
    try {
        StartCwd = fs::current_path();
    } catch (...) {
        StartCwd = fs::path("."); // เผื่อ getcwd ล้มเหลว (เช่นโฟลเดอร์ถูกลบระหว่างรันอยู่)
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
                // TODO: รองรับ action อื่นๆ ที่ส่งมาจาก thread อื่นได้ที่นี่
            } else {
                this_thread::sleep_for(chrono::milliseconds(20)); // กัน busy-loop กิน CPU 100%
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