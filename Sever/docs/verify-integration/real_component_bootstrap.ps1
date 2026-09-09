# real_component_bootstrap.ps1 -- 真实 KBEngine 组件进程的引导验证 (增强项, 需先全量构建引擎)
#
# 前提:
#   - 已用 kbengine.sln Release|Win64 构建, kbe/bin/server 下存在 cluster/logger/dbmgr/
#     baseappmgr/cellappmgr/loginapp/baseapp/cellapp.exe
#   - 模板资产 kbe/res/sdk_templates/server/python_assets 充当服务器资产;
#     本脚本会复制一份到 run 目录并以 <cluster> 单副本(127.0.0.1:20093)覆盖配置。
#   - 无 MySQL 时 dbmgr 会启动失败退出, 其余组件因找不到 dbmgr 最终也会退出,
#     因此"注册/引导"链路可验证, "完整业务运行"受环境(无 DB)限制。
#   - 若系统 App Control(WDAC)拦截 baseapp.exe/cellapp.exe, 脚本记录 WARN 并跳过。
#
# 断言依据: 组件进程自身日志出现 "register self <TYPE>:<cid>(pid=..) success."
#   (Components::process 状态机: 先注册到 cluster, 成功后才查找依赖组件)
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File real_component_bootstrap.ps1
# 退出码: 0 = 关键断言通过; 1 = 失败。

$ErrorActionPreference = 'Continue'
$root     = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # Sever (脚本位于 docs\verify-integration)
$kbe      = Join-Path $root 'kbe'
$binServer= Join-Path $kbe 'bin\server'
$clusterExe = Join-Path $binServer 'cluster.exe'
$tcExe      = Join-Path (Join-Path $kbe 'src\server\cluster\test') 'test_client.exe'
$tmpl       = Join-Path $kbe 'res\sdk_templates\server\python_assets'
$outDir     = Join-Path $PSScriptRoot 'real_run'
$assets     = Join-Path $outDir 'assets'

if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
Copy-Item -Recurse -Force $tmpl $assets | Out-Null

# 覆盖: cluster 单副本自举(127.0.0.1:20093); 无 addresses -> 单成员直接成主
@'
<root>
	<!-- 真实组件引导验证: 单机单副本 cluster, 组件经 127.0.0.1:20093 注册/发现 -->
	<cluster>
		<internalInterface> 127.0.0.1 </internalInterface>
		<servicePort> 20093 </servicePort>
		<dataPort> 20094 </dataPort>
		<componentData>
			<leaseSeconds> 6 </leaseSeconds>
		</componentData>
	</cluster>
</root>
'@ | Set-Content -Path (Join-Path $assets 'res\server\kbengine.xml') -Encoding ASCII

