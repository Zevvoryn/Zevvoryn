param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Say([string]$Russian, [string]$English) {
    if ($script:Language -eq 'en') { Write-Host $English } else { Write-Host $Russian }
}

function Ask([string]$Russian, [string]$English) {
    if ($script:Language -eq 'en') { return Read-Host $English }
    return Read-Host $Russian
}

Write-Host '===== ZEVVORYN ====='
$desktop = [Environment]::GetFolderPath('Desktop')
if ([string]::IsNullOrWhiteSpace($desktop)) { $desktop = Join-Path $HOME 'Desktop' }
$defaultInstall = Join-Path $desktop 'zevvoryn\release'

while ($true) {
    $choice = Read-Host 'Select language / Выберите язык: 1) Русский  2) English'
    $script:Language = if ($choice -eq '2') { 'en' } else { 'ru' }

    Say "Путь установки (по умолчанию: [$defaultInstall]):" "Install path (default: [$defaultInstall]):"
    Say '1) Умолчание' '1) Default'
    Say '2) Ввести свой путь' '2) Enter a custom path'
    Say '3) Выбрать папку в проводнике' '3) Choose a folder in Explorer'
    Say '0) Назад к выбору языка' '0) Back to language selection'
    $pathChoice = Ask 'Ваш выбор [0/1/2/3]:' 'Choose [0/1/2/3]:'
    if ($pathChoice -eq '0') { continue }

    switch ($pathChoice) {
        '2' { $installDir = Ask 'Введите путь установки:' 'Enter the installation path:' }
        '3' {
            Add-Type -AssemblyName System.Windows.Forms
            $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
            $dialog.Description = 'Choose the parent folder for zevvoryn\release'
            $dialog.SelectedPath = $desktop
            if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) {
                Say 'Выбор отменён.' 'Folder selection cancelled.'
                exit 1
            }
            $installDir = Join-Path $dialog.SelectedPath 'zevvoryn\release'
        }
        default { $installDir = $defaultInstall }
    }
    break
}

$installDir = $installDir.Trim().Trim('"')
if ([string]::IsNullOrWhiteSpace($installDir)) { throw 'Installation path cannot be empty.' }
$installDir = [IO.Path]::GetFullPath($installDir)
if ($installDir.Contains('%') -or $installDir.Contains('"') -or $installDir.Contains("`n")) {
    Say 'Путь содержит символы, мешающие созданию файлов запуска. Выберите другой путь.' 'The path contains characters that prevent creating launcher files. Choose another path.'
    exit 1
}
New-Item -ItemType Directory -Path $installDir -Force | Out-Null

$driveRoot = [IO.Path]::GetPathRoot($installDir)
try {
    $freeBytes = [System.IO.DriveInfo]::new($driveRoot).AvailableFreeSpace
} catch {
    Say "Не удалось проверить свободное место на диске $driveRoot. Проверьте его вручную." "Could not check free space on $driveRoot. Check it manually."
    throw
}
if ($freeBytes -lt 1GB) {
    Say "Недостаточно места: нужно минимум 1 ГБ свободного места на диске $driveRoot." "Not enough space: at least 1 GB must be free on $driveRoot."
    exit 1
}
if ($freeBytes -lt 5GB) {
    $freeGiB = [math]::Round($freeBytes / 1GB, 1)
    Say "Свободно только $freeGiB ГБ. Для тестов рекомендуется не менее 5 ГБ." "Only $freeGiB GB is free. At least 5 GB is recommended for testing."
    $continue = Ask 'Продолжить всё равно? [д/н]:' 'Continue anyway? [y/N]:'
    if (($script:Language -eq 'en' -and $continue -notmatch '^(y|yes)$') -or ($script:Language -eq 'ru' -and $continue -notmatch '^(д|да|y|yes)$')) {
        Say 'Установка остановлена.' 'Installation cancelled.'
        exit 1
    }
}

$downloadPath = Join-Path $installDir ".zevvoryn-download-$PID.exe"
$stagedPath = Join-Path $installDir ".zevvoryn-new-$PID.exe"
$executablePath = Join-Path $installDir 'zevvoryn.exe'
$releaseUrl = 'https://github.com/Zevvoryn/Zevvoryn/releases/latest/download/zevvoryn.exe'
try {
    Say 'Скачиваю готовую сборку Windows x64; компилятор не требуется.' 'Downloading the prebuilt Windows x64 release; no compiler is needed.'
    Invoke-WebRequest -Uri $releaseUrl -OutFile $downloadPath -UseBasicParsing
    Say 'Готовая сборка скачана; подготавливаю запуск...' 'Prebuilt release downloaded; preparing the launcher...'
    $download = Get-Item -LiteralPath $downloadPath
    if ($download.Length -lt 1MB) { throw 'The downloaded executable is unexpectedly small; the release asset may be missing.' }
    Move-Item -LiteralPath $downloadPath -Destination $stagedPath -Force
    Move-Item -LiteralPath $stagedPath -Destination $executablePath -Force
} finally {
    Remove-Item -LiteralPath $downloadPath, $stagedPath -Force -ErrorAction SilentlyContinue
}

$runLines = @('@echo off', 'setlocal', 'pushd "%~dp0"', 'if not exist "plugins\cpp" mkdir "plugins\cpp"', 'if not exist "plugins\luau" mkdir "plugins\luau"', 'if not exist "plugins\jvm" mkdir "plugins\jvm"', 'if not exist "plugins\runtimes.json" (', '  >"plugins\runtimes.json" echo {', '  >>"plugins\runtimes.json" echo   "schema_version": 1,', '  >>"plugins\runtimes.json" echo   "notice": "Placeholder only: plugin runtimes are not implemented.",', '  >>"plugins\runtimes.json" echo   "runtimes": {', '  >>"plugins\runtimes.json" echo     "cpp": { "enabled": false },', '  >>"plugins\runtimes.json" echo     "luau": { "enabled": false },', '  >>"plugins\runtimes.json" echo     "jvm": { "enabled": false, "experimental": true }', '  >>"plugins\runtimes.json" echo   }', '  >>"plugins\runtimes.json" echo }', ')', '"%~dp0zevvoryn.exe" %*', 'set "serverExitCode=%ERRORLEVEL%"', 'popd', 'exit /b %serverExitCode%')
Set-Content -LiteralPath (Join-Path $installDir 'run_server.bat') -Value $runLines -Encoding ASCII

$configureNow = Ask 'Настроить сервер сейчас? [1=да/2=нет]:' 'Configure the server now? [1=yes/2=no]:'
if ($configureNow -eq '1') {
    $previousLanguage = [Environment]::GetEnvironmentVariable('ZEVVORYN_LANGUAGE', 'Process')
    try {
        $env:ZEVVORYN_LANGUAGE = if ($script:Language -eq 'en') { 'eng' } else { 'rus' }
        & (Join-Path $installDir 'run_server.bat')
    } finally {
        if ($null -eq $previousLanguage) {
            Remove-Item Env:ZEVVORYN_LANGUAGE -ErrorAction SilentlyContinue
        } else {
            $env:ZEVVORYN_LANGUAGE = $previousLanguage
        }
    }
}

Say "Установка завершена: $installDir`nЗапуск: $installDir\run_server.bat`nДля обновления повторно запустите этот установщик." "Installation complete: $installDir`nRun: $installDir\run_server.bat`nRerun this installer to update."
Write-Host '===== ZEVVORYN ====='
