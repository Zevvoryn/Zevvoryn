#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" 2>/dev/null && pwd || true)"
SOURCE_DIR=""
if [[ -n "$SCRIPT_DIR" && -f "$SCRIPT_DIR/CMakeLists.txt" ]]; then SOURCE_DIR="$SCRIPT_DIR"; fi
LANGUAGE="ru"

say() {
    if [[ "$LANGUAGE" == "en" ]]; then printf '%s\n' "$2"; else printf '%s\n' "$1"; fi
}

get_target_name() {
    local target
    target="$(sed -nE 's/^[[:space:]]*add_executable[[:space:]]*\([[:space:]]*([[:alnum:]_.+-]+).*/\1/p' "$SOURCE_DIR/CMakeLists.txt" | head -n 1)"
    if [[ -z "$target" ]]; then
        say 'Не удалось определить имя серверной программы в CMakeLists.txt.' 'Could not determine the server executable name from CMakeLists.txt.'
        exit 1
    fi
    printf '%s' "$target"
}

read_answer() {
    local prompt_ru="$1" prompt_en="$2" answer
    if [[ ! -r /dev/tty ]]; then
        say 'Этот установщик интерактивный: запустите его в терминале.' 'This installer is interactive; run it from a terminal.'
        exit 1
    fi
    if [[ "$LANGUAGE" == "en" ]]; then read -r -p "$prompt_en" answer </dev/tty; else read -r -p "$prompt_ru" answer </dev/tty; fi
    REPLY="$answer"
}

printf '%s\n' '===== ZEVVORYN ====='
printf '%s\n' 'Выберите язык / Select language:' '1) Русский / Russian' '2) English'
read_answer 'Ваш выбор [1/2]: ' 'Choose [1/2]: '
[[ "$REPLY" == "2" ]] && LANGUAGE="en"

desktop="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
if [[ -z "$desktop" || "$desktop" == "$HOME" ]]; then
    if [[ -d "$HOME/Рабочий стол" ]]; then desktop="$HOME/Рабочий стол"; else desktop="$HOME/Desktop"; fi
fi
DEFAULT_INSTALL="$desktop/zevvoryn/release"

say '' ''
say "Выберите путь установки (по умолчанию: [$DEFAULT_INSTALL]):" "Choose install path (default: [$DEFAULT_INSTALL]):"
say '1) Умолчание' '1) Default'
say '2) Ввести свой путь' '2) Enter a custom path'
say '3) Открыть выбор папки (KDE/Dolphin)' '3) Open folder picker (KDE/Dolphin)'
read_answer 'Ваш выбор [1/2/3]: ' 'Choose [1/2/3]: '
case "$REPLY" in
    2)
        read_answer 'Введите полный путь установки: ' 'Enter the full install path: '
        INSTALL_DIR="${REPLY/#\~/$HOME}"
        ;;
    3)
        if command -v kdialog >/dev/null 2>&1; then
            INSTALL_DIR="$(kdialog --getexistingdirectory "$desktop" 2>/dev/null || true)"
            [[ -n "$INSTALL_DIR" ]] && INSTALL_DIR="$INSTALL_DIR/zevvoryn/release"
        elif command -v zenity >/dev/null 2>&1; then
            INSTALL_DIR="$(zenity --file-selection --directory --filename="$desktop/" 2>/dev/null || true)"
            [[ -n "$INSTALL_DIR" ]] && INSTALL_DIR="$INSTALL_DIR/zevvoryn/release"
        else
            say 'Не найден KDE kdialog или zenity; введите путь вручную.' 'Neither KDE kdialog nor zenity is installed; enter the path manually.'
            read_answer 'Введите полный путь установки: ' 'Enter the full install path: '
            INSTALL_DIR="${REPLY/#\~/$HOME}"
        fi
        ;;
    *) INSTALL_DIR="$DEFAULT_INSTALL" ;;
