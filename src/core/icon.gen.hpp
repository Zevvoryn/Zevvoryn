#pragma once
// ICON_V1: встроенная иконка сервера (64x64 PNG, base64) для списка серверов +
// папка icon_Server, через которую пользователь может подменить иконку своей.
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace nc::icon {

// ICON_V1: встроенная иконка ZEVVORYN (PNG 64x64, base64).
inline const char* kDefaultIconB64 =
    "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";

// ICON_V1: base64-кодирование байтов (для пользовательской icon_Server/icon.png).
inline std::string base64Encode(const std::vector<unsigned char>& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        const unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out.push_back(tbl[(v >> 18) & 63]); out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);  out.push_back(tbl[v & 63]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        const unsigned v = in[i] << 16;
        out.push_back(tbl[(v >> 18) & 63]); out.push_back(tbl[(v >> 12) & 63]);
        out.push_back('='); out.push_back('=');
    } else if (i + 2 == in.size()) {
        const unsigned v = (in[i] << 16) | (in[i + 1] << 8);
        out.push_back(tbl[(v >> 18) & 63]); out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]); out.push_back('=');
    }
    return out;
}

// ICON_V1: готовит папку icon_Server (файл-инструкция на языке сервера) и возвращает
// favicon для Status Response: пользовательская icon_Server/icon.png или встроенная.
inline std::string loadServerIconFavicon(bool ru, std::string& note) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories("icon_Server", ec);
    // ICON_V3: \u-эскейпы вместо u8-литерала — MSVC без флага /utf-8 читает исходник как ANSI,
    // портит кириллицу в u8"..." и имя файла опять выходит кракозябрами. Эскейпы от кодировки исходника не зависят.
    const fs::path readmePath = ru
        ? fs::path(L"icon_Server/\u041A\u0410\u041A_\u041F\u041E\u041C\u0415\u041D\u042F\u0422\u042C_\u0418\u041A\u041E\u041D\u041A\u0423.txt")
        : fs::path(L"icon_Server/HOW_TO_CHANGE_ICON.txt");
    { // ICON_V3: автоматически сносим старый кракозябро-файл (его имя = UTF-8 байты, пропущенные через ANSI)
        std::error_code ec2;
        fs::remove(fs::path("icon_Server/КАК_ПОМЕНЯТЬ_ИКОНКУ.txt"), ec2);
        (void)ec2;
    }
    if (!fs::exists(readmePath, ec)) {
        std::ofstream rm(readmePath, std::ios::binary);
        if (rm.is_open()) rm << (ru ? "Иконка сервера Zevvoryn\n=======================\n\nХотите свою иконку в списке серверов?\n\n1. Подготовьте PNG-файл размером РОВНО 64x64 пикселя.\n2. Назовите его: icon.png\n3. Положите его в эту папку (icon_Server/icon.png).\n4. Перезапустите сервер — ваша иконка заменит встроенную.\n\nЧтобы вернуть встроенную иконку ZEVVORYN — удалите icon.png и перезапустите сервер.\nТребования: формат PNG, размер 64x64, файл не больше 1 МБ.\n" : "Zevvoryn server icon\n====================\n\nWant your own icon in the server list?\n\n1. Prepare a PNG file EXACTLY 64x64 pixels.\n2. Name it: icon.png\n3. Put it into this folder (icon_Server/icon.png).\n4. Restart the server - your icon will replace the built-in one.\n\nTo restore the built-in ZEVVORYN icon - delete icon.png and restart the server.\nRequirements: PNG format, 64x64 size, up to 1 MB.\n");
    }
    const char* iconPath = "icon_Server/icon.png";
    if (fs::exists(iconPath, ec)) {
        std::ifstream f(iconPath, std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!bytes.empty() && bytes.size() <= 1024 * 1024) { // ICON_V2: лимит поднят до 1 МБ
            note = ru ? "Иконка сервера: своя (icon_Server/icon.png)" : "Server icon: custom (icon_Server/icon.png)";
            return std::string("data:image/png;base64,") + base64Encode(bytes);
        }
        note = ru ? "icon_Server/icon.png не подошла (пустая или больше 1 МБ) — использую встроенную" : "icon_Server/icon.png rejected (empty or over 1 MB) - using built-in";
        return std::string("data:image/png;base64,") + kDefaultIconB64;
    }
    note = ru ? "Иконка сервера: встроенная ZEVVORYN (своя — через icon_Server/icon.png)" : "Server icon: built-in ZEVVORYN (custom via icon_Server/icon.png)";
    return std::string("data:image/png;base64,") + kDefaultIconB64;
}

} // namespace nc::icon
