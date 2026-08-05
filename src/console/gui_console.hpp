#pragma once
// GUICON_V1 — собственный терминал сервера (вертикальный срез).
//
// Зачем: при console-mode=classic мы живём в чужом окне (conhost или Windows
// Terminal). Крестик там жмут не у нас, и Windows даёт процессу ~5 секунд на
// завершение — ровно поэтому сохранение мира приходилось торопить. Здесь окно
// наше, WM_CLOSE обрабатываем сами и держим его столько, сколько нужно сейву.
//
// Правила, которых держится этот файл:
//  * ядро остаётся консольным и кроссплатформенным. Вне Windows все функции —
//    пустышки, isAvailable() == false, поведение сервера не меняется;
//  * логгер сюда только КЛАДЁТ строки в очередь и никогда не ждёт отрисовку;
//  * рисуем по событию, а не в цикле: в простое окно спит, CPU ~0%;
//  * рисуем только видимые строки (виртуальный список), поэтому объём лога на
//    скорость не влияет;
//  * буфер кольцевой, память ограничена сверху. Полный лог по-прежнему пишется
//    в файл через core/log.hpp, здесь только то, что видно на экране.

#include "../core/types.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace nc::console {

enum class LineLevel : u8 { Trace, Debug, Info, Warn, Error, Fatal, Input, Chat };

struct GuiOptions {
    std::string title    = "Zevvoryn";
    bool        russian  = true;
    i32         maxLines = 20000;  // кольцевой буфер: верхняя граница памяти
};

// Команда, введённая в поле ввода. Вызывается из UI-потока; обработчик обязан
// быть быстрым и просто положить строку в очередь тик-потока.
using CommandHandler = std::function<void(const std::string&)>;
// Пользователь нажал крестик. Обработчик выполняется в отдельном потоке и может
// работать сколько угодно: окно висит с плашкой «Сохранение мира…».
using CloseHandler   = std::function<void()>;

// Собран ли GUI в этой сборке (Windows + не headless).
bool isAvailable();
// Запущено ли окно прямо сейчас. Логгер по этому флагу решает, дублировать ли
// строку в окно.
bool isRunning();

// Поднимает окно в отдельном потоке. false = не получилось; вызывающий обязан
// молча остаться в classic-режиме.
bool start(const GuiOptions& options, CommandHandler onCommand, CloseHandler onClose);

// Кладёт строку в очередь отрисовки. Потокобезопасно, не блокирует.
void pushLine(LineLevel level, std::string_view tag, std::string text);

// Плашка поверх лога: "Сохранение мира…". Пустая строка убирает плашку.
void setStatus(std::string text);

// GUIWIZARD_V1: первый запуск — мастер настройки идёт прямо в этом окне вместо
// classic-консоли, чтобы не терять баннер/лог из-за позднего создания окна.
// enableWizardInput(true) переключает Enter на очередь readWizardLine() вместо
// onCommand (сервер ещё не существует). readWizardLine() блокирует вызывающий
// поток до Enter; возвращает "", если окно закрыли, не дождавшись ответа.
void enableWizardInput(bool on);
std::string readWizardLine();

// Меняет заголовок окна (например, после того как мастер узнал MOTD).
void setTitle(const std::string& title);

// Закрывает окно. Вызывается после того, как сервер уже остановлен.
void stop();

// GUIWIZARD_V1: закрыть окно сразу, без плашки «мир сохранён» — используется,
// когда мастер только что закрылся мастером выбора classic и сохранять нечего.
void stopImmediate();

} // namespace nc::console
