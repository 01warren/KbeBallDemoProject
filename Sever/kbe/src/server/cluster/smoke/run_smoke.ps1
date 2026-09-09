# run_smoke.ps1  --  单机三副本 cluster 冒烟 + 故障切换验证 (Windows)
#
# 前置:
#   1) 已构建 Release|Win32 的 cluster(输出到 <kbe>/bin/server/cluster.exe);
#   2) 已用 cl 编译 smoke_client(见下), 与本脚本同目录:
#        cl /nologo /EHsc /I<kbe>\src\server\cluster smoke_client.cpp /link ws2_32.lib
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File run_smoke.ps1 [-KeepRunning] [-Fast]
#
# 验证项:
#   S1 三副本启动并选出唯一 leader (query leader 在三个 servicePort 上一致)
#   S2 注册/续租/查询链路 (模拟组件 reg -> renew -> find -> queryall)
#   S3 TTL 下线 (模拟组件停止续租 6s 后从注册表移除)
#   S4 leader 故障切换 (kill leader, 剩余两副本选出新 leader, 新组件可注册)
#   S5 旧 leader 重新加入 (以原 cid 重启, 重新成为 follower 并同步注册表)

param(
    [switch]$KeepRunning,
    [switch]$Fast
)

$ErrorActionPreference = 'Stop'
$smokeDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$clusterDir = Split-Path -Parent $smokeDir            # kbe\src\server\cluster
$kbe = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $clusterDir))  # 4x .. -> kbe 根

$binServer = Join-Path $kbe 'bin\server'
$clusterExe = Join-Path $binServer 'cluster.exe'
$clientExe  = Join-Path $smokeDir 'smoke_client.exe'
$runDir     = Join-Path $smokeDir 'run'

if (!(Test-Path $clusterExe)) { Write-Host "[FATAL] $clusterExe not found"; exit 3 }
if (!(Test-Path $clientExe))  { Write-Host "[FATAL] $clientExe not found (compile smoke_client.cpp first)"; exit 3 }

# 每次从干净状态开始: 清除上一轮残留的 cluster_state_*.bin/日志/快照。
# (残留快照会让新 leader 在旧日志/注册表之上运行, 导致 reg 后紧跟的 renew/find 偶发失败;
#   run_tests.ps1 已有等价的 Clean-RunDir 语义, 冒烟保持一致。)
if (Test-Path $runDir) { Remove-Item -Recurse -Force $runDir }
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
foreach ($d in 'a','b','c') { New-Item -ItemType Directory -Force -Path (Join-Path $runDir $d) | Out-Null }

$hosts = '127.0.0.1','127.0.0.2','127.0.0.3'
$cids  = 30001, 30002, 30003
$servicePort = 20093

$procs = @{}   # name -> Process
$failed = 0

function Log  { param($m) Write-Host "[$(Get-Date -Format HH:mm:ss)] $m" }
function Check{ param($name,$ok,$detail) if($ok){ Write-Host "[PASS] $name" } else { Write-Host "[FAIL] $name : $detail"; $script:failed++ } }

