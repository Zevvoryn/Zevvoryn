// GUICON_V1 — реализация собственного терминала. Весь Win32-код живёт только здесь.
// Вне Windows файл компилируется в пустышки, чтобы Linux-сборка не заметила разницы.

#include "gui_console.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <shellapi.h> // GUILINK_V1: открытие ссылок в браузере
#include <windowsx.h> // GUISEL_V1: GET_X_LPARAM / GET_Y_LPARAM

#include <algorithm>
#include <chrono>
#include <condition_variable> // GUIWIZARD_V1
#include <cstring>
#include <functional>         // GUICOLOR_V4: std::hash
#include <queue>              // GUIWIZARD_V1
#include <thread>
#include <unordered_map>      // GUICOLOR_V4: палитра цветов по категориям
#include <utility>            // GUIWRAP_V1: std::pair для границ переносов
#include <vector>

namespace nc::console {
namespace {

struct Line {
    LineLevel   level;
    std::string tag;
    std::string text;
};

// GUIWRAP_V1: одна визуальная строка на экране — часть логической строки лога.
// По этой карте работают и отрисовка, и попадание мышью: выделение и клик по ссылке.
struct VisualRow {
    i32 line   = 0;  // индекс в State::lines
    i32 begin  = 0;  // смещение в wide-строке
    i32 len    = 0;
    i32 indent = 8;  // отступ в пикселях: продолжения выравниваем под текст сообщения
};

struct State {
    std::mutex         mutex;          // защищает lines/status/input/history
    std::deque<Line>   lines;
    std::string        status;
    std::wstring       input;
    std::vector<std::wstring> history;
    i32                historyPos  = -1;
    i32                maxLines    = 20000;
    i32                scrollOffset = 0;   // 0 = прилипли к низу (автопрокрутка)
    bool               russian     = true;

    std::atomic<bool>  running{false};
    std::atomic<bool>  closing{false};   // крестик уже нажат, идёт остановка
    std::atomic<bool>  stopped{false};   // GUICLOSE_V2: сервер уже остановлен, окно доживает
    std::atomic<bool>  repaintQueued{false};

    HWND               hwnd     = nullptr;
    HFONT              font     = nullptr;
    HFONT              logoFont = nullptr; // NOTOFU_V1: крупный векторный шрифт для логотипа ZEVVORYN
    bool               artRun   = false;   // NOTOFU_V1: идёт блок блочного ASCII-арта
    bool               logoSeeded = false; // LOGOSEED_V1: логотип уже стоит в буфере
    i32                lineH   = 16;
    i32                charW   = 8;

    // GUISEL_V1: геометрия последней отрисовки — нужна, чтобы понять, куда ткнули мышью.
    i32                firstVisible = 0;
    i32                logTop       = 0;
    i32                visibleLines = 20;
    std::vector<VisualRow> rows;        // GUIWRAP_V1: геометрия последней отрисовки, сверху вниз
    i32                selAnchor    = -1;  // выделение мышью — построчное
    i32                selCursor    = -1;
    bool               selecting    = false;
    i32                selAnchorCol = 0;   // GUISEL_V2: посимвольное выделение — индекс символа в строке
    i32                selCursorCol = 0;
    bool               selDragged   = false;  // NOAUTOSEL_V1: true только после реального перетаскивания мышью на другую строку—
                                            // обычный одиночный клик больше не подсвечивает всю строку фоном.

    CommandHandler     onCommand;
    CloseHandler       onClose;
    std::thread        uiThread;
    std::thread        closeThread;

    // GUIWIZARD_V1: первый запуск — мастер читает ответы из этой очереди, а не через onCommand.
    std::atomic<bool>       wizardMode{false};
    std::queue<std::string> wizardQueue;
    std::condition_variable wizardCv;
};

State& st() {
    static State s;
    return s;
}

constexpr UINT WM_NC_APPEND = WM_APP + 1;  // пришли новые строки
constexpr UINT WM_NC_QUIT   = WM_APP + 2;  // сервер остановлен, можно гасить окно

// GUICLOSE_V2: сколько окно висит после полной остановки, чтобы успеть прочитать хвост лога.
constexpr int kLingerSeconds = 5;

// GUICOLOR_V3: настоящая палитра Windows Terminal «Campbell» (та же, что в conhost/
// Windows Terminal по умолчанию) — чтобы окно действительно выглядело как настоящий терминал, а
// не приближённой подделкой под него.
COLORREF levelColor(LineLevel lv) {
    switch (lv) {
        case LineLevel::Trace: return RGB(0x76, 0x76, 0x76);  // bright black
        case LineLevel::Debug: return RGB(0x3A, 0x96, 0xDD);  // cyan
        case LineLevel::Info:  return RGB(0x16, 0xC6, 0x0C);  // bright green
        case LineLevel::Warn:  return RGB(0xF9, 0xF1, 0xA5);  // bright yellow
        case LineLevel::Error: return RGB(0xE7, 0x48, 0x56);  // bright red
        case LineLevel::Fatal: return RGB(0xFF, 0x30, 0x30);  // чуть ярче bright red — фатальное должно бить в глаза
        case LineLevel::Input: return RGB(0x61, 0xD6, 0xD6);  // bright cyan
        case LineLevel::Chat:  return RGB(0xF2, 0xF2, 0xF2);  // bright white
    }
    return RGB(0xCC, 0xCC, 0xCC);  // white
}

// GUICOLOR_V4: у INFO-строк тег раньше всегда красился в один и тот же зелёный, из-за чего лог выглядел
// однотонным. Теперь красим тег по имени подсистемы (Server/Net/RCON/...), чтобы разные
// категории отличались с первого взгляда, а не только по критичности (WARN/ERROR и так уже свои).
COLORREF categoryColor(const std::string& tag) {
    static const std::unordered_map<std::string, COLORREF> kPalette = {
        {"Server",   RGB(0x16, 0xC6, 0x0C)},  // ярко-зелёный — как и раньше, самая частая категория
        {"Console",  RGB(0x61, 0xD6, 0xD6)},  // ярко-голубой
        {"Net",      RGB(0x3B, 0x9C, 0xFF)},  // ярко-синий
        {"World",    RGB(0x6A, 0xCF, 0x8E)},  // мятно-зелёный
        {"RCON",     RGB(0xC5, 0x86, 0xF2)},  // сиреневый
        {"Main",     RGB(0xF2, 0xA1, 0x4E)},  // оранжевый
        {"Crypto",   RGB(0x8A, 0xA6, 0xC2)},  // серо-голубой
        {"Lang",     RGB(0xF2, 0x8B, 0xC6)},  // розовый
        {"Ops",      RGB(0xE8, 0xC5, 0x4E)},  // золотой
        {"Session",  RGB(0xFF, 0xB8, 0x4D)},  // SESSIONLINE_V1: статус прошлого сеанса — янтарный
        {"TickProf", RGB(0xB0, 0x8A, 0x5A)},  // коричневатый
    };
    if (auto it = kPalette.find(tag); it != kPalette.end()) return it->second;
    if (tag.empty()) return RGB(0x16, 0xC6, 0x0C);
    // неизвестные теги — детерминированный цвет из небольшой палитры по хэшу,
    // чтобы новые подсистемы тоже выглядели различимо, а не серо.
    static constexpr COLORREF kFallback[] = {
        RGB(0x5A, 0xC8, 0xE0), RGB(0xE0, 0x9A, 0x5A), RGB(0x9A, 0xD6, 0x5A),
        RGB(0xD6, 0x5A, 0x9A), RGB(0x8A, 0x8A, 0xF2), RGB(0xD6, 0xB8, 0x5A),
    };
    const size_t h = std::hash<std::string>{}(tag);
    return kFallback[h % (sizeof(kFallback) / sizeof(kFallback[0]))];
}

constexpr COLORREF kBgColor     = RGB(0x0C, 0x0C, 0x0C);  // фон — чёрный, как в Windows Terminal по умолчанию
constexpr COLORREF kInputBg     = RGB(0x00, 0x00, 0x00);  // полоса ввода — чистый чёрный
constexpr COLORREF kTimeColor   = RGB(0x61, 0xD6, 0xD6);  // [HH:MM:SS.mmm] — bright cyan
constexpr COLORREF kTextColor   = RGB(0xCC, 0xCC, 0xCC);  // само сообщение — терминальный белый
constexpr COLORREF kArtColor    = RGB(0x3F, 0xD9, 0x35);  // BANNERBACK_V1: вернули яркость — приглушённый зелёный выглядел тускло
constexpr COLORREF kArtColorOld = RGB(0x2E, 0x8B, 0x2A);  // EYECOMFORT_V1: баннер приглушили — было слишком ярко-неоново и «резало глаза»
constexpr COLORREF kBoxColor    = RGB(0x61, 0xD6, 0xD6);  // рамка/шапка — точно как \033[36m\033[1m в classic
constexpr COLORREF kLinkColor   = RGB(0x3B, 0x78, 0xFF);  // GUILINK_V1 — bright blue
constexpr COLORREF kSelColor    = RGB(0x26, 0x3B, 0x5A);  // GUISEL_V1

std::wstring toWide(std::string_view s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), len, nullptr, nullptr);
    return s;
}

