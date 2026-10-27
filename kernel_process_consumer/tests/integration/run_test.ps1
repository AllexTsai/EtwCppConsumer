param([switch]$CreateLeftover, [switch]$RedirectStdin, [string[]]$ExeArgs = @())

$sp   = Split-Path -Parent $MyInvocation.MyCommand.Path
# 腳本位於 <專案>\tests\integration，往上兩層是專案根目錄（kernel_process_consumer）
$root      = Split-Path -Parent (Split-Path -Parent $sp)
$buildDir  = Join-Path $root 'build'
$exe       = Join-Path $buildDir 'kernel_process_consumer.exe'
$sendctrlc = Join-Path $buildDir 'sendctrlc.exe'
$missing = @($exe, $sendctrlc) | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }
if ($missing) {
    foreach ($m in $missing) { "ERROR: required executable not found: $m" }
    exit 1
}
$outDir = Join-Path $buildDir 'integration_out'
if (-not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$out  = Join-Path $outDir 'consumer_out.txt'
$err  = Join-Path $outDir 'consumer_err.txt'
$name = 'EtwCppConsumer-KernelProcess'

if ($CreateLeftover) {
    "===== create leftover session with logman ====="
    logman start $name -ets -rt
    logman query -ets | Select-String $name
}

"===== start consumer ====="
$spArgs = @{ FilePath = $exe; PassThru = $true; WindowStyle = 'Minimized'
             RedirectStandardOutput = $out; RedirectStandardError = $err }
if ($ExeArgs.Count -gt 0) { $spArgs.ArgumentList = $ExeArgs }
if ($RedirectStdin) {
    $in = Join-Path $sp 'empty_stdin.txt'
    if (-not (Test-Path -LiteralPath $in)) { Set-Content -Path $in -Value '' -Encoding ascii }
    $spArgs.RedirectStandardInput = $in   # stdin 不是 console，GetConsoleMode 應該失敗
}
$p = Start-Process @spArgs
$null = $p.Handle   # 先取得 handle，否則 Start-Process 物件結束後讀不到 ExitCode
"consumer PID = $($p.Id)"
Start-Sleep -Seconds 3

"===== spawn test processes ====="
$c1 = Start-Process cmd -ArgumentList '/c', 'exit' -PassThru -WindowStyle Hidden
$c2 = Start-Process cmd -ArgumentList '/c', 'ping -n 2 127.0.0.1 >nul' -PassThru -WindowStyle Hidden
"spawned cmd PIDs = $($c1.Id), $($c2.Id)   (spawned by this PowerShell PID = $PID)"
$c1.WaitForExit(); $c2.WaitForExit()
Start-Sleep -Seconds 3

"===== send Ctrl+C ====="
& $sendctrlc $p.Id
"sendctrlc exit code = $LASTEXITCODE"

if ($p.WaitForExit(15000)) { "consumer exited, exit code = $($p.ExitCode)" }
else { "consumer did NOT exit within 15s"; Stop-Process -Id $p.Id -Force }

"===== consumer stdout ====="
Get-Content $out -Encoding UTF8
"===== consumer stderr ====="
Get-Content $err -Encoding UTF8

"===== logman query -ets (filtered) ====="
$r = logman query -ets | Select-String $name
if ($r) { $r } else { "(no session named $name)" }
