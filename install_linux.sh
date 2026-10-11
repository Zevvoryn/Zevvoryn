#!/usr/bin/env bash
set -euo pipefail

LANGUAGE="ru"

say() {
    if [[ "$LANGUAGE" == "en" ]]; then printf '%s\n' "$2"; else printf '%s\n' "$1"; fi
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

desktop="$(xdg-user-dir DESKTOP 2>/dev/null || true)"
if [[ -z "$desktop" || "$desktop" == "$HOME" ]]; then
    if [[ -d "$HOME/Рабочий стол" ]]; then desktop="$HOME/Рабочий стол"; else desktop="$HOME/Desktop"; fi
fi
DEFAULT_INSTALL="$desktop/zevvoryn/release"

while true; do
    printf '%s\n' '===== ZEVVORYN ====='
    printf '%s\n' 'Выберите язык / Select language:' '1) Русский / Russian' '2) English'
    read_answer 'Ваш выбор [1/2]: ' 'Choose [1/2]: '
    if [[ "$REPLY" == "2" ]]; then LANGUAGE="en"; else LANGUAGE="ru"; fi

    say '' ''
    say "Выберите путь установки (по умолчанию: [$DEFAULT_INSTALL]):" "Choose install path (default: [$DEFAULT_INSTALL]):"
    say '1) Умолчание' '1) Default'
    say '2) Ввести свой путь' '2) Enter a custom path'
    say '3) Открыть выбор папки (KDE/Dolphin)' '3) Open folder picker (KDE/Dolphin)'
    say '0) Назад к выбору языка' '0) Back to language selection'
    read_answer 'Ваш выбор [0/1/2/3]: ' 'Choose [0/1/2/3]: '
    case "$REPLY" in
        0) continue ;;
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
    break
done
if [[ -z "${INSTALL_DIR:-}" ]]; then say 'Выбор папки отменён.' 'Folder selection was cancelled.'; exit 1; fi
INSTALL_DIR="$(mkdir -p -- "$INSTALL_DIR" && cd -- "$INSTALL_DIR" && pwd)"

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
    say "Свободно только $free_gib ГБ. Для тестов рекомендуется не менее 5 ГБ." "Only $free_gib GB is free. At least 5 GB is recommended for testing."
    read_answer 'Продолжить всё равно? [1=да/2=нет]: ' 'Continue anyway? [1=yes/2=no]: '
    [[ "$REPLY" == "1" ]] || { say 'Установка остановлена.' 'Installation cancelled.'; exit 1; }
fi

if [[ "$(uname -m)" != "x86_64" ]]; then
    say 'Готовая сборка пока доступна только для Linux x86_64.' 'The prebuilt release is currently available for Linux x86_64 only.'
    exit 1
fi
if ! command -v tar >/dev/null 2>&1 || { ! command -v curl >/dev/null 2>&1 && ! command -v wget >/dev/null 2>&1; }; then
    say 'Нужны tar и curl или wget. Установите их и запустите установщик повторно.' 'tar and either curl or wget are required. Install them and rerun the installer.'
    exit 1
fi

download_dir="$(mktemp -d "$INSTALL_DIR/.zevvoryn-download.XXXXXX")"
trap 'rm -rf -- "$download_dir"' EXIT
archive="$download_dir/zevvoryn-linux-x86_64.tar.gz"
release_url='https://github.com/Zevvoryn/Zevvoryn/releases/latest/download/zevvoryn-linux-x86_64.tar.gz'
say '' ''
say 'Скачиваю готовую сборку Linux x86_64; компилятор не требуется.' 'Downloading the prebuilt Linux x86_64 release; no compiler is needed.'
if command -v curl >/dev/null 2>&1; then
    if ! curl -fL --progress-bar "$release_url" -o "$archive"; then
        say 'Готовая Linux-сборка пока не опубликована или недоступна. Установщик не запускает долгую компиляцию; проверьте страницу релизов позже.' 'A prebuilt Linux release is not published or unavailable yet. The installer will not start a long source build; check the releases page later.'
        exit 1
    fi
else
    if ! wget --show-progress -O "$archive" "$release_url"; then
        say 'Готовая Linux-сборка пока не опубликована или недоступна. Установщик не запускает долгую компиляцию; проверьте страницу релизов позже.' 'A prebuilt Linux release is not published or unavailable yet. The installer will not start a long source build; check the releases page later.'
        exit 1
    fi
fi
say 'Проверяю архив и устанавливаю сервер...' 'Checking the archive and installing the server...'
tar -tzf "$archive" > "$download_dir/archive.list" || {
    say 'Не удалось прочитать архив сборки.' 'Could not read the release archive.'
    exit 1
}
grep -Fxq 'zevvoryn' "$download_dir/archive.list" || {
    say 'В архиве не найден исполняемый файл zevvoryn.' 'The archive does not contain the zevvoryn executable.'
    exit 1
}
tar -xzf "$archive" -C "$download_dir"
install -m 755 "$download_dir/zevvoryn" "$INSTALL_DIR/.zevvoryn.new"
mv -f -- "$INSTALL_DIR/.zevvoryn.new" "$INSTALL_DIR/zevvoryn"

cat > "$INSTALL_DIR/run_server.sh" <<EOF
#!/usr/bin/env bash
set -euo pipefail
ROOT="\$(cd -- "\$(dirname -- "\${BASH_SOURCE[0]}")" && pwd)"
cd -- "\$ROOT"
mkdir -p plugins/cpp plugins/luau plugins/jvm
if [[ ! -f plugins/runtimes.json ]]; then
    cat > plugins/runtimes.json <<'JSON'
{
  "schema_version": 1,
  "notice": "Placeholder only: plugin runtimes are not implemented.",
  "runtimes": {
    "cpp": { "enabled": false },
    "luau": { "enabled": false },
    "jvm": { "enabled": false, "experimental": true }
  }
}
JSON
fi
exec "\$ROOT/zevvoryn" "\$@"
EOF
chmod 755 "$INSTALL_DIR/run_server.sh"

say '\nХотите настроить сервер сейчас?' '\nWould you like to configure the server now?'
read_answer '1) Да  2) Нет: ' '1) Yes  2) No: '
if [[ "$REPLY" == "1" ]]; then
    if [[ "$LANGUAGE" == "en" ]]; then ZEVVORYN_LANGUAGE=eng; else ZEVVORYN_LANGUAGE=rus; fi
    export ZEVVORYN_LANGUAGE
    "$INSTALL_DIR/run_server.sh"
fi

printf '%s\n' '====== ZEVVORYN ======'
if [[ "$LANGUAGE" == "en" ]]; then
    printf 'Installation complete: %s\nRun with: %s/run_server.sh\n' "$INSTALL_DIR" "$INSTALL_DIR"
else
    printf 'Установка завершена: %s\nЗапуск: %s/run_server.sh\n' "$INSTALL_DIR" "$INSTALL_DIR"
fi
printf '%s\n' '====== ZEVVORYN ======'
