# verify-integration.ps1  --  cluster(machine 替代) 集成验收一键执行器 (Windows)
#
# 将如下验收串联为一条可重复的命令, 任一阶段失败则继续执行后续阶段并汇总:
#   P0  preflight   : 依赖/端口/回环地址/残留进程/二进制新鲜度检查(失败默认中止)
#   P1  static      : docs/cluster_static_check.ps1   (静态一致性, 无需工具链)
#   P2  tests       : kbe/.../cluster/test/run_tests.ps1 -Full  (协议级回归 G1-G4)
#   P3  smoke       : kbe/.../cluster/smoke/run_smoke.ps1        (启动冒烟 S1-S5 + leader 故障切换)
#   P4  bootstrap   : 真实组件类型矩阵的"引导"协议验证
#                     以 cluster 单副本(端口 20193)为 leader, 按真实 COMPONENT_TYPE
#                     注册 dbmgr/loginapp/baseappmgr/cellappmgr/cellapp/baseapp/logger/interfaces,
#                     逐一校验 reg->renew->find(精确/按类型)->queryall 全链路。
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File verify-integration.ps1 [-Phase all|preflight|static|tests|smoke|bootstrap]
#   [-Fast] [-KeepRunning] [-Rebuild] [-SkipStaleCheck] [-NoCleanLeftover]
#
# 说明:
#   - 默认 -Phase all; 退出码 0=全部 PASS, 1=存在 FAIL(汇总于控制台与日志目录)。
#   - 三副本冒烟需要 127.0.0.2 / 127.0.0.3 可绑定(Win 需 netsh 添加回环地址)。
#   - -Rebuild 会先重建 cluster(test 工具/冒烟客户端总是按需重建, 见 P0)。
#   - 各阶段控制台输出同时落盘到 <LogDir>/phase_<name>.log, 默认 docs/verify-integration。

param(
    [ValidateSet('all', 'preflight', 'static', 'tests', 'smoke', 'bootstrap')]
    [string]$Phase = 'all',
    [switch]$Fast,
    [switch]$KeepRunning,
    [switch]$Rebuild,
    [switch]$SkipStaleCheck,
    [switch]$NoCleanLeftover,
    [string]$LogDir = ''
)

$ErrorActionPreference = 'Stop'
$root       = $PSScriptRoot
$kbe        = Join-Path $root 'kbe'
$clusterSrc = Join-Path $kbe 'src\server\cluster'
$testDir    = Join-Path $clusterSrc 'test'
$smokeDir   = Join-Path $clusterSrc 'smoke'
$binServer  = Join-Path $kbe 'bin\server'
$clusterExe = Join-Path $binServer 'cluster.exe'
$clientExe  = Join-Path $testDir 'test_client.exe'
$smokeExe   = Join-Path $smokeDir 'smoke_client.exe'

if ($LogDir -eq '') { $LogDir = Join-Path $root 'docs\verify-integration' }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$script:results = New-Object System.Collections.Generic.List[object]  # @{name; rc; detail}
$script:failures = 0

# ---------------------------------------------------------------- 工具
function Note   { param($m) Write-Host $m }
function Result { param($name, $rc, $detail)
    $script:results.Add([pscustomobject]@{ name = $name; rc = $rc; detail = $detail })
    if ($rc -ne 0) { $script:failures++ }
}
function Chk    { param($name, $ok, $detail)
    if ($ok) { Note "[PASS] $name" } else { Note "[FAIL] $name : $detail"; $script:failures++ }
}

# 子 powershell 阶段执行: 输出即打即落盘, 返回退出码
function Invoke-ChildPhase {
    param([string]$Name, [string]$ScriptFile, [string[]]$ArgList)
    $log = Join-Path $LogDir ("phase_$Name.log")
    $ps  = Join-Path $env:windir 'System32\WindowsPowerShell\v1.0\powershell.exe'
    Note ""
    Note "[verify] === phase $Name : $ScriptFile ==="
    Note "[verify]     console -> $log"
    $lines = & $ps -NoProfile -ExecutionPolicy Bypass -File $ScriptFile @ArgList 2>&1 | Tee-Object -FilePath $log
    $rc = $LASTEXITCODE
    $lines | ForEach-Object { Note ($_ | Out-String).TrimEnd("`r`n") }
    return $rc
}

# 某副本端口上是否可达
function Port-Alive {
    param([string]$h, [int]$port)
    try {
        $c = New-Object System.Net.Sockets.TcpClient
        $iar = $c.BeginConnect($h, $port, $null, $null)
        $ok = $iar.AsyncWaitHandle.WaitOne(300)
        if ($ok) { $c.EndConnect($iar); $c.Close() }
        return $ok
    } catch { return $false }
}

