# RTT 采集：起 OpenOCD（记录 PID，只杀自己起的进程）→ 读 20 秒 RTT → 精确清理
# 位置：tools/rtt/（2026-10-26 从 build/ 迁出——build/ 被 gitignore，make clean 会连脚本一起删）
$ErrorActionPreference = 'Stop'
Set-Location (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
New-Item -ItemType Directory -Force -Path 'build' | Out-Null   # 日志落 build/；clean 后目录可能不存在

$ocd = Start-Process -FilePath 'C:\openocd\xpack-openocd-0.12.0-7\bin\openocd.exe' `
    -ArgumentList '-f','openocd.cfg','-f','tools\rtt\rtt.cfg' `
    -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput 'build\rtt_ocd.log' -RedirectStandardError 'build\rtt_ocd_err.log'

try {
    Start-Sleep -Seconds 3
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', 19021)
    $stream = $client.GetStream()
    $stream.ReadTimeout = 5000
    $buf = New-Object byte[] 4096
    $sb = New-Object System.Text.StringBuilder
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        try {
            $n = $stream.Read($buf, 0, $buf.Length)
            if ($n -gt 0) { [void]$sb.Append([System.Text.Encoding]::UTF8.GetString($buf, 0, $n)) }
        } catch [System.IO.IOException] { break }
    }
    $client.Close()
    Write-Output $sb.ToString()
}
finally {
    if (-not $ocd.HasExited) { Stop-Process -Id $ocd.Id -Force }
    if ($ocd.HasExited) { Get-Content 'build\rtt_ocd_err.log' -Tail 5 -ErrorAction SilentlyContinue }
}
