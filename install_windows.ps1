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

function Get-VisualStudioPath {
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $path = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace(($path | Select-Object -First 1))) { return $null }
    return ($path | Select-Object -First 1)
}

function Refresh-Path {
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$machinePath;$userPath"
    $cmakeBin = Join-Path $env:ProgramFiles 'CMake\bin'
    if (Test-Path -LiteralPath (Join-Path $cmakeBin 'cmake.exe')) { $env:Path = "$cmakeBin;$env:Path" }
}

function Get-CMakeVersion($CMakeCommand) {
    if (-not $CMakeCommand) { return $null }
    $versionLine = & $CMakeCommand.Source --version | Select-Object -First 1
    if ($versionLine -match '(\d+\.\d+(?:\.\d+)?)') { return [Version]$Matches[1] }
    return $null
}

Write-Host '===== ZEVVORYN ====='
$choice = Read-Host 'Select language / Выберите язык: 1) Русский  2) English'
$script:Language = if ($choice -eq '2') { 'en' } else { 'ru' }

$desktop = [Environment]::GetFolderPath('Desktop')
if ([string]::IsNullOrWhiteSpace($desktop)) { $desktop = Join-Path $HOME 'Desktop' }
$defaultInstall = Join-Path $desktop 'zevvoryn\release'
Say "Путь установки (по умолчанию: [$defaultInstall]):" "Install path (default: [$defaultInstall]):"
Say '1) Умолчание' '1) Default'
Say '2) Ввести свой путь' '2) Enter a custom path'
Say '3) Выбрать папку в проводнике' '3) Choose a folder in Explorer'
$pathChoice = Ask 'Ваш выбор [1/2/3]:' 'Choose [1/2/3]:'

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

$installDir = $installDir.Trim().Trim('"')
if ([string]::IsNullOrWhiteSpace($installDir)) { throw 'Installation path cannot be empty.' }
$installDir = [IO.Path]::GetFullPath($installDir)
if ($installDir.Contains('%') -or $installDir.Contains('"') -or $installDir.Contains("`n")) {
    Say 'Путь содержит символы, которые мешают созданию файлов запуска. Выберите другой путь.' 'The path contains characters that prevent creating launcher files. Choose another path.'
    exit 1
}
New-Item -ItemType Directory -Path $installDir -Force | Out-Null
$installDriveRoot = [IO.Path]::GetPathRoot($installDir)
try {
    $freeBytes = [System.IO.DriveInfo]::new($installDriveRoot).AvailableFreeSpace
} catch {
    Say "Не удалось проверить свободное место на диске $installDriveRoot. Проверьте его вручную перед продолжением." "Could not check free space on $installDriveRoot. Check it manually before continuing."
    throw
}
if ($freeBytes -lt 1GB) {
    Say "Недостаточно места: нужно минимум 1 ГБ свободного места на диске $installDriveRoot." "Not enough space: at least 1 GB must be free on $installDriveRoot."
    exit 1
}
if ($freeBytes -lt 5GB) {
    $freeGiB = [math]::Round($freeBytes / 1GB, 1)
    Say "Свободно только $freeGiB ГБ. Рекомендуется не менее 5 ГБ для сборки и тестов." "Only $freeGiB GB is free. At least 5 GB is recommended for building and testing."
    $continueWithLowSpace = Ask 'Продолжить всё равно? [д/н]:' 'Continue anyway? [y/N]:'
    if (($script:Language -eq 'en' -and $continueWithLowSpace -notmatch '^(y|yes)$') -or ($script:Language -eq 'ru' -and $continueWithLowSpace -notmatch '^(д|да|y|yes)$')) {
        Say 'Установка остановлена.' 'Installation cancelled.'
        exit 1
    }
}
$sourceDir = Join-Path $installDir '.source'
$buildDir = Join-Path $installDir '.build'

Refresh-Path
$winget = Get-Command winget.exe -ErrorAction SilentlyContinue
$cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmakeVersion = Get-CMakeVersion $cmake
$vsPath = Get-VisualStudioPath
$missing = @()
if (-not $cmakeVersion -or $cmakeVersion -lt [Version]'3.21') { $missing += 'CMake 3.21+' }
if (-not $vsPath) { $missing += 'Visual Studio 2022 C++ Build Tools' }

if ($missing.Count -gt 0) {
    if (-not $winget) {
        Say 'Не найден winget. Установите App Installer, CMake и Visual Studio 2022 C++ Build Tools, затем запустите скрипт снова.' 'winget was not found. Install App Installer, CMake, and Visual Studio 2022 C++ Build Tools, then run this script again.'
        exit 1
    }
    Say "Не найдены зависимости: $($missing -join ', ')." "Missing dependencies: $($missing -join ', ')."
    $installDependencies = Ask 'Установить их через winget? Это может занять время и потребовать подтверждения UAC [д/н]:' 'Install them with winget? This may take a while and may request UAC approval [y/N]:'
    if (($script:Language -eq 'en' -and $installDependencies -notmatch '^(y|yes)$') -or ($script:Language -eq 'ru' -and $installDependencies -notmatch '^(д|да|y|yes)$')) {
        Say 'Установка остановлена; системные пакеты не менялись.' 'Installation cancelled; no system packages were changed.'
        exit 1
    }

    if (-not $cmake) {
        & $winget.Source install --id Kitware.CMake --exact --source winget --accept-source-agreements --accept-package-agreements --silent
        if ($LASTEXITCODE -ne 0) { throw "winget failed to install CMake (exit code $LASTEXITCODE)." }
    } elseif (-not $cmakeVersion -or $cmakeVersion -lt [Version]'3.21') {
        & $winget.Source upgrade --id Kitware.CMake --exact --source winget --accept-source-agreements --accept-package-agreements --silent
        if ($LASTEXITCODE -ne 0) { throw 'CMake 3.21 or newer is required. Update CMake, then rerun this installer.' }
    }
    if (-not $vsPath) {
        & $winget.Source install --id Microsoft.VisualStudio.2022.BuildTools --exact --source winget --accept-source-agreements --accept-package-agreements --override '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
        if ($LASTEXITCODE -ne 0) { throw "winget failed to install Visual Studio C++ Build Tools (exit code $LASTEXITCODE)." }
    }
    Refresh-Path
    $cmake = Get-Command cmake.exe -ErrorAction SilentlyContinue
    $cmakeVersion = Get-CMakeVersion $cmake
    $vsPath = Get-VisualStudioPath
    if (-not $cmakeVersion -or $cmakeVersion -lt [Version]'3.21' -or -not $vsPath) { throw 'CMake 3.21+ and the Visual Studio C++ Build Tools are required. Update/install them, restart PowerShell, and try again.' }
}

