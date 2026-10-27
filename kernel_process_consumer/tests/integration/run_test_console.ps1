# stdin 保留為真正的 console：用 cmd 在新 console 執行，只把 stdout 導到檔案
$sp      = Split-Path -Parent $MyInvocation.MyCommand.Path
# 腳本位於 <專案>\tests\integration，往上兩層是專案根目錄（kernel_process_consumer）
$root      = Split-Path -Parent (Split-Path -Parent $sp)
$buildDir  = Join-Path $root 'build'
$exe       = Join-Path $buildDir 'kernel_process_consumer.exe'
$getmode   = Join-Path $buildDir 'getmode.exe'
$sendctrlc = Join-Path $buildDir 'sendctrlc.exe'
$missing = @($exe, $getmode, $sendctrlc) | Where-Object { -not (Test-Path -LiteralPath $_ -PathType Leaf) }
if ($missing) {
    foreach ($m in $missing) { "ERROR: required executable not found: $m" }
    exit 1
}
$outDir  = Join-Path $buildDir 'integration_out'
if (-not (Test-Path -LiteralPath $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }
$out     = Join-Path $outDir 'consumer_console_out.txt'
$modes   = Join-Path $outDir 'modes.txt'
$name    = 'EtwCppConsumer-KernelProcess'
Remove-Item $out, $modes -ErrorAction SilentlyContinue

$cmdLine = "/c `"`"$getmode`" before >> `"$modes`" & `"$exe`" > `"$out`" 2>&1 & `"$getmode`" after >> `"$modes`"`""
"===== start cmd in new console ====="
$cmd = Start-Process cmd -ArgumentList $cmdLine -PassThru -WindowStyle Minimized
$null = $cmd.Handle
Start-Sleep -Seconds 3
$consumer = Get-CimInstance Win32_Process -Filter "Name='kernel_process_consumer.exe' AND ParentProcessId=$($cmd.Id)"
"cmd PID = $($cmd.Id), consumer PID = $($consumer.ProcessId)"

"===== spawn one test process ====="
$c1 = Start-Process cmd -ArgumentList '/c', 'exit' -PassThru -WindowStyle Hidden
"spawned cmd PID = $($c1.Id)"
$c1.WaitForExit()
Start-Sleep -Seconds 2

"===== send Ctrl+C ====="
& $sendctrlc $consumer.ProcessId
"sendctrlc exit code = $LASTEXITCODE"
if ($cmd.WaitForExit(15000)) { "cmd exited, exit code = $($cmd.ExitCode)" } else { "cmd did NOT exit within 15s"; Stop-Process -Id $cmd.Id -Force }

"===== modes.txt (getmode before/after) ====="
Get-Content $modes
"===== consumer output ====="
Get-Content $out -Encoding UTF8
"===== logman query -ets (filtered) ====="
$r = logman query -ets | Select-String $name
if ($r) { $r } else { "(no session named $name)" }