// Перерисовка по событию, но не чаще одного запроса на очередь сообщений:
// тысяча строк лога за тик даст один WM_NC_APPEND, а не тысячу.
void requestRepaint() {
    auto& s = st();
    if (!s.hwnd) return;
    bool expected = false;
    if (s.repaintQueued.compare_exchange_strong(expected, true))
        PostMessageW(s.hwnd, WM_NC_APPEND, 0, 0);
}

// GUILINK_V1: границы http(s)-ссылок в строке.
struct UrlSpan { size_t begin; size_t end; };

std::vector<UrlSpan> findUrls(const std::wstring& w) {
    std::vector<UrlSpan> out;
    size_t pos = 0;
    while ((pos = w.find(L"http", pos)) != std::wstring::npos) {
        if (w.compare(pos, 7, L"http://") == 0 || w.compare(pos, 8, L"https://") == 0) {
            size_t end = pos;
            while (end < w.size() && w[end] > 32 && w[end] != L'"' && w[end] != L'<' && w[end] != L'>') ++end;
            while (end > pos && (w[end - 1] == L'.' || w[end - 1] == L',' || w[end - 1] == L')')) --end;
            out.push_back(UrlSpan{pos, end});
            pos = end;
        } else {
            ++pos;
        }
    }
    return out;
}

// GUICOLOR_V3: тип строки без штампа времени — определяем по содержимому, а не по цвету:
// цвет kBoxColor теперь совпадает с levelColor(Input), и сравнение по RGB случайно
// центрировало бы однострочный заголовок версии.
enum class LinePurpose { Plain, Art, Box };

LinePurpose linePurpose(const std::wstring& w) {
    if (w.find(L'\u2588') != std::wstring::npos) return LinePurpose::Art;       // █ — ASCII-арт
    if (w.find_first_of(L"\u2550\u2551\u2554\u2557\u255a\u255d") != std::wstring::npos) return LinePurpose::Box;
    if (!w.empty() && w[0] == L'[') return LinePurpose::Box;                     // [Starting server...]
    return LinePurpose::Plain;
}

COLORREF colorForPurpose(LinePurpose p) {
    switch (p) {
        case LinePurpose::Art: return kArtColor;
        case LinePurpose::Box: return kBoxColor;
        default:               return kTextColor;
    }
}

// GUIWRAP_V1 -----------------------------------------------------------------
// Раньше длинная строка просто уезжала за правый край: горизонтальной
// прокрутки у окна нет, так что хвост сообщения был не виден вообще. Теперь
// строка раскладывается на несколько визуальных по фактической ширине окна:
// перелом по границе слова, продолжение с отступом под текст сообщения, время и тег
// не дублируются, цвета и подчёркнутые ссылки сохраняются на каждой части.

// Цветной кусок логической строки. Спаны идут по возрастанию и не пересекаются.
struct ColorSpan { size_t begin; size_t end; COLORREF color; bool link; };

// Раскладка одной логической строки: текст, цвета и геометрия переносов.
struct LineLayout {
    std::wstring           text;
    std::vector<ColorSpan> spans;
    int                    x0     = 8;      // отступ первой визуальной строки
    int                    indent = 8;      // отступ продолжений
    bool                   single = false;  // логотип/центрованный заголовок — не переносим
};

int measureRange(HDC dc, const std::wstring& w, size_t from, size_t len) {
    if (from >= w.size() || len == 0) return 0;
    len = std::min(len, w.size() - from);
    SIZE sz{};
    GetTextExtentPoint32W(dc, w.c_str() + from, static_cast<int>(len), &sz);
    return static_cast<int>(sz.cx);
}

// Сколько символов начиная с from влезает в avail пикселей.
size_t fitChars(HDC dc, const std::wstring& w, size_t from, int avail) {
    if (from >= w.size()) return 0;
    const int count = static_cast<int>(w.size() - from);
    int fit = 0;
    SIZE sz{};
    if (!GetTextExtentExPointW(dc, w.c_str() + from, count, std::max(1, avail), &fit, nullptr, &sz))
        return static_cast<size_t>(count);
    return static_cast<size_t>(std::clamp(fit, 0, count));
}

// Границы визуальных строк: смещение и длина. Перелом ищем по последнему
// пробелу — для русского текста это единственный корректный вариант, а слово
// длиннее строки — длинный путь или хеш — рвём жёстко.
std::vector<std::pair<size_t, size_t>> wrapRanges(HDC dc, const std::wstring& w,
                                                  int availFirst, int availRest) {
    std::vector<std::pair<size_t, size_t>> out;
    size_t pos = 0;
    bool first = true;
    while (pos < w.size() && out.size() < 512) {
        const int avail = std::max(1, first ? availFirst : availRest);
        size_t fit = fitChars(dc, w, pos, avail);
        if (fit == 0) fit = 1;                 // окно уже одного символа — иначе зациклимся
        size_t end = pos + fit;
        if (end < w.size()) {
            size_t brk = 0;
            for (size_t i = end; i > pos; --i) {
                const wchar_t c = w[i - 1];
                if (c == L' ') { brk = i; break; }
                if (i < end && (c == L'-' || c == L'/' || c == L',' || c == L';')) { brk = i; break; }
            }
            if (brk > pos) end = brk;
        }
        out.push_back({pos, end - pos});
        pos = end;
        while (pos < w.size() && w[pos] == L' ') ++pos;  // пробел на переломе в новую строку не тащим
        first = false;
    }
    if (out.empty()) out.push_back({0, 0});
    return out;
}

