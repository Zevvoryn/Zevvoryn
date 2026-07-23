$ProjectDir = "C:\Users\aaav9\Desktop\TestC++"
$StateDir = Join-Path $ProjectDir ".mcp-hidden"
$stopped = 0

foreach ($name in @("proxy.pid", "tunnel.pid")) {
    $pidFile = Join-Path $StateDir $name
    if (Test-Path $pidFile) {
        $savedPid = Get-Content $pidFile -ErrorAction SilentlyContinue
        if ($savedPid) {
            Stop-Process -Id ([int]$savedPid) -Force -ErrorAction SilentlyContinue
            $stopped++
        }
        Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
    }
}

$shell = New-Object -ComObject WScript.Shell
[void]$shell.Popup("MCP stopped. Processes closed: $stopped", 5, "MCP", 64)
