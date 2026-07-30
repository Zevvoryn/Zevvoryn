<div align="center">

# Zevvoryn

**Сервер Minecraft: Java Edition 1.21.1 на чистом C++20.**
Без JVM. Без JNI. Без обёрток.

![version](https://img.shields.io/badge/version-0.1.0-blue)
![minecraft](https://img.shields.io/badge/Minecraft-1.21.1-green)
![protocol](https://img.shields.io/badge/protocol-767-lightgrey)
![c++](https://img.shields.io/badge/C%2B%2B-20-blue)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-lightgrey)

</div>

---

## Что это

Zevvoryn — самостоятельная реализация серверной части Minecraft: Java Edition 1.21.1 (протокол 767),
написанная с нуля на C++20. Ванильный клиент подключается к нему напрямую, без модов и прокси.

Цель проекта — скорость запуска и предсказуемое потребление ресурсов там, где официальный
Java-сервер тратит секунды на старт JVM и сотни мегабайт на heap.

```
[17:01:10.482] [Server thread/INFO] Done! (за 0.05 сек.)
```

---

## Возможности

| Блок | Состояние |
|------|-----------|
| Протокол 1.21.1 (767), handshake, статус, игра | ✅ |
| Онлайн/офлайн режим, шифрование, сжатие пакетов | ✅ |
| Ванильная генерация мира (cubiomes), сид как в ваниле | ✅ |
| Формат Anvil: чтение и запись региона/чанков | ✅ |
| Незер и Энд, порталы между измерениями | ✅ |
| Физика блоков, жидкости, обновления по тикам | ✅ |
| Инвентарь, предметы, крафт, режимы игры | ✅ |
| Мобы (базовый набор), спавн, ИИ-заготовки | 🚧 |
| Чат, системные сообщения, tab-list, скины | ✅ |
| Белый список (`whitelist.txt`) | ✅ |
| RCON (Source RCON, совместим с любым клиентом) | ✅ |
| Discord-бот + веб-панель управления | ✅ |
| Мастер первичной настройки (RU/EN) | ✅ |
| Автосохранение, корректное сохранение при закрытии окна | ✅ |
| Логи с ротацией (15 последних), защита от крашей | ✅ |
| MiniEdit — быстрые операции с регионами блоков | ✅ |

---

## Требования

- **Windows 10/11 x64** или **Linux x64**
- Компилятор с полной поддержкой C++20: **MSVC 2022** (17.10+) или GCC 13+/Clang 17+
- **CMake 3.20+** и **Ninja**
- **Node.js 18+** — только если нужны Discord-бот и веб-панель

---

## Сборка

### Windows (MSVC + Ninja)

Из «x64 Native Tools Command Prompt for VS 2022»:

```bat
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Готовый `zevvoryn.exe` кладите в отдельную папку — рядом с ним сервер создаст
`settings.properties`, `world/`, `logs/` и `DiscrordBotRcon/`.

### Linux

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/zevvoryn
```

---

## Первый запуск

При первом старте (когда `settings.properties` ещё нет) включается мастер настройки:
язык, порт, режим игры, сложность, дальность прорисовки, белый список, RCON,
Discord-бот и веб-панель. Всё, что вы выберете, сохраняется в `settings.properties`,
а для панели дополнительно создаётся `DiscrordBotRcon/.env`.

Дальше сервер стартует обычным образом:

```
zevvoryn.exe                       # конфиг по умолчанию
zevvoryn.exe my-settings.properties  # свой файл конфигурации
```

---

## Настройки (`settings.properties`)

Формат — обычные `ключ=значение`, как в PocketMine/PMMP.

```properties
# ── Сервер ──
language=rus              # rus | eng — язык логов и сообщений
motd=Zevvoryn Server
server-port=25565
max-players=80
view-distance=16
simulation-distance=8

# ── Мир ──
level-name=world
level-type=DEFAULT        # DEFAULT | FLAT | VOID
level-seed=0              # 0 = случайный

# ── Геймплей ──
gamemode=creative
difficulty=2              # 0 мирная … 3 хардкор
pvp=true
spawn-protection=16

# ── Доступ ──
online-mode=false
white-list=false          # список ников — в whitelist.txt

# ── RCON ──
enable-rcon=true
rcon.port=25575
rcon.password=смените_меня
rcon.max-clients=4

# ── Панель ──
auto-start-panel=true     # поднимать Discord-бота/веб-панель вместе с сервером
```

> **Важно:** пароль RCON задаётся **только здесь**. Файл `DiscrordBotRcon/.env`
> сервер синхронизирует сам при каждом запуске — руками его править не нужно.

---

## Команды консоли

```
help                      справка
list                      кто онлайн
say <текст>               сообщение от консоли
gamemode <0-3> [ник]      сменить режим (или gm0..gm3)
give <ник> <предмет>      выдать предмет
tp <ник> <x> <y> <z>      телепорт
spawn / setworldspawn     точка спавна
time set <значение>       время суток
weather <clear|rain|thunder>
summon / mob / killall    мобы
setblock <x> <y> <z> <блок>
locate <структура>
nether / end / overworld  переход по измерениям
whitelist <add|remove|on|off|list>
kick <ник>
save                      сохранить мир немедленно
tps                       производительность
reload                    мягкая перезагрузка конфига
stop                      корректная остановка
```

Те же команды доступны через RCON, Discord-бота и веб-панель.

---

## Белый список

`whitelist.txt` рядом с сервером, по одному нику в строке:

```
# Белый список игроков
Notch
Steve
```

Включается через `white-list=true` или командой `whitelist on`.

---

## RCON, Discord-бот и веб-панель

В комплекте идёт папка `DiscrordBotRcon` — Node.js-приложение, которое подключается
к серверу по RCON и даёт два интерфейса: команды в Discord и веб-панель в браузере.

```bash
cd DiscrordBotRcon
npm install
```

При `auto-start-panel=true` сервер сам поднимает её вместе с `zevvoryn.exe`
(и сам записывает актуальные RCON-порт и пароль в `.env`).

| Переменная | Назначение |
|------------|-----------|
| `RCON_HOST`, `RCON_PORT`, `RCON_PASSWORD` | заполняются сервером автоматически |
| `WEB_ENABLED` | `false` — выключить веб-панель (по умолчанию включена) |
| `WEB_HOST`, `WEB_PORT` | адрес панели, по умолчанию `127.0.0.1:3000` |
| `WEB_PASSWORD` | пароль на вход в панель |
| `DISCORD_TOKEN`, `CLIENT_ID`, `GUILD_ID` | нужны только для Discord-бота |
| `ADMIN_ROLE_ID`, `COMMAND_CHANNEL_ID` | ограничение прав в Discord |

Панель показывает состояние сервера, онлайн, белый список и даёт консоль в браузере:
`http://127.0.0.1:3000`

Регистрация слэш-команд Discord (один раз):

```bash
npm run deploy
```

---

## Логи и остановка

- Логи пишутся в `logs/log-ДД.ММ.ГГ.log` (в английской локали — `ММ.ДД.ГГ`), хранятся 15 последних.
- Правильная остановка — команда `stop`.
- Закрытие окна крестиком тоже безопасно: сервер кикает игроков, сохраняет мир и данные
  игроков и только потом завершается. Windows даёт закрывающемуся консольному приложению
  около пяти секунд, поэтому при очень больших мирах предпочтительнее `stop`.
- При выходе из системы или выключении компьютера сервер выставляет Windows
  «причину блокировки завершения работы» и сохраняет мир без ограничения по времени.
- После аварийного завершения сервер сообщит об этом при следующем запуске.

---

## Структура проекта

```
core/        конфиг, логи, RCON, whitelist, команды, MiniEdit, ядро сервера
network/     TCP-сервер, соединения, сжатие
protocol/    кодеки пакетов 1.21.1 и общие структуры
entity/      игроки и мобы
world/       чанки, генерация, биомы, формат Anvil
registries/  реестры блоков, предметов, биомов
crypto/      шифрование протокола
utils/       NBT и вспомогательное
data/        игровые данные Minecraft
thirdparty/  cubiomes (ванильная генерация)
tools/       смоук- и стресс-боты на Python
DiscrordBotRcon/  Discord-бот и веб-панель (Node.js)
```

Дополнительная документация: [`PHYSICS.md`](PHYSICS.md) — модель физики,
[`MINIEDIT.md`](MINIEDIT.md) — операции с регионами блоков.

---

## Частые проблемы

**Панель пишет «RCON: Отключено».**
Проверьте, что в `settings.properties` стоит `enable-rcon=true` и непустой `rcon.password`
(с пустым паролем удалённая консоль не запускается). В логе старта должна быть строка
`Удалённая консоль слушает 0.0.0.0:25575`.

**Панель не открывается.**
Порт по умолчанию — `3000`, адрес `http://127.0.0.1:3000`. Причину падения ищите в
`DiscrordBotRcon/panel-crash.log`.

**Автозапуск панели пропущен.**
Нужен установленный Node.js в `PATH` и выполненный `npm install` в папке `DiscrordBotRcon`.

**Клиент не видит сервер в локальной сети.**
Откройте TCP-порт `25565` в брандмауэре Windows.

---

## Дорожная карта

- Полноценный ИИ и разнообразие мобов
- Генерация структур и деревень
- Красный камень и механизмы
- Вагонетки и транспорт
- Расширение веб-панели (карта мира, графики TPS)

---

## Лицензия

См. файл [`LICENSE`](LICENSE). Сторонние компоненты и их лицензии перечислены
в `license/THIRD-PARTY-NOTICES.txt` (в частности [cubiomes](https://github.com/Cubitect/cubiomes)).

Minecraft — товарный знак Mojang Studios. Проект не связан с Mojang Studios и Microsoft
и не содержит их кода.

---

<div align="center">

**GitHub:** https://github.com/Zevvoryn/Zevvoryn

</div>