function Start-Replica {
    param([int]$idx, [string]$tag, [string]$cidArg, [string]$logTag)
    $assets = Join-Path $smokeDir ("assets_" + $tag)
    $cwd    = Join-Path $runDir $tag
    $env:KBE_ROOT = $kbe
    $env:KBE_BIN_PATH = $binServer + '\'
    $env:KBE_RES_PATH = (Join-Path $kbe 'res') + ';' + $assets + ';' + (Join-Path $assets 'res')
    $outFile = Join-Path $runDir ($logTag + '.stdout.log')
    $errFile = Join-Path $runDir ($logTag + '.stderr.log')
    $p = Start-Process -FilePath $clusterExe -ArgumentList $cidArg -WorkingDirectory $cwd `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -WindowStyle Hidden
    $procs[$tag] = $p
    Log "started replica $tag (cid=$cidArg pid=$($p.Id)) log=$logTag"
}

function Stop-Replica {
    param([string]$tag)
    if ($procs.ContainsKey($tag) -and !$procs[$tag].HasExited) {
        try { Stop-Process -Id $procs[$tag].Id -Force -ErrorAction SilentlyContinue } catch {}
        $procs[$tag].WaitForExit(3000) | Out-Null
        Log "stopped replica $tag (pid was $($procs[$tag].Id))"
    }
}

function Invoke-Smoke {
    # client 的 stdout 只作日志, 不得混入返回值(否则 rc 变成 object[], 判定恒假)
    param([string]$hostIp, [string[]]$argsList)
    & $clientExe $hostIp $servicePort @argsList 2>&1 | Out-Null
    return $LASTEXITCODE
}

function Get-LeaderHost {
    # 对每个副本 query leader, 返回响应成功(自身是 leader)的主机
    foreach ($h in $hosts) {
        $rc = Invoke-Smoke $h @('leader')
        if ($rc -eq 0) { return $h }
    }
    return ''
}

function Wait-Leader {
    param([int]$maxSec)
    $deadline = (Get-Date).AddSeconds($maxSec)
    while ((Get-Date) -lt $deadline) {
        foreach ($h in $hosts) {
            $rc = Invoke-Smoke $h @('leader')
            if ($rc -eq 0) { return $h }
        }
        Start-Sleep -Milliseconds 500
    }
    return ''
}

function Replica-Alive {
    param([string]$h)
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $iar = $c.BeginConnect($h, $servicePort, $null, $null)
        $ok = $iar.AsyncWaitHandle.WaitOne(300)
        if ($ok) { $c.EndConnect($iar); $c.Close() }
        return $ok
    } catch { return $false }
}

function Wait-ReplicaDead {
    param([string]$h, [int]$maxSec)
    $deadline = (Get-Date).AddSeconds($maxSec)
    while ((Get-Date) -lt $deadline) {
        if (!(Replica-Alive $h)) { return $true }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

function Wait-QueryAllCount {
    param([string]$leaderHost, [string]$expect, [int]$maxSec)
    # smoke_client queryall prints "count=N"; leader 地址变更时重查
    $deadline = (Get-Date).AddSeconds($maxSec)
    while ((Get-Date) -lt $deadline) {
        $rc = Invoke-Smoke $leaderHost @('leader')
        $leader = if ($rc -eq 0) { $leaderHost } else { Get-LeaderHost }
        if ($leader) {
            $out = & $clientExe $leader $servicePort queryall 1 2>&1 | Out-String
            if ($out -match "count=\s*$expect") { return $leader }
        }
        Start-Sleep -Milliseconds 500
    }
    return ''
}

# ============================ S1: 启动与选主 ============================
Log 'S1: starting 3 replicas...'
Start-Replica 0 'a' '--cid=30001' 'a'
Start-Replica 1 'b' '--cid=30002' 'b'
Start-Replica 2 'c' '--cid=30003' 'c'

$leader = Wait-Leader 20
Check 'S1 cluster started & leader elected' ($leader -ne '') "no leader in 20s"
if (!$leader) { foreach ($t in 'a','b','c') { if ($procs[$t].HasExited) { Log "replica $t exited code=$($procs[$t].ExitCode)" } } ; if (!$KeepRunning) { foreach($t in 'a','b','c'){ Stop-Replica $t } }; exit 1 }
Log "S1 leader is $leader"

# ============================ S2: 注册/续租/查询 ============================
Log 'S2: register/renew/find/queryall via leader...'
$rc = Invoke-Smoke $leader @('reg','1','2','20001','43210','0')
Check 'S2 reg baseapp(uid=1 type=2 cid=20001) OK' ($rc -eq 0) "exit=$rc"

$rc = Invoke-Smoke $leader @('renew','1','2','20001','43210')
Check 'S2 renew baseapp OK' ($rc -eq 0) "exit=$rc"

$rc = Invoke-Smoke $leader @('find','1','2','20001')
Check 'S2 find baseapp by (uid,type,cid) OK' ($rc -eq 0) "exit=$rc"

# ============================ S3: TTL 下线 ============================
if ($Fast) {
    Log 'S3 skipped (-Fast)'
} else {
    Log 'S3: waiting lease=6s expiry for un-renewed component...'
    Start-Sleep -Seconds 8
    $rc = Invoke-Smoke $leader @('find','1','2','20001')
    Check 'S3 un-renewed component removed after TTL' ($rc -ne 0) "find still ok(exit=$rc), lease removal failed"
}

# ============================ S4: leader 故障切换 ============================
Log "S4: killing leader replica at $leader ..."
$leadTag = @{ '127.0.0.1'='a'; '127.0.0.2'='b'; '127.0.0.3'='c' }[$leader]
Stop-Replica $leadTag
$deadOk = Wait-ReplicaDead $leader 10
Check "S4 leader $leader down" $deadOk "still reachable after 10s"

$newLeader = Wait-Leader 15
Check 'S4 new leader elected among remaining replicas' ($newLeader -ne '') "no new leader in 15s"
if ($newLeader) {
    Check 'S4 new leader differs from dead one' ($newLeader -ne $leader) "same leader=$newLeader"
}

$rc = Invoke-Smoke $newLeader @('reg','1','3','30051','1000','0')
Check 'S4 register cellapp(uid=1 type=3 cid=30051) on new leader OK' ($rc -eq 0) "exit=$rc"
$rc = Invoke-Smoke $newLeader @('queryall','1')
Check 'S4 queryall via new leader OK' ($rc -eq 0) "exit=$rc"

# ============================ S5: 旧 leader 重新加入 ============================
# 说明: cluster 副本自身不写入组件注册表(设计如此), 因此不做 queryall 中找副本cid的断言;
#       验证的是重新加入后: ①单一leader不被干扰  ②该副本恢复为follower并把leader重定向
#       回现任leader  ③在现任leader上注册/查询仍一致。
Log 'S5: restarting old leader replica (same member index / cid)...'
$oldTag = $leadTag
$oldHost = @{ 'a'='127.0.0.1'; 'b'='127.0.0.2'; 'c'='127.0.0.3' }[$oldTag]
Start-Replica 0 $oldTag ('--cid=' + $cids[@{'a'=0;'b'=1;'c'=2}[$oldTag]]) ($oldTag + '_restart')
Start-Sleep -Seconds 4

$rc = Invoke-Smoke $newLeader @('leader')
Check 'S5 new leader not disturbed after rejoin' ($rc -eq 0) "leader lost after rejoin exit=$rc"

$out = & $clientExe $oldHost $servicePort leader 2>&1 | Out-String
Check 'S5 rejoined replica redirects to current leader' ($out -match 'NOT_LEADER' -and $out -match $newLeader) 'rejoined replica not following new leader'

$rc = Invoke-Smoke $newLeader @('reg','1','2','20077','7777','0')
Check 'S5 register on leader after rejoin OK' ($rc -eq 0) "exit=$rc"
$out = & $clientExe $newLeader $servicePort queryall 1 2>&1 | Out-String
Check 'S5 queryall lists newly registered component' ($out -match '20077') 'component missing in registry'

# ============================ 收尾 ============================
Log "results: $failed failure(s). logs under $runDir"
if (!$KeepRunning) { foreach ($t in 'a','b','c') { Stop-Replica $t } }

if ($failed -eq 0) { Write-Host '[SMOKE RESULT] ALL PASS'; exit 0 }
else               { Write-Host "[SMOKE RESULT] $failed FAILED"; exit 1 }