if (-not (Test-Path -LiteralPath (Join-Path $sourceDir 'CMakeLists.txt'))) {
    if (Test-Path -LiteralPath $sourceDir) {
        throw "Source folder exists but has no CMakeLists.txt: $sourceDir. Move it aside and rerun the installer."
    }
    $localSource = $PSScriptRoot
    if (-not [string]::IsNullOrWhiteSpace($localSource) -and (Test-Path -LiteralPath (Join-Path $localSource 'CMakeLists.txt'))) {
        $sourceDir = $localSource
    } else {
        $tempRoot = Join-Path $installDir ".source-download-$PID"
        if (Test-Path -LiteralPath $tempRoot) { throw "Temporary download folder already exists: $tempRoot" }
        New-Item -ItemType Directory -Path $tempRoot | Out-Null
        $downloaded = $false
        try {
            foreach ($repo in @('Zevvoryn/Zevvoryn', 'xzz11222/Zevvoryn')) {
                try {
                    Say "Загружаю исходники из $repo ..." "Downloading sources from $repo ..."
                    $zip = Join-Path $tempRoot 'source.zip'
                    $extract = Join-Path $tempRoot 'extract'
                    Invoke-WebRequest -Uri "https://github.com/$repo/archive/refs/heads/main.zip" -OutFile $zip -UseBasicParsing
                    Expand-Archive -LiteralPath $zip -DestinationPath $extract -Force
                    $root = Get-ChildItem -LiteralPath $extract -Directory | Select-Object -First 1
                    if (-not $root -or -not (Test-Path -LiteralPath (Join-Path $root.FullName 'CMakeLists.txt'))) {
                        throw 'Downloaded archive does not contain CMakeLists.txt at its root.'
                    }
                    Move-Item -LiteralPath $root.FullName -Destination $sourceDir
                    $downloaded = $true
                    break
                } catch {
                    Say "Загрузка из $repo не удалась: $($_.Exception.Message)" "Download from $repo failed: $($_.Exception.Message)"
                }
            }
        } finally {
            Remove-Item -LiteralPath $tempRoot -Recurse -Force
        }
        if (-not $downloaded) { throw 'Could not download the source from either GitHub repository.' }
    }
}

$cmakeLists = Get-Content -LiteralPath (Join-Path $sourceDir 'CMakeLists.txt') -Raw
$targetMatch = [regex]::Match($cmakeLists, '(?im)^\s*add_executable\s*\(\s*([A-Za-z_][A-Za-z0-9_.+-]*)')
if (-not $targetMatch.Success) { throw 'Could not determine the executable target in CMakeLists.txt.' }
$targetName = $targetMatch.Groups[1].Value

Say 'Собираю Zevvoryn (Release, x64)...' 'Building Zevvoryn (Release, x64)...'
& $cmake.Source -S $sourceDir -B $buildDir -G 'Visual Studio 17 2022' -A x64 `
    "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE=$installDir" `
    '-DCMAKE_CONFIGURATION_TYPES=Release'
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit code $LASTEXITCODE)." }
& $cmake.Source --build $buildDir --config Release --target $targetName --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed (exit code $LASTEXITCODE)." }

$exe = Join-Path $installDir "$targetName.exe"
if (-not (Test-Path -LiteralPath $exe)) { throw "Build succeeded but executable was not found: $exe" }
$runLines = @('@echo off', 'setlocal', 'pushd "%~dp0"', "`"%~dp0$targetName.exe`" %*", 'set "serverExitCode=%ERRORLEVEL%"', 'popd', 'exit /b %serverExitCode%')
$compileLines = @('@echo off', "`"$($cmake.Source)`" --build `"$buildDir`" --config Release --target $targetName --parallel", 'exit /b %ERRORLEVEL%')
Set-Content -LiteralPath (Join-Path $installDir 'run_server.bat') -Value $runLines -Encoding ASCII
Set-Content -LiteralPath (Join-Path $installDir 'compile.bat') -Value $compileLines -Encoding ASCII

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

Say "Установка завершена: $installDir`nЗапуск: $installDir\run_server.bat`nПересборка: $installDir\compile.bat" "Installation complete: $installDir`nRun: $installDir\run_server.bat`nRebuild: $installDir\compile.bat"
Write-Host '===== ZEVVORYN ====='