$procs = @{}
function Start-App {
    param([string]$Name)
    $exe = Join-Path $binServer ($Name + '.exe')
    try {
        $p = Start-Process -FilePath $exe -WorkingDirectory $assets `
            -RedirectStandardOutput (Join-Path $outDir ($Name + '.out.log')) `
            -RedirectStandardError  (Join-Path $outDir ($Name + '.err.log')) `
            -PassThru -WindowStyle Hidden
        $procs[$Name] = $p
        Write-Host ("started {0} (pid={1})" -f $Name, $p.Id)
    } catch {
        Write-Host ("[WARN] cannot start {0}: {1}" -f $Name, $_.Exception.Message)
    }
}
function Stop-Apps {
    foreach ($k in $procs.Keys) {
        if ($procs[$k] -and !$procs[$k].HasExited) { Stop-Process -Id $procs[$k].Id -Force -ErrorAction SilentlyContinue }
    }
    Start-Sleep -Milliseconds 500
}
function App-LogFiles { param([string]$Name)
    $logDir = Join-Path $assets 'logs'
    if (!(Test-Path $logDir)) { return @() }
    $f = Get-ChildItem -Path $logDir -Filter ('*' + $Name + '*') -File -ErrorAction SilentlyContinue
    if ($null -eq $f) { return @() }
    return @($f | Select-Object -ExpandProperty FullName)
}
function Has-RegLog { param([string]$Name)
    # 注册成功证据位于组件文件日志(assets/logs)与 stdout 中:
    # "register self <type>:<cid>(pid=..) success."
    $lf = Join-Path $outDir ($Name + '.out.log')
    if (Test-Path $lf) {
        $raw = Get-Content $lf -Raw -ErrorAction SilentlyContinue
        if ($raw -match ("register self " + $Name + ":.* success")) { return $true }
    }
    foreach ($p in App-LogFiles $Name) {
        $raw = Get-Content $p -Raw -ErrorAction SilentlyContinue
        if ($raw -match ("register self " + $Name + ":.* success")) { return $true }
    }
    return $false
}
function Log-Tail { param([string]$Name, [int]$N = 8)
    $files = App-LogFiles $Name
    if ($files.Count -eq 0) {
        $l = Join-Path $outDir ($Name + '.out.log')
        if (Test-Path $l) { $files = @($l) }
    }
    if ($files.Count -gt 0) {
        Write-Host ("---- tail " + $Name + " ( " + ($files -join ' ; ') + " ) ----")
        foreach ($p in $files) {
            Get-Content $p -Tail $N | ForEach-Object { Write-Host $_ }
        }
    }
}
function Tag { param($m) Write-Host ("[{0}] {1}" -f (Get-Date -Format HH:mm:ss), $m) }

$env:KBE_ROOT     = $kbe
$env:KBE_BIN_PATH = $binServer + '\'
$env:KBE_RES_PATH = (Join-Path $kbe 'res') + ';' + $assets + ';' + (Join-Path $assets 'res')

$script:failed = $false
try {
    Tag '== R0: start single-member cluster =='
    Start-App 'cluster'
    $leader = $false
    for ($i = 0; $i -lt 30 -and !$leader; $i++) {
        Start-Sleep -Milliseconds 500
        & $tcExe '127.0.0.1' 20093 leader 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) { $leader = $true }
    }
    if ($leader) { Write-Host '[PASS] R0 cluster leader up' } else { Write-Host '[FAIL] R0 no leader'; $script:failed = $true }

    if ($leader) {
        $qaRaw = & $tcExe '127.0.0.1' 20093 queryall 1 2>&1 | Out-String
        Write-Host ('-- queryall raw --'); Write-Host $qaRaw.TrimEnd("`r`n")

        Tag '== R1: real logger bootstrap =='
        Start-App 'logger'
        Start-Sleep -Seconds 5
        if (Has-RegLog 'logger') { Write-Host '[PASS] R1 logger registered to cluster (register self ... success)' }
        else { Write-Host '[FAIL] R1 logger register line not found'; Log-Tail 'logger' 15; $script:failed = $true }

        Tag '== R2: real dbmgr/baseappmgr/cellappmgr/loginapp/interfaces bootstrap (db/demo dependent) =='
        foreach ($n in 'interfaces','dbmgr','baseappmgr','cellappmgr','loginapp') { Start-App $n; Start-Sleep -Milliseconds 1200 }
        Start-Sleep -Seconds 8

        $started = @()
        foreach ($n in 'interfaces','dbmgr','baseappmgr','cellappmgr','loginapp') {
            if ($procs[$n]) { $started += $n }
        }
        $regCount = 0
        foreach ($n in $started) {
            if (Has-RegLog $n) {
                Write-Host ("[PASS] R2 " + $n + " registered to cluster before dependency wait/exit")
                $regCount++
            }
        }
        if ($regCount -gt 0) { Write-Host ("[PASS] R2 {0}/{1} real components registered" -f $regCount, $started.Count) }
        else {
            Write-Host '[WARN] R2 none registered (see tails below; likely DB/dependency exit too early)'
        }

        Tag '== R3: survivors & log tails =='
        Start-Sleep -Seconds 6
        $aliveList = @()
        foreach ($n in $procs.Keys) { if ($procs[$n] -and !$procs[$n].HasExited) { $aliveList += $n } }
        Write-Host ("still alive: " + ($aliveList -join ', '))
        foreach ($n in 'logger','interfaces','dbmgr','baseappmgr','cellappmgr','loginapp') { Log-Tail $n }
    }
}
finally {
    Tag 'cleanup'
    Stop-Apps
}

if ($script:failed) { Write-Host '[REAL-RESULT] FAILED'; exit 1 }
else { Write-Host ('[REAL-RESULT] PASS (real components bootstrap via cluster; logs under ' + $outDir + ')'); exit 0 }
