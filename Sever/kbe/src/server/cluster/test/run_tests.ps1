# run_tests.ps1  --  cluster 测试工程主执行器 (Windows)
#
# 前置:
#   1) 已构建 cluster.exe(Release|Win32, 输出到 <kbe>/bin/server/cluster.exe);
#   2) 已构建测试工具(见 _build_tests.bat): test_client/snapshot_gen/dummy_proc 与本脚本同目录。
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File run_tests.ps1 [-Fast] [-Full] [-KeepRunning]
#     -Fast : 跳过慢速 TTL 用例(约省 20s)
#     -Full : 追加"多数派丢失 -> 拒绝写 -> 恢复"慢用例
#     -KeepRunning : 结束后保留副本进程(排查用)
#
# 覆盖的用例组(与 TESTCASES.md 一一对应):
#   G1  单副本链路+写应答即时性回归(registerWaiter 修复) / cid 冲突 / TTL / ctl 起停
#   G2  快照文件加载回归(loadSnapshotFromDisk 4 字节偏移修复) + 损坏快照容忍
#   G3  三副本: 选主唯一 / follower 重定向 / follower 掉线写提交 / leader 故障切换 / 旧 leader 重入
#   G4  (-Full) 多数派丢失拒绝写 / 恢复后重新服务
#
# 测试资产独立端口 20193/20194, 与冒烟(20093/20094)互不干扰。

param(
    [switch]$Fast,
    [switch]$Full,
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- 路径
$testDir  = Split-Path -Parent $MyInvocation.MyCommand.Path          # .../cluster/test
$clusterDir = Split-Path -Parent $testDir                            # .../cluster
$kbe      = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $clusterDir))  # 4-> kbe
$binServer = Join-Path $kbe 'bin\server'
$assetsDir = Join-Path $testDir 'assets'
$runDir    = Join-Path $testDir 'run'

$clusterExe = Join-Path $binServer 'cluster.exe'
$clientExe  = Join-Path $testDir 'test_client.exe'
$snapGenExe = Join-Path $testDir 'snapshot_gen.exe'
$dummyExe   = Join-Path $testDir 'dummy_proc.exe'

foreach ($p in @($clusterExe, $clientExe, $snapGenExe, $dummyExe)) {
    if (!(Test-Path $p)) { Write-Host "[FATAL] missing $p (build first)"; exit 3 }
}

$servicePort = 20193

# 副本目录表: tag -> (资产子目录, --cid, 绑定的回环 IP)
$cfg = @{
    'single' = @{ assets = 'single';      cid = 33001; host = '127.0.0.1' }
    'snap'   = @{ assets = 'single_long'; cid = 33002; host = '127.0.0.1' }
    'a'      = @{ assets = 'a';           cid = 31001; host = '127.0.0.1' }
    'b'      = @{ assets = 'b';           cid = 31002; host = '127.0.0.2' }
    'c'      = @{ assets = 'c';           cid = 31003; host = '127.0.0.3' }
}
$script:procs = @{}       # tag -> Process
$script:passed = 0
$script:failed = 0

# ---------------------------------------------------------------- 基础工具
function Log  { param($m) Write-Host "[$(Get-Date -Format HH:mm:ss)] $m" }
function Check { param($name, $ok, $detail)
    if ($ok) { Write-Host "[PASS] $name"; $script:passed++ }
    else     { Write-Host "[FAIL] $name : $detail"; $script:failed++ }
}

# 执行 test_client, 结果写入 $script:tcRc/$script:tcOut/$script:tcElapsed
function Send-TC {
    # 结果写入 $script:tcRc/$script:tcOut/$script:tcElapsed; 不向管道输出
    param([string]$HostIp, [string[]]$ArgsList, [int]$Timeout = 8000)
    $all = @($ArgsList) + @("--timeout=$Timeout")
    $o = & $script:clientExe $HostIp $script:servicePort @all 2>&1
    $script:tcRc = $LASTEXITCODE
    $script:tcOut = ($o | Out-String)
    $m = [regex]::Match($script:tcOut, '(?m)^ELAPSED_MS=(\d+)')
    $script:tcElapsed = if ($m.Success) { [int]$m.Groups[1].Value } else { -1 }
    $script:tcResp = (Get-O 'RESP')
}