LineLayout layoutLine(HDC dc, const Line& line, int width) {
    LineLayout out;
    std::wstring w = toWide(line.text);
    const COLORREF lvColor = levelColor(line.level);
    if (line.tag == "@logo") { out.text = std::move(w); out.single = true; return out; }

    const auto urls = findUrls(w);
    auto pushRich = [&](size_t begin, size_t end, COLORREF color) {
        if (end <= begin) return;
        size_t cur = begin;
        for (const auto& u : urls) {
            if (u.end <= begin || u.begin >= end) continue;
            const size_t ub = std::max(u.begin, begin);
            const size_t ue = std::min(u.end, end);
            if (ub > cur) out.spans.push_back(ColorSpan{cur, ub, color, false});
            out.spans.push_back(ColorSpan{ub, ue, kLinkColor, true});
            cur = ue;
        }
        if (cur < end) out.spans.push_back(ColorSpan{cur, end, color, false});
    };

    // Строка вида "[HH:MM:SS.mmm] [Server thread/INFO] сообщение".
    if (w.size() > 2 && w[0] == L'[') {
        const size_t timeEnd = w.find(L']');
        if (timeEnd != std::wstring::npos && timeEnd + 2 < w.size() && w[timeEnd + 2] == L'[') {
            const size_t tagEnd = w.find(L']', timeEnd + 2);
            if (tagEnd != std::wstring::npos) {
                const bool loud = line.level == LineLevel::Warn || line.level == LineLevel::Error ||
                                  line.level == LineLevel::Fatal || line.level == LineLevel::Chat;
                // GUICOLOR_V4: у INFO тег красим по категории, у остальных — по критичности.
                const COLORREF tagColor = (line.level == LineLevel::Info) ? categoryColor(line.tag) : lvColor;
                out.spans.push_back(ColorSpan{0, timeEnd + 1, kTimeColor, false});
                out.spans.push_back(ColorSpan{timeEnd + 1, tagEnd + 1, tagColor, false});
                pushRich(tagEnd + 1, w.size(), loud ? lvColor : kTextColor);
                // продолжение выравниваем под текст, но не съедаем больше трети окна
                out.indent = std::clamp(8 + measureRange(dc, w, 0, tagEnd + 2), 8, std::max(8, width / 3));
                out.text = std::move(w);
                return out;
            }
        }
    }

    // GUICOLOR_V3: баннер зелёный, рамка голубая — ровно как ANSI в classic-консоли.
    const LinePurpose purpose = (line.level == LineLevel::Info) ? linePurpose(w) : LinePurpose::Plain;
    const COLORREF color = (line.level == LineLevel::Info) ? colorForPurpose(purpose) : lvColor;
    if (purpose != LinePurpose::Plain) {
        const size_t b = w.find_first_not_of(L' ');
        if (b == std::wstring::npos) { out.single = true; return out; }
        size_t e = w.size();
        while (e > b && w[e - 1] == L' ') --e;
        std::wstring trimmed = w.substr(b, e - b);
        const int tw = measureRange(dc, trimmed, 0, trimmed.size());
        out.spans.push_back(ColorSpan{0, trimmed.size(), color, false});
        if (tw <= width - 16) {          // влезает — центрируем, как раньше
            out.x0 = std::max(8, (width - tw) / 2);
            out.single = true;
        }                                // не влезает — переносим по словам от левого края
        out.text = std::move(trimmed);
        return out;
    }

    pushRich(0, w.size(), color);
    out.indent = std::clamp(8 + st().charW * 2, 8, std::max(8, width / 3));  // висячий отступ
    out.text = std::move(w);
    return out;
}

// Одна визуальная строка: символы от rowBegin до rowEnd с цветами и подчёркиванием ссылок.
void drawRow(HDC dc, int x, int y, const LineLayout& layout, size_t rowBegin, size_t rowEnd, int lineH) {
    for (const auto& sp : layout.spans) {
        const size_t b = std::max(sp.begin, rowBegin);
        const size_t e = std::min(sp.end, rowEnd);
        if (e <= b) continue;
        const std::wstring part = layout.text.substr(b, e - b);
        SetTextColor(dc, sp.color);
        TextOutW(dc, x, y, part.c_str(), static_cast<int>(part.size()));
        SIZE sz{};
        GetTextExtentPoint32W(dc, part.c_str(), static_cast<int>(part.size()), &sz);
        if (sp.link) {  // GUILINK_V1
            RECT underline{x, y + lineH - 2, x + static_cast<int>(sz.cx), y + lineH - 1};
            if (HBRUSH ub = CreateSolidBrush(kLinkColor)) { FillRect(dc, &underline, ub); DeleteObject(ub); }
        }
        x += static_cast<int>(sz.cx);
    }
}