esac
if [[ -z "${INSTALL_DIR:-}" ]]; then say 'Выбор папки отменён.' 'Folder selection was cancelled.'; exit 1; fi
INSTALL_DIR="$(mkdir -p -- "$INSTALL_DIR" && cd -- "$INSTALL_DIR" && pwd)"
BUILD_DIR="$INSTALL_DIR/.build"

free_kib="$(df -Pk -- "$INSTALL_DIR" | awk 'NR == 2 { print $4 }')"
if [[ ! "$free_kib" =~ ^[0-9]+$ ]]; then
    say 'Не удалось проверить свободное место на диске; проверьте его вручную.' 'Could not check available disk space; check it manually.'
    exit 1
fi
minimum_kib=$((1024 * 1024))
recommended_kib=$((5 * 1024 * 1024))
if ((free_kib < minimum_kib)); then
    say 'Недостаточно места: нужно минимум 1 ГБ свободного места на диске установки.' 'Not enough space: at least 1 GB must be free on the installation drive.'
    exit 1
fi
if ((free_kib < recommended_kib)); then
    free_gib="$(awk -v kib="$free_kib" 'BEGIN { printf "%.1f", kib / 1024 / 1024 }')"
    say "Свободно только $free_gib ГБ. Для сборки и тестов рекомендуется не менее 5 ГБ." "Only $free_gib GB is free. At least 5 GB is recommended for building and testing."
    read_answer 'Продолжить всё равно? [1=да/2=нет]: ' 'Continue anyway? [1=yes/2=no]: '
    [[ "$REPLY" == "1" ]] || { say 'Установка остановлена.' 'Installation cancelled.'; exit 1; }
fi

