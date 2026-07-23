$ErrorActionPreference = "Stop"

$ProjectDir = "C:\Users\aaav9\Desktop\TestC++"
$SshKey = Join-Path $env:USERPROFILE ".ssh\id_ed25519"
$StateDir = Join-Path $ProjectDir ".mcp-hidden"
$ProxyOut = Join-Path $StateDir "proxy-out.log"
$ProxyErr = Join-Path $StateDir "proxy-error.log"
$TunnelOut = Join-Path $StateDir "tunnel-out.log"
$TunnelErr = Join-Path $StateDir "tunnel-error.log"
$ProxyPidFile = Join-Path $StateDir "proxy.pid"
$TunnelPidFile = Join-Path $StateDir "tunnel.pid"

New-Item -ItemType Directory -Force -Path $StateDir | Out-Null
Remove-Item $ProxyOut, $ProxyErr, $TunnelOut, $TunnelErr -Force -ErrorAction SilentlyContinue

function Show-Popup([string]$Message, [int]$Seconds, [int]$Icon = 64) {
    $shell = New-Object -ComObject WScript.Shell
    [void]$shell.Popup($Message, $Seconds, "MCP", $Icon)
}

try {
    if (-not (Test-Path $SshKey)) {
        throw "SSH key not found: $SshKey"
    }

    $portBusy = Get-NetTCPConnection -LocalPort 3000 -State Listen -ErrorAction SilentlyContinue
    if ($portBusy) {
        throw "Port 3000 is already busy. Close node server.js or the old MCP Proxy first."
    }

    $proxyArgs = @(
        "-y", "mcp-proxy",
        "--port", "3000",
        "--shell", "--",
        "npx", "-y", "@modelcontextprotocol/server-filesystem", $ProjectDir
    )

    $proxy = Start-Process -FilePath "npx.cmd" -ArgumentList $proxyArgs -WorkingDirectory $ProjectDir -WindowStyle Hidden -RedirectStandardOutput $ProxyOut -RedirectStandardError $ProxyErr -PassThru
    Set-Content -Path $ProxyPidFile -Value $proxy.Id -Encoding ASCII

    Start-Sleep -Seconds 4
    if ($proxy.HasExited) {
        $details = (Get-Content $ProxyErr -Raw -ErrorAction SilentlyContinue)
        throw "MCP Proxy stopped: $details"
    }

    $sshArgs = @(
        "-i", $SshKey,
        "-o", "ServerAliveInterval=30",
        "-o", "ServerAliveCountMax=3",
        "-o", "ExitOnForwardFailure=yes",
        "-R", "80:127.0.0.1:3000",
        "nocmdfiles@localhost.run"
    )

    $tunnel = Start-Process -FilePath "ssh.exe" -ArgumentList $sshArgs -WindowStyle Hidden -RedirectStandardOutput $TunnelOut -RedirectStandardError $TunnelErr -PassThru
    Set-Content -Path $TunnelPidFile -Value $tunnel.Id -Encoding ASCII

    $url = $null
    for ($i = 0; $i -lt 30 -and -not $url; $i++) {
        Start-Sleep -Seconds 1
        $allText = ""
        if (Test-Path $TunnelOut) { $allText += Get-Content $TunnelOut -Raw -ErrorAction SilentlyContinue }
        if (Test-Path $TunnelErr) { $allText += "`n" + (Get-Content $TunnelErr -Raw -ErrorAction SilentlyContinue) }
        $match = [regex]::Match($allText, 'https://[a-zA-Z0-9.-]+\.lhr\.life')
        if ($match.Success) { $url = $match.Value }
        if ($tunnel.HasExited) { break }
    }

    if ($url) {
        Set-Clipboard -Value $url
        Show-Popup "MCP is running.`n`n$url`n`nThe link was copied to the clipboard.`nThis window will close in 15 seconds." 15 64
    } else {
        $details = (Get-Content $TunnelErr -Raw -ErrorAction SilentlyContinue)
        Show-Popup "Could not get the tunnel link.`n`n$details`n`nLogs: $StateDir" 15 48
    }
} catch {
    Show-Popup "Startup error:`n`n$($_.Exception.Message)" 15 16
}