function Get-O {
    param([string]$key)
    $m = [regex]::Match($script:tcOut, "(?m)^$([regex]::Escape($key))=(.*)$")
    if ($m.Success) { return $m.Groups[1].Value.Trim() } else { return '' }
}

# ---------------------------------------------------------------- 进程管理
function Start-Replica {
    param([string]$tag)
    $c = $cfg[$tag]
    $assets = Join-Path $assetsDir $c.assets
    $cwd    = Join-Path $runDir $tag
    New-Item -ItemType Directory -Force -Path $cwd | Out-Null

    $env:KBE_ROOT     = $kbe
    $env:KBE_BIN_PATH = $binServer + '\'
    $env:KBE_RES_PATH = (Join-Path $kbe 'res') + ';' + $assets + ';' + (Join-Path $assets 'res')

    $outFile = Join-Path $runDir ($tag + '.stdout.log')
    $errFile = Join-Path $runDir ($tag + '.stderr.log')
    $p = Start-Process -FilePath $clusterExe -ArgumentList @("--cid=$($c.cid)") `
        -WorkingDirectory $cwd -RedirectStandardOutput $outFile -RedirectStandardError $errFile `
        -PassThru -WindowStyle Hidden
    $script:procs[$tag] = $p
    Log "started replica $tag (cid=$($c.cid) host=$($c.host) pid=$($p.Id))"
}

function Stop-Replica {
    param([string]$tag)
    if ($script:procs.ContainsKey($tag) -and !$script:procs[$tag].HasExited) {
        try { Stop-Process -Id $script:procs[$tag].Id -Force -ErrorAction SilentlyContinue } catch {}
        $script:procs[$tag].WaitForExit(3000) | Out-Null
        Log "stopped replica $tag (pid was $($script:procs[$tag].Id))"
    }
}