pick_compiler() {
    if [[ -n "${CXX:-}" ]] && command -v "$CXX" >/dev/null 2>&1; then
        CXX_BIN="$(command -v "$CXX")"
        return
    fi
    for candidate in c++ g++ clang++; do
        if command -v "$candidate" >/dev/null 2>&1; then CXX_BIN="$(command -v "$candidate")"; return; fi
    done
    CXX_BIN=''
}
pick_compiler
missing=()
command -v cmake >/dev/null 2>&1 || missing+=(cmake)
command -v make >/dev/null 2>&1 || missing+=(make)
[[ -n "$CXX_BIN" ]] || missing+=(c++-compiler)
if [[ -z "$SOURCE_DIR" ]]; then command -v git >/dev/null 2>&1 || missing+=(git); fi
if ((${#missing[@]})); then
    say 'Нужны cmake и компилятор C++23. Установить системные пакеты сейчас?' 'CMake and a C++23 compiler are required. Install the system packages now?'
    read_answer 'Установить зависимости? [1=да/2=нет]: ' 'Install dependencies? [1=yes/2=no]: '
    [[ "$REPLY" == "1" ]] || { say 'Установка остановлена; зависимости не установлены.' 'Installation stopped; no dependencies were installed.'; exit 1; }
    if [[ -f /etc/arch-release ]] && command -v pacman >/dev/null 2>&1; then
        sudo pacman -S --needed --noconfirm base-devel cmake zlib git
    elif command -v apt-get >/dev/null 2>&1; then
        sudo apt-get update
        sudo apt-get install -y build-essential cmake zlib1g-dev git
    elif command -v dnf >/dev/null 2>&1; then
        sudo dnf install -y gcc-c++ cmake make zlib-devel git
    elif command -v zypper >/dev/null 2>&1; then
        sudo zypper --non-interactive install gcc-c++ cmake make zlib-devel git
    else
        say 'Не удалось определить менеджер пакетов; установите cmake, C++23 compiler, make и zlib-devel вручную.' 'Package manager not recognized; install cmake, a C++23 compiler, make, and zlib development files manually.'
        exit 1
    fi
fi

if [[ -z "$SOURCE_DIR" ]]; then
    SOURCE_DIR="$INSTALL_DIR/.source"
    if [[ ! -f "$SOURCE_DIR/CMakeLists.txt" ]]; then
        source_tmp="$INSTALL_DIR/.source-download"
        if [[ -e "$source_tmp" ]]; then
            say "Папка загрузки уже существует: $source_tmp" "A source download folder already exists: $source_tmp"
            exit 1
        fi
        say 'Загружаю исходники Zevvoryn с GitHub…' 'Downloading Zevvoryn sources from GitHub…'
        if ! git clone --depth 1 https://github.com/Zevvoryn/Zevvoryn.git "$source_tmp"; then
            if [[ -e "$source_tmp" ]]; then rm -rf -- "$source_tmp"; fi
            if ! git clone --depth 1 https://github.com/xzz11222/Zevvoryn.git "$source_tmp"; then
                if [[ -e "$source_tmp" ]]; then rm -rf -- "$source_tmp"; fi
                say 'Не удалось загрузить исходники ни из одного репозитория.' 'Could not download sources from either repository.'
                exit 1
            fi
        fi
        if [[ ! -f "$source_tmp/CMakeLists.txt" ]]; then
            say 'В загруженном репозитории не найден CMakeLists.txt.' 'The downloaded repository does not contain CMakeLists.txt.'
            exit 1
        fi
        mv -- "$source_tmp" "$SOURCE_DIR"
    fi
fi

pick_compiler
if [[ -z "$CXX_BIN" ]] || ! "$CXX_BIN" -std=c++23 -x c++ -fsyntax-only - <<< 'int main() { return 0; }' >/dev/null 2>&1; then
    say 'Компилятор по умолчанию не поддерживает C++23. Нужен GCC 13+ или Clang 17+.' 'The default compiler lacks C++23 support. GCC 13+ or Clang 17+ is required.'
    exit 1
fi

say '' ''
say 'Собираю Zevvoryn; загружаются только недостающие пакеты сборки.' 'Building Zevvoryn; only missing build packages are downloaded.'
TARGET_NAME="$(get_target_name)"
cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="$CXX_BIN" \
    -DCMAKE_RUNTIME_OUTPUT_DIRECTORY="$INSTALL_DIR"
cmake --build "$BUILD_DIR" --target "$TARGET_NAME" --parallel

cat > "$INSTALL_DIR/run_server.sh" <<EOF
#!/usr/bin/env bash
set -euo pipefail
cd -- "$INSTALL_DIR"
exec "$INSTALL_DIR/$TARGET_NAME" "\$@"
EOF
cat > "$INSTALL_DIR/compile.sh" <<EOF
#!/usr/bin/env bash
set -euo pipefail
cmake --build "$BUILD_DIR" --target "$TARGET_NAME" --parallel
EOF
chmod 755 "$INSTALL_DIR/run_server.sh" "$INSTALL_DIR/compile.sh"

say '\nХотите настроить сервер сейчас?' '\nWould you like to configure the server now?'
read_answer '1) Да  2) Нет: ' '1) Yes  2) No: '
if [[ "$REPLY" == "1" ]]; then
    if [[ "$LANGUAGE" == "en" ]]; then ZEVVORYN_LANGUAGE=eng; else ZEVVORYN_LANGUAGE=rus; fi
    export ZEVVORYN_LANGUAGE
    "$INSTALL_DIR/run_server.sh"
fi

printf '%s\n' '====== ZEVVORYN ======'
if [[ "$LANGUAGE" == "en" ]]; then
    printf 'Installation complete: %s\nRun with: %s/run_server.sh\nRecompile with: %s/compile.sh\n' "$INSTALL_DIR" "$INSTALL_DIR" "$INSTALL_DIR"
else
    printf 'Установка завершена: %s\nЗапуск: %s/run_server.sh\nПересборка: %s/compile.sh\n' "$INSTALL_DIR" "$INSTALL_DIR" "$INSTALL_DIR"
fi
printf '%s\n' '====== ZEVVORYN ======'