// GUISEL_V2: копирование выделенного — теперь посимвольно, а не целыми строками.
void copySelection(HWND hwnd) {
    auto& s = st();
    std::wstring buf;
    {
        std::lock_guard lock(s.mutex);
        if (s.selAnchor < 0 || s.selCursor < 0 || !s.selDragged || s.lines.empty()) return; // NOAUTOSEL_V1: копировать только реально выделенное
        const int total = static_cast<int>(s.lines.size());
        int aLine = std::clamp(s.selAnchor, 0, total - 1);
        int bLine = std::clamp(s.selCursor, 0, total - 1);
        int aCol  = s.selAnchorCol;
        int bCol  = s.selCursorCol;
        if (bLine < aLine || (aLine == bLine && bCol < aCol)) { std::swap(aLine, bLine); std::swap(aCol, bCol); }
        for (int i = aLine; i <= bLine; ++i) {
            const std::wstring w = toWide(s.lines[static_cast<size_t>(i)].text);
            const int len  = static_cast<int>(w.size());
            const int from = (i == aLine) ? std::clamp(aCol, 0, len) : 0;
            const int to   = (i == bLine) ? std::clamp(bCol, 0, len) : len;
            if (to > from) buf += w.substr(static_cast<size_t>(from), static_cast<size_t>(to - from));
            if (i != bLine) buf += L"\r\n";
        }
    }
    if (buf.empty() || !OpenClipboard(hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (buf.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(mem)) {
            std::memcpy(p, buf.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// GUIPASTE_V1: правая кнопка / Ctrl+V / Shift+Insert — вставка текста из буфера в строку ввода
// (как в обычной консоли Windows). Переводы строк схлопываем в пробелы — ввод однострочный.
void pasteClipboardToInput(HWND hwnd) {
    auto& s = st();
    if (s.closing.load()) return;
    std::wstring text;
    if (!OpenClipboard(hwnd)) return;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            text = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    if (text.empty()) return;
    for (auto& ch : text) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    {
        std::lock_guard lock(s.mutex);
        s.input += text;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

// GUIDPI_V1: шрифт пересоздаётся под текущий DPI — объявление нужно раньше wndProc (WM_DPICHANGED).
void createFontForDpi(HWND hwnd, unsigned dpi);

// GUIWRAP_V1: попадание мышью считаем по визуальным строкам последней отрисовки:
// одна логическая строка теперь может занимать несколько рядов на экране.
struct HitPos { int line = -1; int col = 0; };

// false, если ткнули мимо лога. Текст строки отдаём копией: дальше замеры текста
// идут без удержания мьютекса.
bool rowUnderCursor(int py, VisualRow& rowOut, std::wstring& textOut) {
    auto& s = st();
    std::lock_guard lock(s.mutex);
    if (s.lineH <= 0 || s.rows.empty() || s.lines.empty()) return false;
    int r = (py < s.logTop) ? 0 : (py - s.logTop) / s.lineH;
    r = std::clamp(r, 0, static_cast<int>(s.rows.size()) - 1);
    const VisualRow row = s.rows[static_cast<size_t>(r)];
    if (row.line < 0 || row.line >= static_cast<int>(s.lines.size())) return false;
    rowOut  = row;
    textOut = toWide(s.lines[static_cast<size_t>(row.line)].text);
    return true;
}

// GUISEL_V2: строка и символ под курсором — выделение идёт по буквам.
HitPos hitTest(HWND hwnd, int px, int py) {
    auto& s = st();
    VisualRow row{};
    std::wstring w;
    if (!rowUnderCursor(py, row, w)) return {};
    HitPos hit;
    hit.line = row.line;
    const size_t begin = std::min(static_cast<size_t>(std::max(0, row.begin)), w.size());
    const size_t len   = std::min(static_cast<size_t>(std::max(0, row.len)), w.size() - begin);
    hit.col = static_cast<int>(begin);
    if (len == 0) return hit;
    HDC dc = GetDC(hwnd);
    if (!dc) return hit;
    HGDIOBJ oldFont = SelectObject(dc, s.font);
    const int avail = std::max(0, px - row.indent);
    size_t fit = std::min(fitChars(dc, w, begin, avail), len);
    if (fit < len) {  // округляем к ближайшей границе символа
        const int a = measureRange(dc, w, begin, fit);
        const int b = measureRange(dc, w, begin, fit + 1);
        if (b > a && (avail - a) > (b - a) / 2) ++fit;
    }
    SelectObject(dc, oldFont);
    ReleaseDC(hwnd, dc);
    hit.col = static_cast<int>(begin + fit);
    return hit;
}

// GUILINK_V1: клик по ссылке с учётом того, что строка могла перенестись.
bool tryOpenLinkAt(HWND hwnd, int px, int py) {
    auto& s = st();
    VisualRow row{};
    std::wstring w;
    if (!rowUnderCursor(py, row, w)) return false;
    const auto urls = findUrls(w);
    if (urls.empty()) return false;
    const size_t begin = std::min(static_cast<size_t>(std::max(0, row.begin)), w.size());
    const size_t end   = std::min(begin + static_cast<size_t>(std::max(0, row.len)), w.size());
    HDC dc = GetDC(hwnd);
    if (!dc) return false;
    HGDIOBJ oldFont = SelectObject(dc, s.font);
    bool opened = false;
    for (const auto& u : urls) {
        const size_t ub = std::max(u.begin, begin);
        const size_t ue = std::min(u.end, end);
        if (ue <= ub) continue;  // эта часть ссылки на другом ряду
        const int x0 = row.indent + measureRange(dc, w, begin, ub - begin);
        const int x1 = x0 + measureRange(dc, w, ub, ue - ub);
        if (px >= x0 && px <= x1) {
            const std::wstring url = w.substr(u.begin, u.end - u.begin);
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            opened = true;
            break;
        }
    }
    SelectObject(dc, oldFont);
    ReleaseDC(hwnd, dc);
    return opened;
}

// LOGORASTER_V1: логотип рисуем сами прямоугольниками 5x7, а не глифами шрифта —
// у моноширинных шрифтов между клетками остаются щели, и надпись рассыпается.
static const char* const kLogoGlyphs[8][7] = {
    {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}, // Z
    {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}, // E
    {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}, // V
    {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}, // V
    {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}, // O
    {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}, // R
    {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}, // Y
    {"#...#", "##..#", "#.#.#", "#.#.#", "#..##", "#...#", "#...#"}, // N
};

int logoRasterWidth(int px) { return 8 * 6 * px - px; } // 8 букв по 5 клеток + пробел, последний не нужен

void drawLogoRaster(HDC dc, int x, int y, int px) {
    HBRUSH brush = CreateSolidBrush(kArtColor);
    if (!brush) return;
    for (int g = 0; g < 8; ++g) {
        const int gx = x + g * 6 * px;
        for (int row = 0; row < 7; ++row) {
            const char* line = kLogoGlyphs[g][row];
            for (int col = 0; col < 5; ++col) {
                if (line[col] != '#') continue;
                RECT cell{gx + col * px, y + row * px, gx + (col + 1) * px, y + (row + 1) * px};
                FillRect(dc, &cell, brush);
            }
        }
    }
    DeleteObject(brush);
}

void paint(HWND hwnd) {
    auto& s = st();
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT rc;
    GetClientRect(hwnd, &rc);
    const int width  = rc.right - rc.left;
    const int height = rc.bottom - rc.top;

    // Двойная буферизация: без неё лог мерцает на каждом обновлении.
    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, width, height);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    HBRUSH bg = CreateSolidBrush(kBgColor); // GUICOLOR_V2
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    HGDIOBJ oldFont = SelectObject(mem, s.font);
    SetBkMode(mem, TRANSPARENT);

    const int inputH  = s.lineH + 8;
    const int logH    = height - inputH;
    const int visible = std::max(1, logH / s.lineH);

    // GUIWRAP_V1: логические строки раскладываем на визуальные по ширине окна.
    std::vector<Line> pool;
    std::string status;
    std::wstring input;
    int total = 0;
    int offset = 0;
    int poolFirst = 0;
    int selA = -1, selB = -1;
    int selACol = 0, selBCol = 0; // GUISEL_V2
    {
        std::lock_guard lock(s.mutex);
        total  = static_cast<int>(s.lines.size());
        // GUISCROLL_V2: выше самой первой строки уехать нельзя.
        s.scrollOffset = std::clamp(s.scrollOffset, 0, std::max(0, total - visible));
        offset = s.scrollOffset;
        status = s.status;
        input  = s.input;
        // Любая логическая строка занимает минимум один ряд, поэтому хвоста
        // в visible строк заведомо достаточно, чтобы заполнить экран.
        const int last = std::max(0, total - offset);
        poolFirst = std::max(0, last - visible);
        pool.assign(s.lines.begin() + poolFirst, s.lines.begin() + last);
        if (s.selAnchor >= 0 && s.selCursor >= 0 && s.selDragged) { // NOAUTOSEL_V1
            selA = s.selAnchor; selACol = s.selAnchorCol;
            selB = s.selCursor; selBCol = s.selCursorCol;
            if (selB < selA || (selA == selB && selBCol < selACol)) { std::swap(selA, selB); std::swap(selACol, selBCol); }
        }
    }

    std::vector<LineLayout> layouts;
    layouts.reserve(pool.size());
    std::vector<VisualRow> rows;
    for (size_t i = 0; i < pool.size(); ++i) {
        layouts.push_back(layoutLine(mem, pool[i], width));
        const LineLayout& layout = layouts.back();
        const i32 lineIdx = static_cast<i32>(poolFirst + static_cast<int>(i));
        if (layout.single || layout.text.empty()) {
            rows.push_back(VisualRow{lineIdx, 0, static_cast<i32>(layout.text.size()), layout.x0});
            continue;
        }
        const auto ranges = wrapRanges(mem, layout.text,
                                       std::max(1, width - layout.x0 - 8),
                                       std::max(1, width - layout.indent - 8));
        for (size_t r = 0; r < ranges.size(); ++r) {
            rows.push_back(VisualRow{lineIdx, static_cast<i32>(ranges[r].first), static_cast<i32>(ranges[r].second),
                                     (r == 0) ? layout.x0 : layout.indent});
        }
    }
    // Лог прижат к низу: если переносы дали больше рядов, чем влезает, срезаем сверху.
    if (static_cast<int>(rows.size()) > visible)
        rows.erase(rows.begin(), rows.begin() + (static_cast<int>(rows.size()) - visible));

    const int logTop = logH - static_cast<int>(rows.size()) * s.lineH;
    {
        std::lock_guard lock(s.mutex);
        s.rows         = rows;  // GUISEL_V1 + GUIWRAP_V1: геометрия для попадания мышью
        s.firstVisible = rows.empty() ? 0 : rows.front().line;
        s.logTop       = logTop;
        s.visibleLines = visible;
    }

    int y = logTop;
    for (const auto& row : rows) {
        const size_t li = static_cast<size_t>(row.line - poolFirst);
        if (li >= layouts.size()) { y += s.lineH; continue; }
        const LineLayout& layout = layouts[li];
        const size_t rowBegin = std::min(static_cast<size_t>(std::max(0, row.begin)), layout.text.size());
        const size_t rowEnd   = std::min(rowBegin + static_cast<size_t>(std::max(0, row.len)), layout.text.size());
        if (selA >= 0 && row.line >= selA && row.line <= selB) { // GUISEL_V2: подсветка только выделенных символов
            const size_t textLen = layout.text.size();
            const size_t selFrom = (row.line == selA) ? std::min(static_cast<size_t>(std::max(0, selACol)), textLen) : 0;
            const size_t selTo   = (row.line == selB) ? std::min(static_cast<size_t>(std::max(0, selBCol)), textLen) : textLen;
            const size_t hb = std::max(selFrom, rowBegin);
            const size_t he = std::min(selTo, rowEnd);
            if (he > hb) {
                const int sx0 = row.indent + measureRange(mem, layout.text, rowBegin, hb - rowBegin);
                const int sx1 = sx0 + measureRange(mem, layout.text, hb, he - hb);
                RECT sel{sx0, y, sx1, y + s.lineH};
                if (HBRUSH selBg = CreateSolidBrush(kSelColor)) { FillRect(mem, &sel, selBg); DeleteObject(selBg); }
            }
        }
        if (pool[li].tag == "@logo") { // NOTOFU_V1: крупный чёткий логотип вместо блочного арта
            if (!pool[li].text.empty()) { // LOGOCENTER_V1: растровый логотип по центру окна
                const int px = std::max(2, (s.lineH * 3) / 9);
                const int lw = logoRasterWidth(px);
                drawLogoRaster(mem, std::max(8, (width - lw) / 2), y + 2, px);
            }
            y += s.lineH;
            continue;
        }
        drawRow(mem, row.indent, y, layout, rowBegin, rowEnd, s.lineH); // GUICOLOR_V2 + GUIWRAP_V1
        y += s.lineH;
    }

    // Полоса ввода
    RECT inputRect{0, logH, width, height};
    HBRUSH inputBg = CreateSolidBrush(kInputBg); // GUICOLOR_V2
    FillRect(mem, &inputRect, inputBg);
    DeleteObject(inputBg);
    SetTextColor(mem, levelColor(LineLevel::Input));
    // GUIWRAP_V1: строка ввода однострочная, поэтому длинную команду прокручиваем
    // по содержимому: виден хвост с курсором, а не начало, уехавшее за край.
    std::wstring prompt = L"> " + input + (s.closing.load() ? L"" : L"_");
    const int promptAvail = std::max(1, width - 16);
    if (measureRange(mem, prompt, 0, prompt.size()) > promptAvail) {
        std::wstring tail = input + (s.closing.load() ? L"" : L"_");
        const int reserve = std::max(1, promptAvail - 4 * std::max(1, s.charW));
        while (tail.size() > 1 && measureRange(mem, tail, 0, tail.size()) > reserve) tail.erase(0, 1);
        prompt = L"> \u2026" + tail;
    }
    TextOutW(mem, 8, logH + 4, prompt.c_str(), static_cast<int>(prompt.size()));

    // Плашка «Сохранение мира…» поверх всего
    if (!status.empty()) {
        RECT band{0, height / 2 - s.lineH, width, height / 2 + s.lineH};
        HBRUSH bandBg = CreateSolidBrush(RGB(52, 42, 20)); // GUICOLOR_V2
        FillRect(mem, &band, bandBg);
        DeleteObject(bandBg);
        SetTextColor(mem, RGB(232, 198, 130));
        const std::wstring w = toWide(status);
        TextOutW(mem, 16, height / 2 - s.lineH / 2, w.c_str(), static_cast<int>(w.size()));
    }

    BitBlt(hdc, 0, 0, width, height, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void submitInput() {
    auto& s = st();
    std::string command;
    const bool wizard = s.wizardMode.load();
    {
        std::lock_guard lock(s.mutex);
        // GUIWIZARD_V1: мастер должен принимать пустую строку (значение по умолчанию).
        if (s.input.empty() && !wizard) return;
        command = toUtf8(s.input);
        s.history.push_back(s.input);
        if (s.history.size() > 200) s.history.erase(s.history.begin());
        s.historyPos = -1;
        s.input.clear();
        s.lines.push_back(Line{LineLevel::Input, "", "> " + command});
        while (static_cast<i32>(s.lines.size()) > s.maxLines) s.lines.pop_front();
        s.scrollOffset = 0;
        if (wizard) s.wizardQueue.push(command);
    }
    if (wizard) s.wizardCv.notify_one();
    else if (s.onCommand) s.onCommand(command); // CMDLOG_V2: логгирует processConsoleCommands
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void stepHistory(int delta) {
    auto& s = st();
    std::lock_guard lock(s.mutex);
    if (s.history.empty()) return;
    if (s.historyPos < 0) s.historyPos = static_cast<i32>(s.history.size());
    s.historyPos = std::clamp(s.historyPos + delta, 0, static_cast<i32>(s.history.size()));
    s.input = (s.historyPos >= static_cast<i32>(s.history.size())) ? std::wstring{}
                                                                  : s.history[static_cast<size_t>(s.historyPos)];
}

// Крестик. Окно наше, поэтому никаких пяти секунд: сообщаем пользователю, что
// идёт сохранение, и гасим окно только когда сервер честно остановлен.
void handleCloseRequest() {
    auto& s = st();
    bool expected = false;
    if (!s.closing.compare_exchange_strong(expected, true)) {
        // GUICLOSE_V2: повторный клик. Пока идёт сейв — игнор; когда всё сохранено — гасим сразу.
        if (s.stopped.load() && s.hwnd) PostMessageW(s.hwnd, WM_NC_QUIT, 0, 0);
        return;
    }
    setStatus(s.russian ? "Остановка: кик игроков → сохранение мира. Не закрывайте принудительно…"
                        : "Stopping: kicking players \u2192 saving the world. Please do not force-close\u2026");
    if (s.closeThread.joinable()) return;
    s.closeThread = std::thread([&s] {
        if (s.onClose) s.onClose();      // полный stop("closed"): кик → сейв → сеть
        s.stopped.store(true);
        // GUICLOSE_V2: окно не исчезает мгновенно — видно, что мир сохранён.
        for (int left = kLingerSeconds; left > 0 && s.running.load(); --left) {
            setStatus(s.russian
                          ? "Сервер остановлен, мир сохранён. Окно закроется через " + std::to_string(left) +
                                " с (Esc или крестик — сразу)"
                          : "Server stopped, world saved. Closing in " + std::to_string(left) +
                                "s (Esc or the X button to close now)");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (s.hwnd) PostMessageW(s.hwnd, WM_NC_QUIT, 0, 0);
    });
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto& s = st();
    switch (msg) {
        case WM_NC_APPEND:
            s.repaintQueued.store(false);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_NC_QUIT:
            DestroyWindow(hwnd);
            return 0;
        case WM_PAINT:
            paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1; // фон рисуем сами в paint(), иначе мерцание
        case WM_DPICHANGED: { // GUIDPI_V1: перетащили окно на другой монитор — пересобираем шрифт под новый DPI
            createFontForDpi(hwnd, static_cast<unsigned>(HIWORD(wParam)));
            if (const RECT* r = reinterpret_cast<const RECT*>(lParam)) {
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            std::lock_guard lock(s.mutex);
            // GUISCROLL_V2: верхний предел — первая строка лога, а не пустота.
            const int maxOffset = std::max(0, static_cast<int>(s.lines.size()) - s.visibleLines);
            s.scrollOffset = std::clamp(s.scrollOffset + notches * 3, 0, maxOffset);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: { // GUISEL_V1: начало выделения
            SetFocus(hwnd);
            const HitPos hit = hitTest(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); // GUIWRAP_V1
            const int idx = hit.line;
            const int col = hit.col; // GUISEL_V2
            {
                std::lock_guard lock(s.mutex);
                s.selAnchor = idx;
                s.selCursor = idx;
                s.selAnchorCol = col;
                s.selCursorCol = col;
                s.selecting = idx >= 0;
                s.selDragged = false; // NOAUTOSEL_V1: сбрасываем старое выделение на каждом новом клике
            }
            if (idx >= 0) SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!s.selecting) return 0;
            const HitPos hit = hitTest(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)); // GUIWRAP_V1
            const int idx = hit.line;
            if (idx >= 0) {
                const int col = hit.col; // GUISEL_V2 (без удержания мьютекса)
                std::lock_guard lock(s.mutex);
                if (idx != s.selAnchor || col != s.selAnchorCol) s.selDragged = true; // NOAUTOSEL_V1: подсветка только при реальном перетаскивании
                s.selCursor = idx;
                s.selCursorCol = col;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const bool wasSelecting = s.selecting;
            s.selecting = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            const int idx = hitTest(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)).line; // GUIWRAP_V1
            bool singleClick = false;
            {
                std::lock_guard lock(s.mutex);
                singleClick = (s.selAnchor >= 0 && s.selAnchor == s.selCursor && !s.selDragged); // GUISEL_V2
            }
            if (wasSelecting && singleClick && idx >= 0) {
                if (tryOpenLinkAt(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) { // GUILINK_V1 + GUIWRAP_V1
                    std::lock_guard lock(s.mutex);
                    s.selAnchor = s.selCursor = -1;
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_RBUTTONUP: { // GUIPASTE_V1: правая кнопка вставляет текст из буфера (как в cmd), а не копирует
            pasteClipboardToInput(hwnd);
            return 0;
        }
        case WM_CHAR: {
            if (s.closing.load()) return 0;
            const wchar_t ch = static_cast<wchar_t>(wParam);
            if (ch == L'\r') { submitInput(); return 0; }
            if (ch == L'\b') {
                std::lock_guard lock(s.mutex);
                if (!s.input.empty()) s.input.pop_back();
            } else if (ch >= 32) {
                std::lock_guard lock(s.mutex);
                s.input.push_back(ch);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN: {
            const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if ((ctrl && wParam == 'V') || (shift && wParam == VK_INSERT)) { pasteClipboardToInput(hwnd); return 0; } // GUIPASTE_V1
            if (ctrl && (wParam == 'C' || wParam == VK_INSERT)) { copySelection(hwnd); return 0; } // GUISEL_V1
            if (ctrl && wParam == 'A') {
                std::lock_guard lock(s.mutex);
                s.selAnchor = 0;
                s.selAnchorCol = 0;                 // GUISEL_V2
                s.selDragged = true;
                s.selCursor = std::max(0, static_cast<int>(s.lines.size()) - 1);
                s.selCursorCol = s.lines.empty() ? 0 : static_cast<i32>(toWide(s.lines.back().text).size());
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_PRIOR || wParam == VK_NEXT || wParam == VK_HOME) { // GUISCROLL_V2
                std::lock_guard lock(s.mutex);
                const int maxOffset = std::max(0, static_cast<int>(s.lines.size()) - s.visibleLines);
                if (wParam == VK_HOME) s.scrollOffset = maxOffset;
                else s.scrollOffset = std::clamp(s.scrollOffset + (wParam == VK_PRIOR ? s.visibleLines : -s.visibleLines),
                                                 0, maxOffset);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_UP)        stepHistory(-1);
            else if (wParam == VK_DOWN) stepHistory(+1);
            else if (wParam == VK_END) { std::lock_guard lock(s.mutex); s.scrollOffset = 0; }
            else if (wParam == VK_ESCAPE) { // GUICLOSE_V2: закрыть сразу, но только после сейва
                if (s.stopped.load()) { PostMessageW(hwnd, WM_NC_QUIT, 0, 0); return 0; }
                std::lock_guard lock(s.mutex); // иначе Esc просто снимает выделение
                s.selAnchor = s.selCursor = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            else return DefWindowProcW(hwnd, msg, wParam, lParam);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_CLOSE: {
            // CLOSEWARN_V1: крестик больше не гасит сервер молча — честно предупреждаем,
            // что Windows даёт процессу всего ~5 секунд и сейв будет аварийным: последние
            // изменения мира и инвентари могут потеряться. Надёжный путь — команда stop.
            if (!s.closing.load() && !s.stopped.load() && !s.wizardMode.load()) {
                const wchar_t* text = s.russian
                    ? L"Закрыть сервер крестиком?\r\n\r\n"
                      L"Windows даёт процессу всего около 5 секунд, поэтому мир будет сохранён АВАРИЙНО.\r\n"
                      L"Часть последних изменений (блоки, инвентари, прогресс игроков) может пропасть.\r\n\r\n"
                      L"Надёжно: нажмите «Нет» и введите команду stop — тогда сохранение пройдёт полностью."
                    : L"Close the server with the X button?\r\n\r\n"
                      L"Windows gives the process only about 5 seconds, so the world will be EMERGENCY-saved.\r\n"
                      L"Some of the most recent changes (blocks, inventories, player progress) may be lost.\r\n\r\n"
                      L"Safe way: click \"No\" and type the stop command \u2014 then the save completes in full.";
                const wchar_t* caption = s.russian ? L"Zevvoryn \u2014 аварийное завершение"
                                                   : L"Zevvoryn \u2014 emergency shutdown";
                const int answer = MessageBoxW(hwnd, text, caption,
                                               MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST);
                if (answer != IDYES) return 0; // передумали — сервер продолжает работать
            }
            handleCloseRequest();
            return 0; // окно НЕ закрываем сразу — ждём конца сейва
        }
        case WM_DESTROY:
            s.running.store(false);
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// GUICOLOR_V3: отдельная именованная функция вместо лямбды: EnumFontFamiliesExW ждёт CALLBACK
// (__stdcall), а неявная конверсия беззахватной лямбды в такой указатель — MS-расширение,
// которое /permissive- может отключить.
int CALLBACK fontExistsEnumProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lParam) {
    *reinterpret_cast<bool*>(lParam) = true;
    return 0; // нашли, хватит одного совпадения
}

// GUICOLOR_V3: проверяем, установлен ли шрифт с таким именем, чтобы не получить тихую
// подмену GDI на что-то непохожее.
bool fontExists(const std::wstring& name) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, name.c_str(), LF_FACESIZE - 1);
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, fontExistsEnumProc, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

// GUICOLOR_V3: тот же шрифт, что и в Windows Terminal по умолчанию — Cascadia Mono со всеми
// лигатурами, затем Cascadia Code, иначе Consolas (есть всегда, новее узкая).
std::wstring pickFontFace() {
    if (fontExists(L"Cascadia Mono")) return L"Cascadia Mono";
    if (fontExists(L"Cascadia Code")) return L"Cascadia Code";
    return L"Consolas";
}

// GUIDPI_V1: главная причина «мыла» — процесс не был DPI-aware, и Windows тупо растягивала
// готовую картинку окна на дисплее со шкалой 125/150/200% — отсюда вид «480p вместо Full HD».
// Объявляемся per-monitor aware и рисуем в реальных пикселях.
void enableDpiAwareness() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    if (auto setCtx = reinterpret_cast<SetCtxFn>(reinterpret_cast<void*>(GetProcAddress(u32, "SetProcessDpiAwarenessContext")))) {
        if (setCtx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)))) return; // PER_MONITOR_AWARE_V2
        if (setCtx(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-3)))) return; // PER_MONITOR_AWARE
    }
    using SetAwareFn = BOOL(WINAPI*)();
    if (auto setAware = reinterpret_cast<SetAwareFn>(reinterpret_cast<void*>(GetProcAddress(u32, "SetProcessDPIAware")))) setAware();
}

unsigned dpiForWindow(HWND hwnd) {
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        using GetDpiFn = UINT(WINAPI*)(HWND);
        if (auto getDpi = reinterpret_cast<GetDpiFn>(reinterpret_cast<void*>(GetProcAddress(u32, "GetDpiForWindow")))) {
            const UINT d = getDpi(hwnd);
            if (d >= 72) return d;
        }
    }
    if (HDC dc = GetDC(nullptr)) {
        const int d = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        if (d >= 72) return static_cast<unsigned>(d);
    }
    return 96;
}

// GUIDPI_V1: шрифт считаем от DPI, а не фиксированные 18px — иначе на шкалированном экране текст мелкий/замыленный.
void createFontForDpi(HWND hwnd, unsigned dpi) {
    auto& s = st();
    if (dpi < 72) dpi = 96;
    HFONT font = CreateFontW(-MulDiv(14, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_NATURAL_QUALITY, FIXED_PITCH | FF_MODERN, pickFontFace().c_str());
    if (!font) return;
    { // NOTOFU_V1: логотип рисуем крупным жирным шрифтом (высотой в 3 строки) — без блочных символов
        HFONT big = CreateFontW(-MulDiv(34, static_cast<int>(dpi), 96), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_NATURAL_QUALITY, FIXED_PITCH | FF_MODERN, pickFontFace().c_str());
        if (big) {
            HFONT oldBig = s.logoFont;
            s.logoFont = big;
            if (oldBig) DeleteObject(oldBig);
        }
    }
    HFONT old = s.font;
    s.font = font;
    if (HDC hdc = GetDC(hwnd)) {
        HGDIOBJ prev = SelectObject(hdc, s.font);
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        s.lineH = static_cast<i32>(tm.tmHeight + tm.tmExternalLeading);
        s.charW = static_cast<i32>(tm.tmAveCharWidth);
        SelectObject(hdc, prev);
        ReleaseDC(hwnd, hdc);
    }
    if (old) DeleteObject(old);
}

void uiThreadMain(GuiOptions options) {
    auto& s = st();

    enableDpiAwareness(); // GUIDPI_V1: до создания окна

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ZevvorynConsole";
    if (!RegisterClassExW(&wc)) { s.running.store(false); return; }

    const std::wstring title = toWide(options.title);
    s.hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(),
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             1000, 620, nullptr, nullptr, wc.hInstance, nullptr);
    if (!s.hwnd) { s.running.store(false); return; }

    // GUICOLOR_V3 + GUIDPI_V1: тот же шрифт, что и в Windows Terminal (Cascadia Mono/Code, откат на Consolas),
    // но размер теперь считается от DPI монитора — текст чёткий, а не растянутый битмап.
    const unsigned dpi = dpiForWindow(s.hwnd);
    createFontForDpi(s.hwnd, dpi);
    if (dpi != 96) { // окно тоже масштабируем, иначе на 150% оно выйдет крошечным
        SetWindowPos(s.hwnd, nullptr, 0, 0,
                     MulDiv(1000, static_cast<int>(dpi), 96), MulDiv(620, static_cast<int>(dpi), 96),
                     SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
    }

    ShowWindow(s.hwnd, SW_SHOW);
    UpdateWindow(s.hwnd);
    s.running.store(true);

    // Цикл сообщений блокирующий: в простое поток спит, CPU не тратится.
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (s.font) { DeleteObject(s.font); s.font = nullptr; }
    s.hwnd = nullptr;
    s.running.store(false);
}

} // namespace

bool isAvailable() { return true; }
bool isRunning()   { return st().running.load(); }

bool start(const GuiOptions& options, CommandHandler onCommand, CloseHandler onClose) {
    auto& s = st();
    if (s.running.load()) return true;
    s.onCommand = std::move(onCommand);
    s.onClose   = std::move(onClose);
    s.maxLines  = std::max(1000, options.maxLines);
    s.russian   = options.russian;
    s.uiThread  = std::thread(uiThreadMain, options);

    // Ждём появления окна, чтобы честно вернуть false и откатиться к classic.
    for (int i = 0; i < 200 && !s.running.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!s.running.load()) {
        if (s.uiThread.joinable()) s.uiThread.join();
        return false;
    }
    // LOGOSEED_V1: раньше логотип появлялся только если детектор ловил ASCII-арт баннера.
    // Баннер печатается в startWithConfig, а окно может подняться позже — тогда pushLine
    // уходит в early-return по !running и надпись теряется навсегда. Ставим сами.
    {
        std::lock_guard lock(s.mutex);
        if (!s.logoSeeded) {
            s.logoSeeded = true;
            s.lines.push_front(Line{LineLevel::Info, "@logo", ""});
            s.lines.push_front(Line{LineLevel::Info, "@logo", ""});
            s.lines.push_front(Line{LineLevel::Info, "@logo", "ZEVVORYN"});
        }
    }
    requestRepaint();
    return true;
}

void pushLine(LineLevel level, std::string_view tag, std::string text) {
    auto& s = st();
    if (!s.running.load()) return;
    // NOTOFU_V1: блочный ASCII-логотип (█ и рамки) в окне рассыпается на квадратики:
    // между глифами моноширинного шрифта есть щели, и буквы теряют форму.
    // В файле лога и в классической консоли арт остаётся как был, а в окне та же надпись ZEVVORYN
    // рисуется одним крупным векторным текстом — чётко на любом DPI.
    // ARTTAIL_V1: нижняя строка арта состоит только из рамочных символов (╚══╝), символа █ в ней
    // нет, поэтому раньше она проскакивала мимо замены и висела под логотипом огрызками скобок.
    // Рамку «Minecraft Java Edition» при этом не трогаем — её узнаём по верхнему углу ╔.
    const bool hasBlock = text.find("\xE2\x96\x88") != std::string::npos;
    const bool boxTop   = text.find("\xE2\x95\x94") != std::string::npos;
    bool onlyBoxChars = !boxTop;
    bool sawBoxChar   = false;
    for (size_t i = 0; onlyBoxChars && i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++i; continue; }
        if (c == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x95) {
            sawBoxChar = true;
            i += 3;
            continue;
        }
        onlyBoxChars = false;
    }
    onlyBoxChars = onlyBoxChars && sawBoxChar;
    {
        std::lock_guard lock(s.mutex);
        const bool isArt = hasBlock || (s.artRun && onlyBoxChars);
        if (isArt) {
            if (!s.artRun) {
                s.artRun = true;
                // LOGOSEED_V1: логотип уже поставлен при старте окна — второй раз не дублируем,
                // но сам арт по-прежнему глотаем, чтобы не сыпался квадратиками.
                if (!s.logoSeeded) {
                    s.logoSeeded = true;
                    s.lines.push_back(Line{LineLevel::Info, "@logo", "ZEVVORYN"});
                    s.lines.push_back(Line{LineLevel::Info, "@logo", ""});
                    s.lines.push_back(Line{LineLevel::Info, "@logo", ""});
                    while (static_cast<i32>(s.lines.size()) > s.maxLines) s.lines.pop_front();
                }
            }
            requestRepaint();
            return;
        }
        s.artRun = false;
        s.lines.push_back(Line{level, std::string(tag), std::move(text)});
        while (static_cast<i32>(s.lines.size()) > s.maxLines) s.lines.pop_front();
        // Если пользователь отмотал вверх — удерживаем его место, а не дёргаем вниз.
        if (s.scrollOffset > 0 && s.scrollOffset < s.maxLines) ++s.scrollOffset;
    }
    requestRepaint();
}

void setStatus(std::string text) {
    auto& s = st();
    {
        std::lock_guard lock(s.mutex);
        s.status = std::move(text);
    }
    requestRepaint();
}

// GUIWIZARD_V1
void enableWizardInput(bool on) {
    auto& s = st();
    s.wizardMode.store(on);
    if (!on) {
        std::lock_guard lock(s.mutex);
        std::queue<std::string> empty;
        std::swap(s.wizardQueue, empty);
    }
}

std::string readWizardLine() {
    auto& s = st();
    std::unique_lock lock(s.mutex);
    s.wizardCv.wait(lock, [&s] { return !s.wizardQueue.empty() || !s.running.load(); });
    if (s.wizardQueue.empty()) return {};
    std::string v = std::move(s.wizardQueue.front());
    s.wizardQueue.pop();
    return v;
}

void setTitle(const std::string& title) {
    auto& s = st();
    if (s.hwnd) SetWindowTextW(s.hwnd, toWide(title).c_str());
}

void stop() {
    auto& s = st();
    // GUICLOSE_V2: если остановку начал сам крестик — не перебиваем его отсчёт, а ждём.
    if (s.closing.load()) {
        if (s.uiThread.joinable()) s.uiThread.join();
        if (s.closeThread.joinable()) s.closeThread.join();
        return;
    }
    // Остановка пришла изнутри (команда stop, Ctrl+C): тоже даём прочитать хвост лога.
    if (s.running.load()) {
        s.stopped.store(true);
        for (int left = kLingerSeconds; left > 0 && s.running.load(); --left) {
            setStatus(s.russian
                          ? "Сервер остановлен, мир сохранён. Окно закроется через " + std::to_string(left) +
                                " с (Esc или крестик — сразу)"
                          : "Server stopped, world saved. Closing in " + std::to_string(left) +
                                "s (Esc or the X button to close now)");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    if (s.hwnd) PostMessageW(s.hwnd, WM_NC_QUIT, 0, 0);
    if (s.uiThread.joinable()) s.uiThread.join();
    if (s.closeThread.joinable()) s.closeThread.detach();
}

// GUIWIZARD_V1: закрыть сразу, без плашки и отсчёта — нечего сохранять, мастер просто закрылся.
void stopImmediate() {
    auto& s = st();
    if (s.hwnd) PostMessageW(s.hwnd, WM_NC_QUIT, 0, 0);
    if (s.uiThread.joinable()) s.uiThread.join();
    if (s.closeThread.joinable()) s.closeThread.detach();
}

} // namespace nc::console

#else  // !_WIN32 — кроссплатформенные пустышки

namespace nc::console {

bool isAvailable() { return false; }
bool isRunning()   { return false; }
bool start(const GuiOptions&, CommandHandler, CloseHandler) { return false; }
void pushLine(LineLevel, std::string_view, std::string) {}
void setStatus(std::string) {}
void enableWizardInput(bool) {}
std::string readWizardLine() { return {}; }
void setTitle(const std::string&) {}
void stop() {}
void stopImmediate() {}

} // namespace nc::console

#endif // _WIN32