# 测试客户端一次调用 (test_client <host> <port> cmd args...)
function Invoke-TC {
    param([string[]]$CmdArgs)
    $all = @($CmdArgs) + @('--timeout=5000')
    $o = & $clientExe '127.0.0.1' 20193 @all 2>&1
    $rc = $LASTEXITCODE
    return [pscustomobject]@{ rc = $rc; out = ($o | Out-String) }
}
function Get-O {
    param($text, $key)
    $m = [regex]::Match($text, "(?m)^$([regex]::Escape($key))=(.*)$")
    if ($m.Success) { return $m.Groups[1].Value.Trim() } else { return '' }
}

# ---------------------------------------------------------------- P0 preflight
function Test-LoopbackBind {
    param([string]$ip)
    try {
        $l = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Parse($ip), 0)
        $l.Start(); $l.Stop()
        return $true
    } catch { return $false }
}

function Newest-SourceTime {
    param([string[]]$dirs)
    $max = Get-Date '2000-01-01'
    foreach ($d in $dirs) {
        if (!(Test-Path $d)) { continue }
        $t = Get-ChildItem $d -Recurse -Include *.cpp,*.h,*.inl,*.c -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($t -and $t.LastWriteTime -gt $max) { $max = $t.LastWriteTime }
    }
    return $max
}

