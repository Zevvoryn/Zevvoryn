# Zevvoryn

**Zevvoryn** — самописный сервер Minecraft: Java Edition **1.21.1** (протокол 767) на C++.
Без Java, без JVM — один лёгкий нативный `zevvoryn.exe`.

> ⚠️ **Статус: 0.0.1 — Proof of Concept.** Это ранняя демо-версия: многого ещё нет,
> что-то работает криво. Не для продакшена.

*Zevvoryn is a custom Minecraft: Java Edition 1.21.1 server written in C++
(no Java/JVM required). This is an early proof-of-concept release.*

---

## Что уже работает

- Вход клиентом 1.21.1 (offline-mode + ONLINE), шифрование, сжатие пакетов
- Мультиплеер: видимость игроков, анимации, таб-лист, чат
- Генерация мира:  overworld (шумы/биомы/пещеры/структуры) или плоский мир
- Сохранение/загрузка мира и инвентарей, автосейв
- Блоки: ставить/ломать (+ эффекты ломания), двери, жидкости, сундуки (в т.ч. двойные/эндер)
- Инвентарь, экипировка, креатив-режим
- Бой: PvP, кулдаун атаки, криты (x1.5), броня, щит(не работает), урон от падения, экран смерти и респавн
- Цикл дня/ночи, погода, TPS-боссбар, иконка сервера (папка `icon_Server/`)
- Команды с автодополнением, операторы, защита спавна
- Русский/английский язык сервера, мастер первого запуска, консоль с лог-файлами

## Чего пока НЕТ (PoC!)

- Мобов и их ИИ, дропа предметов (с блоков/при смерти), крафта и печек
- Редстоуна, ферм, Незера/Энда, зачарований, алхимии
- Плагинов и API — в планах

## Сборка (Windows)

Требуется: **Visual Studio 2022** (MSVC v14.4x), **CMake**, **Ninja**.

```bat
rem из "x64 Native Tools Command Prompt for VS 2022":
cmake -S . -B build-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-ninja
rem результат: build-ninja\zevvoryn.exe
```

Запуск: `zevvoryn.exe`. При первом старте мастер настройки создаст `settings.properties`.

## Настройки (`settings.properties`)

| Ключ | Описание |
|---|---|
| `language` | `rus` / `eng` |
| `motd`, `sub-motd` | Описание сервера в списке серверов |
| `server-port` / `server-portv6` | Порты (по умолчанию 25565) |
| `max-players` | Лимит игроков |
| `view-distance`, `simulation-distance` | Дальность прорисовки/симуляции |
| `level-name`, `level-seed` | Имя мира и сид |
| `generator` | `default` — ванильная генерация, иначе плоский мир |
| `gamemode`, `force-gamemode` | Режим игры по умолчанию |
| `pvp`, `difficulty` | PvP и сложность |
| `ops` | Операторы через запятую (пусто = команды доступны всем) |
| `spawn-protection` | Радиус защиты спавна |
| `max-ram-gb`, `max-cores` | Лимиты ресурсов |
| `auto-save`, `auto-save-interval` | Автосохранение |
| `log-level` | Уровень логов |

Иконка сервера: положи `icon.png` **64x64** в папку `icon_Server/`.

## Команды (в игре)

`/help`, `/tps`, `/list`, `/spawn`, `/setworldspawn`, `/tp`,
`/gamemode` (+ шорткаты `/gm0`…`/gm3`), `/time`, `/weather`, `/say`,
`/setblock`, `/kick`, `/summon`, `/killall`

Консоль: `help`, `list`, `say`, `save-all`, `stop` (будет расширяться).

## Структура проекта

```
core/        ядро сервера, конфиг, логи, иконка
network/     сеть, буфера, протокол
crypto/      шифрование (RSA/AES)
world/       чанки, генерация мира
entity/      игроки и сущности
registries/  реестры блоков/предметов
biomes/, carving/  ванильный ворлдген
```

## Лицензия

Проект распространяется под лицензией **Apache License 2.0** — см. [LICENSE](LICENSE).

Zevvoryn содержит код и референсные материалы, производные от проекта
[Cuberite](https://cuberite.org) (Apache License 2.0) — см. [NOTICE](NOTICE)
и [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

"Minecraft" — товарный знак Mojang Synergies AB. Zevvoryn — независимый проект,
не является официальным продуктом Minecraft и никак не связан с Mojang/Microsoft.