function Stop-All {
    foreach ($tag in @($script:procs.Keys)) { Stop-Replica $tag }
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

function Wait-ReplicaDead { param([string]$h, [int]$maxSec)
    $dl = (Get-Date).AddSeconds($maxSec)
    while ((Get-Date) -lt $dl) { if (!(Replica-Alive $h)) { return $true }; Start-Sleep -Milliseconds 300 }
    return $false
}

function Wait-PidGone { param([int]$procId, [int]$maxSec)
    $dl = (Get-Date).AddSeconds($maxSec)
    while ((Get-Date) -lt $dl) {
        if (-not (Get-Process -Id $procId -ErrorAction SilentlyContinue)) { return $true }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

# 返回当前应答"我是 leader"的 tag; 无则空串
function Get-LeaderTag {
    param([string[]]$tags)
    foreach ($t in $tags) {
        $c = $cfg[$t]
        Send-TC $c.host @('leader') 2000 | Out-Null
        if ($script:tcRc -eq 0) { return $t }
    }
    return ''
}

function Wait-LeaderTag {
    param([string[]]$tags, [int]$maxSec)
    $dl = (Get-Date).AddSeconds($maxSec)
    while ((Get-Date) -lt $dl) {
        $t = Get-LeaderTag $tags
        if ($t) { return $t }
        Start-Sleep -Milliseconds 500
    }
    return ''
}

# 轮询 find 精确查询直到命中/未命中期望状态; 返回达到期望的 host 或 ''
function Wait-FindState {
    param([string]$hostIp, [int]$uid, [int]$type, [uint64]$cid, [bool]$wantFound, [int]$maxSec)
    $dl = (Get-Date).AddSeconds($maxSec)
    while ((Get-Date) -lt $dl) {
        Send-TC $hostIp @('find', "$uid", "$type", "$cid") 2000 | Out-Null
        $isFound = ($script:tcRc -eq 0)
        if ($isFound -eq $wantFound) { return $hostIp }
        Start-Sleep -Milliseconds 500
    }
    return ''
}

# 轮询 queryall 的 COUNT 是否 >= n; 返回达到的 host 或 ''
function Wait-QueryAllGE {
    param([string]$hostIp, [int]$uid, [int]$n, [int]$maxSec)
    $dl = (Get-Date).AddSeconds($maxSec)
    while ((Get-Date) -lt $dl) {
        Send-TC $hostIp @('queryall', "$uid") 2000 | Out-Null
        $cntS = Get-O 'COUNT'
        if ($cntS -ne '' -and [int]$cntS -ge $n) { return $hostIp }
        Start-Sleep -Milliseconds 500
    }
    return ''
}

function Clean-RunDir {
    # 删除整个 run 目录, 保证状态文件/日志从零开始
    if (Test-Path $runDir) { Remove-Item -Recurse -Force $runDir }
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null
}

function Clean-TagDir {
    # 清空某副本的运行目录(状态文件 cluster_state_*.bin 残留会污染下一用例组基线)
    param([string]$tag)
    $cwd = Join-Path $runDir $tag
    if (Test-Path $cwd) { Remove-Item -Recurse -Force $cwd }
    New-Item -ItemType Directory -Force -Path $cwd | Out-Null
}

# ================================================================ G1 单副本链路
function Test-Group-Single {
    Log '=== G1: single-member registry/consistency/ctl (asset: single) ==='
    Clean-TagDir 'single'
    Start-Replica 'single'
    $h = $cfg['single'].host
    $lt = Wait-LeaderTag @('single') 15
    Check 'G1-0 single replica becomes leader' ($lt -eq 'single') "no leader in 15s"

    # G1-1 写应答即时性回归(registerWaiter: 提交先于 waiter 登记时必须补应答)
    Send-TC $h @('reg','1','6','40001','12345') 4000
    $resp = Get-O 'RESP'
    $el = $script:tcElapsed
    Check 'G1-1 register returns MSG_RESP_OK immediately (<2500ms)' ($script:tcRc -eq 0 -and $resp -eq 'MSG_RESP_OK' -and $el -ge 0 -and $el -lt 2500) "rc=$($script:tcRc) resp=$resp elapsed=$el"

    # G1-2 查询链路
    Send-TC $h @('renew','1','6','40001','12345') 3000
    Check 'G1-2 renew OK' ($script:tcRc -eq 0 -and (Get-O 'RESP') -eq 'MSG_RESP_OK') "rc=$($script:tcRc) out=$($script:tcOut)"

    Send-TC $h @('find','1','6','40001') 3000
    Check 'G1-3 find exact cid returns ENTRY' ($script:tcRc -eq 0 -and (Get-O 'RESP') -eq 'MSG_RESP_ENTRY' -and (Get-O 'CID') -eq '40001') "rc=$($script:tcRc) resp=$(Get-O 'RESP')"

    Send-TC $h @('find','1','6','0') 3000
    Check 'G1-4 find by type returns list containing 40001' ($script:tcRc -eq 0 -and ($script:tcOut -match '40001')) "rc=$($script:tcRc) out=$($script:tcOut)"

    Send-TC $h @('queryall','1') 3000
    $cnt = Get-O 'COUNT'
    Check 'G1-5 queryall count>=1' ($script:tcRc -eq 0 -and $cnt -ne '' -and [int]$cnt -ge 1) "count=$cnt"

    # G1-6 cid 冲突
    Send-TC $h @('reg','1','6','40001','99999') 3000
    Check 'G1-6 duplicate (uid,type,cid) rejected with IDENTITY_CONFLICT' ($script:tcRc -ne 0 -and (Get-O 'RESP') -eq 'MSG_RESP_IDENTITY_CONFLICT') "rc=$($script:tcRc) resp=$(Get-O 'RESP')"

    # 不同 uid 允许复用相同 type+cid
    Send-TC $h @('reg','2','6','40001','11111') 3000
    Check 'G1-7 same type+cid under different uid allowed' ($script:tcRc -eq 0 -and (Get-O 'RESP') -eq 'MSG_RESP_OK') "rc=$($script:tcRc)"

    if (!$Fast) {
        # G1-8 续租保持
        Send-TC $h @('reg','1','5','40002','54321') 3000 | Out-Null
        Check 'G1-8a reg second component' ($script:tcRc -eq 0) "rc=$($script:tcRc)"
        $kept = $true
        for ($i = 0; $i -lt 7; $i++) {
            Send-TC $h @('renew','1','6','40001','12345') 3000 | Out-Null
            if ($script:tcRc -ne 0) { $kept = $false; break }
            Send-TC $h @('renew','1','5','40002','54321') 3000 | Out-Null
            if ($script:tcRc -ne 0) { $kept = $false; break }
            if ($i -eq 3) {
                Send-TC $h @('find','1','5','40002') 3000 | Out-Null
                if ($script:tcRc -ne 0) { $kept = $false; break }
            }
            Start-Sleep -Milliseconds 1100
        }
        Check 'G1-8 renews keep entries alive (TTL lease held)' $kept "renew chain broke at loop i=$i"

        # G1-9 TTL 过期
        $gone = Wait-FindState $h 1 6 40001 $false 12
        Check 'G1-9 un-renewed component removed after lease expiry' ($gone -ne '') '40001 still found after 12s'
        $gone2 = Wait-FindState $h 1 5 40002 $false 12
        Check 'G1-10 second component also expired' ($gone2 -ne '') '40002 still found after 12s'
    } else {
        Log 'G1-8/9/10 TTL cases skipped (-Fast)'
    }

    # ---- ctl 负路径 ----
    Send-TC $h @('ctl','start','1','15','0','0') 3000
    $code = Get-O 'CODE'
    Check 'G1-11 ctl start invalid type(15=CLUSTER) rejected' ($script:tcRc -ne 0 -and $code -eq '-1' -and ($script:tcOut -match 'invalid')) "rc=$($script:tcRc) code=$code msg=$(Get-O 'MSG')"

    # G1-12 负路径: watcher 类型合法但引擎从不产出独立 watcher.exe(内嵌于各 App),
    # 因此无论是否已全量构建引擎, 该用例都稳定验证“二进制缺失 -> code=-1”语义。
    Send-TC $h @('ctl','start','1','12','0','0') 4000
    $code = Get-O 'CODE'
    Check 'G1-12 ctl start watcher(missing binary) returns code=-1' ($script:tcRc -ne 0 -and $code -eq '-1') "rc=$($script:tcRc) code=$code msg=$(Get-O 'MSG')"

    Send-TC $h @('ctl','stop','1','3','0','0') 3000
    $code = Get-O 'CODE'
    Check 'G1-13 ctl stop with no running instance returns code=0(no-op)' ($script:tcRc -eq 0 -and $code -eq '0' -and ($script:tcOut -match 'no running')) "rc=$($script:tcRc) code=$code msg=$(Get-O 'MSG')"

    # ---- ctl kill 正路径(用真实进程验证本机代理执行) ----
    $d1out = Join-Path $runDir 'dummy1.out'
    $d1 = Start-Process -FilePath $dummyExe -RedirectStandardOutput $d1out -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 500
    Send-TC $h @('reg','1','6','40100',"$($d1.Id)") 3000 | Out-Null
    Send-TC $h @('ctl','kill','1','6','40100','0') 8000
    $code = Get-O 'CODE'
    $killed = Wait-PidGone $d1.Id 8
    Check 'G1-14 ctl kill terminates real process (code=0 & pid gone)' ($script:tcRc -eq 0 -and $code -eq '0' -and $killed) "rc=$($script:tcRc) code=$code pidGone=$killed"
    Send-TC $h @('unreg','1','6','40100') 2000 | Out-Null

    # ---- ctl stop(优雅路径)。taskkill 不带 /f 依赖交互式窗口站(WM_CLOSE);
    #      非交互会话下进程无法优雅关闭, cluster 必须返回明确错误码并带原因,
    #      两种结果都属正确语义: 交互式 -> code=0 且进程退出;
    #      无窗口站 -> code=-1 且消息含 taskkill/失败原因, 由 runner 兜底强杀。 ----
    $d2out = Join-Path $runDir 'dummy2.out'
    $d2 = Start-Process -FilePath $dummyExe -RedirectStandardOutput $d2out -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 500
    Send-TC $h @('reg','1','6','40101',"$($d2.Id)") 3000 | Out-Null
    Send-TC $h @('ctl','stop','1','6','40101','0') 8000
    $code = Get-O 'CODE'
    $msg  = Get-O 'MSG'
    $stopped = Wait-PidGone $d2.Id 8
    $graceOk      = ($script:tcRc -eq 0 -and $code -eq '0' -and $stopped)
    $graceUnavail = ($script:tcRc -ne 0 -and $code -eq '-1' -and ($msg -match 'taskkill|force|pid'))
    Check 'G1-15 ctl stop dispatched; graceful success or explicit failure surfaced' ($graceOk -or $graceUnavail) "rc=$($script:tcRc) code=$code msg=$msg pidGone=$stopped"
    if (!$stopped) { Stop-Process -Id $d2.Id -Force -ErrorAction SilentlyContinue }
    Send-TC $h @('unreg','1','6','40101') 2000 | Out-Null

    Stop-Replica 'single'
}

# ================================================================ G2 快照加载回归
function Test-Group-Snapshot {
    Log '=== G2: snapshot file load regression (asset: single_long) ==='
    # 预置状态文件: 含 2 个组件(uid=1 type=6 cid=42001; uid=1 type=5 cid=42002)
    Clean-TagDir 'snap'
    $snapCwd = Join-Path $runDir 'snap'
    $stateFile = Join-Path $snapCwd 'cluster_state_0.bin'
    & $snapGenExe $stateFile 1 6 42001 1001 1 5 42002 1002 | Out-Null
    Check 'G2-0 snapshot_gen produced state file' (Test-Path $stateFile) "missing $stateFile"

    # 校验文件头 magic
    $bytes = [System.IO.File]::ReadAllBytes($stateFile)
    $magicOk = ($bytes.Length -ge 40 -and $bytes[0] -eq 0x4B -and $bytes[1] -eq 0x42 -and $bytes[2] -eq 0xE5 -and $bytes[3] -eq 0x33)
    $hexHead = ('{0:X2}{1:X2}{2:X2}{3:X2}' -f $bytes[0], $bytes[1], $bytes[2], $bytes[3])
    Check 'G2-1 state file header magic=0x4B42E533' $magicOk ("len=$($bytes.Length) first=$hexHead")

    Start-Replica 'snap'
    $h = $cfg['snap'].host
    $lt = Wait-LeaderTag @('snap') 15
    Check 'G2-2 snapshot replica started & leader' ($lt -eq 'snap') 'no leader in 15s'

    Send-TC $h @('find','1','6','42001') 3000
    Check 'G2-3 registry restored: find 42001' ($script:tcRc -eq 0 -and (Get-O 'CID') -eq '42001') "rc=$($script:tcRc) out=$($script:tcOut)"
    Send-TC $h @('find','1','5','42002') 3000
    Check 'G2-4 registry restored: find 42002' ($script:tcRc -eq 0 -and (Get-O 'CID') -eq '42002') "rc=$($script:tcRc)"
    Send-TC $h @('queryall','1') 3000
    $cnt = Get-O 'COUNT'
    Check 'G2-5 queryall after snapshot load count=2' ($script:tcRc -eq 0 -and $cnt -eq '2') "count=$cnt"

    # 重启后仍可写新条目(快照上追加日志)
    Send-TC $h @('reg','1','6','42003','2222') 3000
    Check 'G2-6 append after snapshot load OK' ($script:tcRc -eq 0 -and (Get-O 'RESP') -eq 'MSG_RESP_OK') "rc=$($script:tcRc)"

    Stop-Replica 'snap'

    # G2-7 损坏快照容忍: 魔数被破坏 -> 静默忽略, 仍能正常服务(注册表为空)
    $b2 = [System.IO.File]::ReadAllBytes($stateFile)
    for ($i = 0; $i -lt 6; $i++) { $b2[$i] = 0 }
    [System.IO.File]::WriteAllBytes($stateFile, $b2)

    Start-Replica 'snap'
    $lt = Wait-LeaderTag @('snap') 15
    Check 'G2-7 corrupted snapshot ignored gracefully (still leader)' ($lt -eq 'snap') 'not leader after corrupted snapshot'
    if ($lt) {
        Send-TC $h @('queryall','1') 3000
        $cnt = Get-O 'COUNT'
        Check 'G2-8 corrupted snapshot -> empty registry (no crash/partial state)' ($cnt -eq '0') "count=$cnt"
        Send-TC $h @('reg','1','6','42004','3333') 3000
        Check 'G2-9 service still writable after corrupted snapshot' ($script:tcRc -eq 0) "rc=$($script:tcRc)"
    }
    Stop-Replica 'snap'
}

# ================================================================ G3 三副本一致与故障切换
function Test-Group-Three {
    Log '=== G3: 3-replica consensus / failover (assets a/b/c) ==='
    $tags = @('a', 'b', 'c')
    foreach ($t in $tags) { Clean-TagDir $t }
    foreach ($t in $tags) { Start-Replica $t }

    $L = Wait-LeaderTag $tags 25
    Check 'G3-1 3 replicas elect a leader within 25s' ($L -ne '') 'no leader'
    if (!$L) { Stop-All; return }

    # 唯一 leader
    $leaders = @()
    foreach ($t in $tags) {
        Send-TC $cfg[$t].host @('leader') 2000 | Out-Null
        if ($script:tcRc -eq 0) { $leaders += $t }
    }
    Check 'G3-2 exactly one replica reports itself leader' ($leaders.Count -eq 1 -and $leaders[0] -eq $L) "leaders=[$($leaders -join ',')]"

    # follower 重定向
    $fTags = @($tags | Where-Object { $_ -ne $L })
    $f0 = $fTags[0]
    Send-TC $cfg[$f0].host @('find','1','6','0') 3000
    $lr = Get-O 'LEADER'
    Check 'G3-3 follower redirects find with NOT_LEADER+leader addr' ($script:tcRc -ne 0 -and (Get-O 'RESP') -eq 'MSG_RESP_NOT_LEADER' -and $lr -match [regex]::Escape($cfg[$L].host)) "rc=$($script:tcRc) resp=$(Get-O 'RESP') leader=$lr"

    # 在 leader 注册
    Send-TC $cfg[$L].host @('reg','1','6','51001','70001') 3000
    Check 'G3-4 register on leader OK' ($script:tcRc -eq 0) "rc=$($script:tcRc)"

    # follower 掉线 -> 仍可 majority 提交
    $fDown = $fTags[0]
    Stop-Replica $fDown
    Check 'G3-5 follower killed' (Wait-ReplicaDead $cfg[$fDown].host 10) 'still alive after 10s'
    Send-TC $cfg[$L].host @('reg','1','6','51002','70002') 3000
    Check 'G3-6 register with 2/3 quorum (one follower down) OK' ($script:tcRc -eq 0) "rc=$($script:tcRc) out=$($script:tcOut)"

    # follower 重入并追赶日志
    Start-Replica $fDown
    Start-Sleep -Seconds 6
    Send-TC $cfg[$L].host @('leader') 2000 | Out-Null
    Check 'G3-7 rejoined follower does not disturb leader' ($script:tcRc -eq 0) 'leader lost after follower rejoin'

    # kill leader -> 剩余两副本选主; 已注册条目必须保留
    Stop-Replica $L
    Check 'G3-8 old leader down' (Wait-ReplicaDead $cfg[$L].host 10) 'still alive after 10s'
    $remain = @($tags | Where-Object { $_ -ne $L })
    $L2 = Wait-LeaderTag $remain 20
    Check 'G3-9 new leader elected among remaining replicas' ($L2 -ne '') 'no new leader in 20s'
    if ($L2) {
        Check 'G3-10 new leader differs from old' ($L2 -ne $L) "same=$L2"
        Send-TC $cfg[$L2].host @('queryall','1') 3000
        $cnt = Get-O 'COUNT'
        $hasA = $script:tcOut -match '51001'
        $hasB = $script:tcOut -match '51002'
        Check 'G3-11 registry preserved across failover (51001 & 51002)' ($script:tcRc -eq 0 -and $hasA -and $hasB) "count=$cnt a=$hasA b=$hasB"
    }

    # 旧 leader 重入 -> 同步为 follower
    Start-Replica $L
    Start-Sleep -Seconds 6
    $leaders = @()
    foreach ($t in $tags) {
        Send-TC $cfg[$t].host @('leader') 2000 | Out-Null
        if ($script:tcRc -eq 0) { $leaders += $t }
    }
    Check 'G3-12 after old-leader rejoin still exactly one leader' ($leaders.Count -eq 1) "leaders=[$($leaders -join ',')]"

    $cur = if ($leaders.Count -eq 1) { $leaders[0] } else { (Get-LeaderTag $tags) }
    if ($cur) {
        Send-TC $cfg[$cur].host @('reg','1','6','51003','70003') 3000
        $regOk = ($script:tcRc -eq 0)
        Send-TC $cfg[$cur].host @('queryall','1') 3000
        $cnt = Get-O 'COUNT'
        Check 'G3-13 register after rejoin & queryall count>=3' ($regOk -and $script:tcRc -eq 0 -and [int]$cnt -ge 3) "regOk=$regOk count=$cnt"
    }
    Stop-All
}

# ================================================================ G4 多数派丢失(-Full)
function Test-Group-Majority {
    Log '=== G4: majority-loss quorum (asset a/b/c, -Full) ==='
    $tags = @('a', 'b', 'c')
    foreach ($t in $tags) { Clean-TagDir $t }
    foreach ($t in $tags) { Start-Replica $t }
    $L = Wait-LeaderTag $tags 25
    Check 'G4-1 initial leader' ($L -ne '') 'no leader'
    if (!$L) { Stop-All; return }

    # 杀掉两个 follower -> 多数派丢失
    $downs = @($tags | Where-Object { $_ -ne $L })
    foreach ($t in $downs) { Stop-Replica $t }
    foreach ($t in $downs) {
        Check "G4-2 follower $t down" (Wait-ReplicaDead $cfg[$t].host 10) 'still alive'
    }
    Start-Sleep -Seconds 3   # 越过选举窗口, leader 处于无多数派状态

    Send-TC $cfg[$L].host @('reg','1','6','52001','1') 3500
    Check 'G4-3 write blocked without majority (no commit reply)' ($script:tcRc -ne 0) "unexpected rc=$($script:tcRc) resp=$(Get-O 'RESP')"

    # 恢复一个副本 -> 服务恢复
    Start-Replica $downs[0]
    Start-Sleep -Seconds 6
    $live = @($tags | Where-Object { $_ -ne $downs[1] })
    $L2 = Wait-LeaderTag $live 20
    Check 'G4-4 leader after majority restored' ($L2 -ne '') 'no leader in 20s'
    if ($L2) {
        Send-TC $cfg[$L2].host @('reg','1','6','52002','2') 4000
        Check 'G4-5 write path recovered after majority restored' ($script:tcRc -eq 0) "rc=$($script:tcRc) out=$($script:tcOut)"
    }
    Stop-All
}

# ================================================================ 主流程
Clean-RunDir
$sw = [System.Diagnostics.Stopwatch]::StartNew()
try {
    Test-Group-Single
    Test-Group-Snapshot
    Test-Group-Three
    if ($Full) { Test-Group-Majority }
}
finally {
    Stop-All
    if (!$KeepRunning) {
        # 回收可能残留的测试伪进程(保险)
        Get-Process -Name 'dummy_proc' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
}

$sw.Stop()
Log "=== finished in $([int]$sw.Elapsed.TotalSeconds)s : passed=$($script:passed) failed=$($script:failed) ==="
if ($script:failed -eq 0) { Write-Host '[TEST RESULT] ALL PASS'; exit 0 }
else                      { Write-Host "[TEST RESULT] $($script:failed) FAILED"; exit 1 }
