<#
.SYNOPSIS
    比對 started_pids.txt 的 PID 是否都以 ProcessStart 事件出現在 consumer 的輸出中。

.DESCRIPTION
    比對的是事件「內容」裡的 ProcessID（"  ProcessID  [InType  8] = 1234" 那一行），
    不是標頭的 Header.ProcessId（ProcessStart 的標頭 PID 是建立者，也就是父行程）。

    ProcessStart 的判斷條件：EventId=1 且 Task 名稱為 ProcessStart。
    兩者不一致時會發出警告。

.PARAMETER PidsFile
    每行一個十進位 PID 的文字檔。空白行會略過，其他無法解析的行會警告。

.PARAMETER LogFile
    kernel_process_consumer.exe 的輸出檔。Get-Content 會依 BOM 自動判斷編碼
    （PowerShell 5.1 用 > 重導時產生 UTF-16LE + BOM）。

.OUTPUTS
    結束碼：0 = 全部找到，1 = 有 PID 沒找到，2 = 輸入檔錯誤。

.EXAMPLE
    .\verify_pids.ps1
    .\verify_pids.ps1 -PidsFile .\started_pids.txt -LogFile .\run1.txt
#>
param(
    [string]$PidsFile = 'started_pids.txt',
    [string]$LogFile  = 'run1.txt'
)

$ErrorActionPreference = 'Stop'

foreach ($f in @($PidsFile, $LogFile)) {
    if (-not (Test-Path -LiteralPath $f -PathType Leaf)) {
        Write-Host "ERROR: file not found: $f"
        exit 2
    }
}

# ---------------------------------------------------------------------------
# 1. 讀 PID 清單
# ---------------------------------------------------------------------------
# PID 一律正規化成十進位字串（去掉前導 0），作為強型別集合的鍵
$pidList = New-Object 'System.Collections.Generic.List[string]'
$badLines = 0
$lineNo = 0
foreach ($line in Get-Content -LiteralPath $PidsFile) {
    $lineNo++
    $t = $line.Trim()
    if ($t -eq '') { continue }
    $value = [uint32]0
    if ([uint32]::TryParse($t, [ref]$value)) {
        $pidList.Add($value.ToString())
    } else {
        $badLines++
        Write-Host "WARNING: ${PidsFile}:$lineNo is not a PID: '$t'"
    }
}
# 保留檔案中的原始順序，去除重複
$uniquePids = New-Object 'System.Collections.Generic.List[string]'
$seenPids = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($p in $pidList) {
    if ($seenPids.Add($p)) { $uniquePids.Add($p) }
}

# ---------------------------------------------------------------------------
# 2. 解析 consumer 輸出：逐個事件區塊取出 EventId、Task 名稱、內容 ProcessID
# ---------------------------------------------------------------------------
$reSeq       = '^-{5,} seq=(\d+) -{5,}$'
$reEventId   = '^EventId=(\d+)\s.*\bTask=\d+\(([^)]*)\)'
# 只匹配名稱剛好是 ProcessID 的屬性行（有縮排、名稱後接 [InType）；
# 不會匹配 ParentProcessID，也不會匹配 "Header.ProcessId=..."（沒有縮排、大小寫不同）
$rePayloadPid = '^\s+ProcessID\s+\[InType\s+\d+\]\s+=\s+(\d+)\s*$'

$sawSummary = $false

# 先把每個事件區塊收集起來，迴圈結束後再分類
$eventList = New-Object 'System.Collections.Generic.List[object]'
$cur = $null
foreach ($line in Get-Content -LiteralPath $LogFile) {
    if ($line -match $reSeq) {
        $cur = [pscustomobject]@{ Seq = $Matches[1]; EventId = -1; Task = ''; PayloadPid = '' }
        $eventList.Add($cur)
        continue
    }
    if ($line -like 'Summary:*') { $sawSummary = $true }
    if ($null -eq $cur) { continue }
    if ($line -match $reEventId) {
        $cur.EventId = [int]$Matches[1]
        $cur.Task = $Matches[2]
    } elseif ($cur.PayloadPid -eq '' -and $line -match $rePayloadPid) {
        $cur.PayloadPid = ([uint32]$Matches[1]).ToString()
    }
}

# 內容 ProcessID -> 出現在哪些 seq
$startPids = New-Object 'System.Collections.Generic.Dictionary[string,System.Collections.Generic.List[string]]'
$stopPids  = New-Object 'System.Collections.Generic.Dictionary[string,System.Collections.Generic.List[string]]'
$startEvents = 0
$inconsistent = 0
$noPayloadPid = 0
foreach ($e in $eventList) {
    if ($e.PayloadPid -eq '') { $noPayloadPid++; continue }
    $isStartById   = ($e.EventId -eq 1)
    $isStartByTask = ($e.Task -eq 'ProcessStart')
    if ($isStartById -ne $isStartByTask) {
        $inconsistent++
        Write-Host "WARNING: seq=$($e.Seq) EventId=$($e.EventId) Task=$($e.Task) disagree; not counted as ProcessStart"
        continue
    }
    if ($isStartById) {
        $startEvents++
        $target = $startPids
    } elseif ($e.EventId -eq 2) {
        $target = $stopPids
    } else {
        continue
    }
    if (-not $target.ContainsKey($e.PayloadPid)) {
        $target[$e.PayloadPid] = New-Object 'System.Collections.Generic.List[string]'
    }
    $target[$e.PayloadPid].Add($e.Seq)
}
$events = $eventList.Count

# ---------------------------------------------------------------------------
# 3. 比對與輸出
# ---------------------------------------------------------------------------
$found   = New-Object 'System.Collections.Generic.List[string]'
$missing = New-Object 'System.Collections.Generic.List[string]'
foreach ($p in $uniquePids) {
    if ($startPids.ContainsKey($p)) { $found.Add($p) } else { $missing.Add($p) }
}

Write-Host "PID file : $((Resolve-Path -LiteralPath $PidsFile).Path)"
Write-Host "Log file : $((Resolve-Path -LiteralPath $LogFile).Path)"
Write-Host "Log events parsed: $events (ProcessStart: $startEvents, distinct ProcessStart payload PIDs: $($startPids.Count))"
if (-not $sawSummary) {
    Write-Host "WARNING: no 'Summary:' line in log; the consumer output may be truncated"
}
if ($inconsistent -gt 0) {
    Write-Host "WARNING: $inconsistent event(s) with inconsistent EventId/Task"
}
if ($noPayloadPid -gt 0) {
    Write-Host "WARNING: $noPayloadPid event(s) without a payload ProcessID line"
}
Write-Host ""
Write-Host "Total PIDs in ${PidsFile}: $($pidList.Count) (unique: $($uniquePids.Count), invalid lines: $badLines)"
Write-Host "Found as ProcessStart payload ProcessID: $($found.Count)"
Write-Host "Missing: $($missing.Count)"
foreach ($m in $missing) {
    $note = if ($stopPids.ContainsKey($m)) { "appears only as ProcessStop (seq $($stopPids[$m] -join ','))" } else { 'not in any ProcessStart/ProcessStop payload' }
    Write-Host ("  {0,-8} {1}" -f $m, $note)
}

if ($missing.Count -gt 0) { exit 1 }
exit 0
