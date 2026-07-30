# Zevvoryn

**Minecraft Java Edition 1.21.1 сервер, написанный с нуля на C++20. Без JVM, без Java, без .jar.**

![version](https://img.shields.io/badge/version-0.1.0-blue)
![minecraft](https://img.shields.io/badge/Minecraft-1.21.1-green)
![protocol](https://img.shields.io/badge/protocol-767-lightgrey)
![c++](https://img.shields.io/badge/C%2B%2B-20-orange)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-informational)

---

## Что это

Zevvoryn — самостоятельная реализация серверной части Minecraft 1.21.1 (протокол 767) на чистом C++.
Ванильный клиент подключается к нему как к обычному серверу: никаких модов и никакой Java на машине сервера не требуется.

| Возможность | Статус |
|---|---|
| Подключение ванильного клиента 1.21.1 | ✅ |
| Генерация мира (биомы через cubiomes), сохранение/загрузка | ✅ |
| Чанки, освещение, блоки, взаимодействие | ✅ |
| Физика: падение блоков, вода/лава, красный камень (базово) | ✅ |
| Игроки: движение, инвентарь, режимы игры, урон, respawn | ✅ |
| Незер и Край + порталы | ✅ |
| Чат, команды, права | ✅ |
| Белый список (whitelist) | ✅ |
| RCON (совместим с обычными RCON-клиентами) | ✅ |
| Discord-бот + веб-панель управления | ✅ |
| MiniEdit (выделение и массовое редактирование блоков) | ✅ |
| Онлайн-режим / авторизация Mojang | ⚠️ только с OpenSSL |
| Мобы, структуры, вагонетки | 🚧 в работе |

---

## Требования

- **Windows 10/11 x64** или Linux x64
- Компилятор с C++20: MSVC 2022 (14.4x+), GCC 12+, Clang 15+
- CMake 3.20+
- Ninja (рекомендуется)
- Интернет при первой сборке — CMake сам скачает `nlohmann/json`
- *(опционально)* OpenSSL — нужен только для `online-mode=true`
- *(опционально)* Node.js 18+ — только для Discord-бота и веб-панели

---

## Сборка

### Windows (MSVC + Ninja)

Откройте **x64 Native Tools Command Prompt for VS 2022** в корне репозитория:

```bat
cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ninja --config Release
```

Готовый бинарник: `build-ninja\zevvoryn.exe`.

> **Важно:** запускать сборку нужно именно из окружения Visual Studio (`vcvars64.bat` уже применён),
> иначе CMake не найдёт `cl.exe` и вы увидите «Системе не удается найти указанный путь».

### Linux

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/zevvoryn
```

---

## Первый запуск

При первом запуске (когда рядом нет `settings.properties`) стартует мастер настройки:
язык, порт, seed, режим игры, сложность, максимум игроков, whitelist, RCON, Discord-бот и веб-панель.
По завершении мастер сам создаст `settings.properties`, а при необходимости — папку `DiscrordBotRcon`
с файлами бота и готовым `.env`.

```
zevvoryn.exe
```

---

## settings.properties

| Ключ | По умолчанию | Описание |
|---|---|---|
| `server-port` | `25565` | Порт сервера |
| `max-players` | `20` | Лимит игроков |
| `motd` | `Zevvoryn Server` | Описание в списке серверов |
| `level-seed` | random | Seed генерации мира |
| `gamemode` | `survival` | Режим игры по умолчанию |
| `difficulty` | `normal` | Сложность |
| `online-mode` | `false` | Авторизация Mojang (нужен OpenSSL) |
| `view-distance` | `10` | Радиус прогрузки чанков |
| `language` | `rus` | Язык логов и сообщений (`rus` / `eng`) |
| `white-list` | `false` | Включить белый список |
| `enable-rcon` | `false` | Включить RCON |
| `rcon.port` | `25575` | Порт RCON |
| `rcon.password` | — | Пароль RCON (обязателен при `enable-rcon=true`) |
| `rcon.max-clients` | `4` | Одновременных RCON-подключений |
| `rcon.log-commands` | `true` | Логировать команды из RCON |
| `auto-start-panel` | `false` | Запускать Discord-бот и веб-панель вместе с сервером |

---

## Команды консоли и чата

```
crash        end          gamemode     gm0 gm1 gm2 gm3
give         help         kick         killall
list         locate       mob          nether
overworld    reload       save         say
setblock     setworldspawn             skin
spawn        stop         summon       time
tp           tps          warprandomtick          weather
whitelist
```

`help` выведет подробности по каждой команде.

### Белый список

```
whitelist on
whitelist off
whitelist add <ник>
whitelist remove <ник>
whitelist list
```

Список хранится в `whitelist.json` и применяется сразу: при включении не-разрешённые игроки кикаются.

---

## RCON, Discord-бот и веб-панель

Включите RCON в `settings.properties`:

```properties
enable-rcon=true
rcon.port=25575
rcon.password=ваш_пароль
auto-start-panel=true
```

При `auto-start-panel=true` сервер сам поднимет `DiscrordBotRcon/index.js` и **синхронизирует** его `.env`
с актуальными настройками RCON, так что пароли не разъезжаются.

Веб-панель по умолчанию: **http://127.0.0.1:3000**

### DiscrordBotRcon/.env

| Переменная | Назначение |
|---|---|
| `RCON_HOST` | Адрес сервера (обычно `127.0.0.1`) |
| `RCON_PORT` | Порт RCON |
| `RCON_PASSWORD` | Пароль RCON |
| `WEB_ENABLED` | `true` (по умолчанию) — включает веб-панель |
| `WEB_PORT` | Порт панели, по умолчанию `3000` |
| `DISCORD_TOKEN` | Токен бота — только если нужен Discord |
| `DISCORD_CLIENT_ID` | ID приложения Discord |
| `DISCORD_GUILD_ID` | ID сервера Discord для регистрации команд |

Без `DISCORD_TOKEN` бот просто не подключается к Discord — веб-панель и RCON работают как обычно.

---

## Логи и остановка

- Логи пишутся в `logs/` с временными метками; в консоли — цветной вывод в UTF-8.
- **Правильная остановка:** команда `stop` в консоли (или через RCON / панель).
- **Крестик окна:** сервер перехватывает закрытие, кикает игроков, сохраняет мир и игроков,
  гасит панель и только потом выходит. Windows даёт закрывающейся консоли около 5 секунд —
  этого хватает на обычный мир, но `stop` всё равно надёжнее.
- **Выключение/перезагрузка Windows:** лимит снимается через `ShutdownBlockReasonCreate` —
  система показывает «Это приложение мешает завершению работы» и ждёт, пока мир сохранится.

---

## Структура проекта

```
src/
  main.cpp            точка входа, автозапуск панели, обработчики закрытия
  core/               сервер, конфиг, логи, RCON, whitelist, MiniEdit
  network/            сокеты, пакеты, сжатие, шифрование
  protocol/           реализация протокола 767
  world/              генерация, чанки, сохранение, физика
  entity/             игроки и сущности
  registries/         блоки, предметы, биомы
  crypto/             шифрование и хеши
  utils/              вспомогательные утилиты
  thirdparty/cubiomes генератор биомов
DiscrordBotRcon/      Discord-бот и веб-панель (Node.js)
```

---

## Частые проблемы

| Симптом | Решение |
|---|---|
| «Системе не удается найти указанный путь» | Запускайте сборку из x64 Native Tools Command Prompt |
| `error C2059` + `C4003 ... "min"/"max"` | Заголовок с `windows.h`/`winsock2.h` подключён без `NOMINMAX` |
| Кракозябры вместо русских букв | Обновите `core/log.hpp` — вывод идёт через `WriteConsoleW` |
| Панель не открывается | Проверьте `WEB_ENABLED=true` и что порт 3000 свободен |
| RCON не подключается | `enable-rcon=true`, пароль не пустой, порт не занят |
| Клиент не видит сервер | Версия клиента строго 1.21.1, проверьте порт и брандмауэр |

---

## Планы

- Мобы и их ИИ
- Генерация структур (деревни, крепости, порталы)
- Вагонетки и транспорт
- Распространение травы и мицелия
- Медленное фоновое автосохранение

---

## Автор

**yicekot811** — https://github.com/Zevvoryn/Zevvoryn

Minecraft — торговая марка Mojang Studios. Проект не связан с Mojang и Microsoft.