function Phase-Preflight {
    Note ""
    Note "[verify] ========== P0 preflight =========="
    # 1) 依赖二进制
    $missing = @()
    foreach ($p in @($clusterExe, $clientExe, (Join-Path $testDir 'snapshot_gen.exe'),
                      (Join-Path $testDir 'dummy_proc.exe'), $smokeExe)) {
        if (!(Test-Path $p)) { $missing += $p }
    }
    if ($missing.Count) {
        Note "[FAIL] missing binaries:"
        $missing | ForEach-Object { Note "        $_" }
        Note "       rebuild:  cluster  -> kbe\src\server\cluster\smoke\_build_cluster.bat"
        Note "                 test tools-> kbe\src\server\cluster\test\_build_tests.bat"
        Note "                 smoke     -> kbe\src\server\cluster\smoke\_build_smoke.bat"
        Result 'P0 dependencies' 1 'missing binaries (see above)'
        return $false
    }
    Note "[PASS] dependency binaries present"

    # 2) 残留 cluster 进程 (仅清理属于本引擎 bin 目录的进程, 防端口冲突)
    $mine = @(Get-Process -Name 'cluster' -ErrorAction SilentlyContinue |
              Where-Object { $_.Path -and $_.Path.StartsWith($binServer, [System.StringComparison]::OrdinalIgnoreCase) })
    if ($mine.Count -gt 0) {
        if ($NoCleanLeftover) {
            Note "[WARN] leftover cluster processes kept (-NoCleanLeftover): $($mine.Id -join ',')"
        } else {
            Note "[INFO] stopping leftover cluster processes: $($mine.Id -join ',')"
            $mine | Stop-Process -Force -ErrorAction SilentlyContinue
            Start-Sleep -Milliseconds 800
        }
    }

    # 3) 端口占用
    foreach ($prt in 20093, 20094, 20193, 20194) {
        $used = Get-NetTCPConnection -LocalPort $prt -State Listen -ErrorAction SilentlyContinue
        if ($used) {
            $pidStr = ($used | Select-Object -ExpandProperty OwningProcess -Unique) -join ','
            Result 'P0 ports' 1 "port $prt in use by pid(s) $pidStr"
            return $false
        }
    }
    Note "[PASS] ports 20093/20094/20193/20194 free"

    # 4) 回环地址 (冒烟三副本需要)
    if (!$Fast) {
        foreach ($ip in '127.0.0.2', '127.0.0.3') {
            if (!(Test-LoopbackBind $ip)) {
                Note "[FAIL] loopback $ip not bindable (smoke 3-replica needs it)."
                Note "       Run as Administrator:"
                Note "         netsh interface ipv4 add address Loopback $ip 255.0.0.0"
                Result 'P0 loopback' 1 "127.0.0.2/3 not bindable"
                return $false
            }
        }
    }
    Note "[PASS] loopback 127.0.0.2 / 127.0.0.3 bindable"

    # 5) 二进制新鲜度 (源码新于产物 -> 建议 -Rebuild)
    if (!$SkipStaleCheck -and !$Rebuild) {
        $srcNew  = Newest-SourceTime @($clusterSrc)
        $exeOld  = (Get-Item $clusterExe).LastWriteTime
        if ($srcNew -gt $exeOld.AddSeconds(2)) {
            Note "[FAIL] cluster.exe($exeOld) older than cluster sources($srcNew); run with -Rebuild"
            Result 'P0 stale binaries' 1 "cluster.exe stale"
            return $false
        }
        $toolNew = Newest-SourceTime @($testDir, $smokeDir)
        $toolOld = [datetime]::MinValue
        foreach ($p in @($clientExe, $smokeExe)) { if ((Get-Item $p).LastWriteTime -gt $toolOld) { $toolOld = (Get-Item $p).LastWriteTime } }
        if ($toolNew -gt $toolOld.AddSeconds(2)) {
            Note "[FAIL] test/smoke clients older than sources; run with -Rebuild"
            Result 'P0 stale binaries' 1 'test/smoke clients stale'
            return $false
        }
    }
    if ($Rebuild) {
        Note "[INFO] -Rebuild: rebuilding test tools / smoke client / cluster ..."
        foreach ($b in (Join-Path $testDir '_build_tests.bat'),
                        (Join-Path $smokeDir '_build_smoke.bat'),
                        (Join-Path $smokeDir '_build_cluster.bat')) {
            Note "  running $b"
            cmd /c "`"$b`"" 2>&1 | Out-String | ForEach-Object { Note $_.TrimEnd() }
            if ($LASTEXITCODE -ne 0) { Result 'P0 rebuild' $LASTEXITCODE "build failed: $b"; return $false }
        }
    }
    Result 'P0 preflight' 0 ''
    return $true
}

# ---------------------------------------------------------------- P4 bootstrap
function Phase-Bootstrap {
    Note ""
    Note "[verify] ========== P4 component-bootstrap (protocol) =========="
    $runDir = Join-Path $testDir 'run\bootstrap'
    if (Test-Path $runDir) { Remove-Item -Recurse -Force $runDir }
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null

    $assets = Join-Path $testDir 'assets\single'
    $saveRoot   = $env:KBE_ROOT;     $saveBin = $env:KBE_BIN_PATH; $saveRes = $env:KBE_RES_PATH
    try {
        $env:KBE_ROOT     = $kbe
        $env:KBE_BIN_PATH = $binServer + '\'
        $env:KBE_RES_PATH = (Join-Path $kbe 'res') + ';' + $assets + ';' + (Join-Path $assets 'res')

        $p = Start-Process -FilePath $clusterExe -ArgumentList '--cid=33001' `
            -WorkingDirectory $runDir `
            -RedirectStandardOutput (Join-Path $runDir 'cluster.stdout.log') `
            -RedirectStandardError  (Join-Path $runDir 'cluster.stderr.log') `
            -PassThru -WindowStyle Hidden
        Note "started single-member cluster (pid=$($p.Id), port 20193)"

        $leader = ''
        $dl = (Get-Date).AddSeconds(20)
        while ((Get-Date) -lt $dl) {
            $r = Invoke-TC @('leader')
            if ($r.rc -eq 0) { $leader = '127.0.0.1'; break }
            Start-Sleep -Milliseconds 500
        }
        Chk 'B00 single-member cluster becomes leader' ($leader -ne '') 'no leader in 20s'
        if (!$leader) { return }

        # 真实服务器组件矩阵: 名称 -> COMPONENT_TYPE (common.h)
        #   dbmgr=1 loginapp=2 baseappmgr=3 cellappmgr=4 cellapp=5 baseapp=6
        #   logger=10 interfaces=13   (type 0/7/8/9/12/14/15 非游戏服务器引导组件)
        $lineup = @(
            @{ name = 'dbmgr';       type = 1;  cid = 90010 },
            @{ name = 'loginapp';    type = 2;  cid = 90020 },
            @{ name = 'baseappmgr';  type = 3;  cid = 90030 },
            @{ name = 'cellappmgr';  type = 4;  cid = 90040 },
            @{ name = 'cellapp';     type = 5;  cid = 90050 },
            @{ name = 'baseapp';     type = 6;  cid = 90060 },
            @{ name = 'logger';      type = 10; cid = 90100 },
            @{ name = 'interfaces';  type = 13; cid = 90130 }
        )

        $regOk = $true
        for ($i = 0; $i -lt $lineup.Count; $i++) {
            $e = $lineup[$i]
            $r = Invoke-TC @('reg', '1', "$($e.type)", "$($e.cid)", "$(80000 + $e.type)")
            $resp = Get-O $r.out 'RESP'
            if ($r.rc -ne 0 -or $resp -ne 'MSG_RESP_OK') { $regOk = $false; break }
        }
        Chk 'B01 register full component lineup (8 real types) OK' $regOk "reg chain broke at idx=$i"

        # 续租一次, 保证 6s lease 内完成后续查询
        $renewOk = $true
        foreach ($e in $lineup) {
            $r = Invoke-TC @('renew', '1', "$($e.type)", "$($e.cid)", "$(80000 + $e.type)")
            if ($r.rc -ne 0) { $renewOk = $false; break }
        }
        Chk 'B02 renew all components OK' $renewOk 'renew chain broke'

        $findOk = $true
        foreach ($e in $lineup) {
            $r = Invoke-TC @('find', '1', "$($e.type)", "$($e.cid)")
            if ($r.rc -ne 0 -or $r.out -notmatch ("CID=" + [regex]::Escape($e.cid))) { $findOk = $false; break }
        }
        Chk 'B03 find each component by (uid,type,cid)' $findOk 'exact find broke'

        $listOk = $true
        foreach ($e in $lineup) {
            $r = Invoke-TC @('find', '1', "$($e.type)", '0')   # 按类型查列表
            if ($r.rc -ne 0 -or $r.out -notmatch [regex]::Escape($e.cid)) { $listOk = $false; break }
        }
        Chk 'B04 find-by-type returns each component' $listOk 'type-list query broke'

        $qa = Invoke-TC @('queryall', '1')
        $cntS = Get-O $qa.out 'COUNT'
        $cnt = 0; if ($cntS -ne '' -and $cntS -match '^\d+$') { $cnt = [int]$cntS }
        $hasAll = $true
        foreach ($e in $lineup) { if ($qa.out -notmatch [regex]::Escape($e.cid)) { $hasAll = $false; break } }
        Chk 'B05 queryall lists all 8 components (registry snapshot)' ($qa.rc -eq 0 -and $cnt -ge 8 -and $hasAll) "rc=$($qa.rc) count=$cnt hasAll=$hasAll"
    }
    finally {
        if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; $p.WaitForExit(3000) | Out-Null }
        if ($null -ne $saveRoot) { $env:KBE_ROOT = $saveRoot } else { Remove-Item Env:\KBE_ROOT -ErrorAction SilentlyContinue }
        if ($null -ne $saveBin)  { $env:KBE_BIN_PATH = $saveBin } else { Remove-Item Env:\KBE_BIN_PATH -ErrorAction SilentlyContinue }
        if ($null -ne $saveRes)  { $env:KBE_RES_PATH = $saveRes } else { Remove-Item Env:\KBE_RES_PATH -ErrorAction SilentlyContinue }
    }
}

# ---------------------------------------------------------------- 主流程
$plan = @()
if ($Phase -eq 'all' -or $Phase -eq 'preflight') { $plan += 'preflight' }
if ($Phase -eq 'all' -or $Phase -eq 'static')    { $plan += 'static' }
if ($Phase -eq 'all' -or $Phase -eq 'tests')     { $plan += 'tests' }
if ($Phase -eq 'all' -or $Phase -eq 'smoke')     { $plan += 'smoke' }
if ($Phase -eq 'all' -or $Phase -eq 'bootstrap') { $plan += 'bootstrap' }

foreach ($ph in $plan) {
    $ok = $true
    switch ($ph) {
        'preflight' { $ok = Phase-Preflight }
        'static'    { $r = Invoke-ChildPhase 'static' (Join-Path $root 'docs\cluster_static_check.ps1') @(); Result 'P1 static check' $r '' }
        'tests'     { $a = @(); if ($Fast) { $a += '-Fast' }; if ($KeepRunning) { $a += '-KeepRunning' }
                      $r = Invoke-ChildPhase 'tests' (Join-Path $testDir 'run_tests.ps1') $a; Result 'P2 protocol regression tests' $r '' }
        'smoke'     { $a = @(); if ($Fast) { $a += '-Fast' }; if ($KeepRunning) { $a += '-KeepRunning' }
                      $r = Invoke-ChildPhase 'smoke' (Join-Path $smokeDir 'run_smoke.ps1') $a; Result 'P3 startup smoke & failover' $r '' }
        'bootstrap' { Phase-Bootstrap }
    }
    if ($ph -eq 'preflight' -and !$ok) {
        Note ""
        Note "[verify] preflight failed; aborting (fix above, or target a single phase with -Phase)."
        break
    }
}

# ---------------------------------------------------------------- 汇总
Note ""
Note "================ verify-integration summary ================"
$totalFail = 0
foreach ($res in $script:results) {
    $tag = if ($res.rc -eq 0) { 'PASS' } else { 'FAIL' }
    Note ("[{0}] {1}" -f $tag, $res.name)
    if ($res.rc -ne 0) { $totalFail++ }
}
if ($totalFail -eq 0) { Note '[VERIFY RESULT] ALL PASS'; exit 0 }
else                  { Note "[VERIFY RESULT] $totalFail FAILED"; exit 1 }
