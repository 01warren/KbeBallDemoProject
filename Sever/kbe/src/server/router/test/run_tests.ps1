# ---------------------------------------------------------------------------
# Router 测试统一编排器 (Windows)
#
#   powershell -ExecutionPolicy Bypass -File run_tests.ps1 [-WithRouter]
#
# 分层：
#   L1  纯逻辑单元   router_registry_test / router_protocol_test / router_mail_test
#   L2  静态收敛检查 converge_static_check.ps1（无需工具链）
#   L3  真实 router 进程的协议测试（需要 router.exe + router_proto_client.exe；
#       -WithRouter 时用 assets/res/server/kbengine.xml 拉起 router 子进程并跑客户端，
#       客户端退出码 2(router 不可达)按 SKIP 处理。本机因 v143 链接 v142/GL 预编译库
#       报 C1905，无法构建 router.exe，故 L3 恒为 SKIP）
#   L4  集群 E2E（不在本脚本内，见 TESTCASES.md）
#
# 退出码：0=全部通过 1=有用例失败 2=构建失败
# ---------------------------------------------------------------------------

param([switch]$WithRouter)

$ErrorActionPreference = "Continue"
$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $TestDir

$failedSuites = 0
$passedSuites = 0
$skippedSuites = 0

function Run-Suite {
    param([string]$Layer, [string]$Exe)

    Write-Host ""
    Write-Host "########## $Layer  $Exe ##########"

    if (-not (Test-Path "$TestDir\$Exe")) {
        Write-Host "[ SKIP ] $Exe not built"
        $script:skippedSuites++
        return
    }

    & "$TestDir\$Exe"
    if ($LASTEXITCODE -eq 0) { $script:passedSuites++ }
    else { $script:failedSuites++ }
}

Write-Host "########## build ##########"
& cmd /c "`"$TestDir\_build_tests.bat`""
if ($LASTEXITCODE -ne 0) {
    Write-Host "[ERROR] build failed, exit=$LASTEXITCODE"
    exit 2
}

# ---- L1 ----
Run-Suite "L1" "router_registry_test.exe"
Run-Suite "L1" "router_protocol_test.exe"
Run-Suite "L1" "router_mail_test.exe"

# ---- L2 ----
Write-Host ""
Write-Host "########## L2  converge_static_check ##########"
& powershell -ExecutionPolicy Bypass -File "$TestDir\converge_static_check.ps1"
if ($LASTEXITCODE -eq 0) { $passedSuites++ }
else { $failedSuites++ }

# ---- L3 ----
# 与 assets/res/server/kbengine.xml 中的 <router> 段保持一致。
$RouterServerPort  = 20296
$PendingBufferMax  = 8
$PendingTimeoutMS  = 2000

Write-Host ""
Write-Host "########## L3  router process protocol tests ##########"

$routerExe = Join-Path $TestDir "router.exe"
$clientExe = Join-Path $TestDir "router_proto_client.exe"

if (-not $WithRouter) {
    Write-Host "[ SKIP ] L3 requires -WithRouter and a built router.exe"
    Write-Host "         (this machine cannot link router.exe: v143 vs v142/GL libs -> C1905)"
    $skippedSuites++
}
elseif (-not (Test-Path $routerExe)) {
    Write-Host "[ SKIP ] router.exe not found, run _link_router.bat first"
    $skippedSuites++
}
elseif (-not (Test-Path $clientExe)) {
    Write-Host "[ SKIP ] router_proto_client.exe not built (run _build_tests.bat)"
    $skippedSuites++
}
else {
    # 用 test/assets/res/server/kbengine.xml 覆盖本次 router 子进程的 <router> 段
    # (端口 / pending 上限 / 超时)，不污染产品配置 kbe/res/server/kbengine_defaults.xml。
    $runDir  = Join-Path $TestDir "run"
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null

    $assets  = Join-Path $TestDir "assets"
    $kbeSrc  = (Resolve-Path (Join-Path $TestDir "..\..\..")).Path
    $kbeRoot = (Resolve-Path (Join-Path $kbeSrc "..\..")).Path
    $kbeRes  = Join-Path $kbeRoot "kbe\res"

    $env:KBE_ROOT     = $kbeRoot
    $env:KBE_BIN_PATH = (Join-Path $kbeRoot "kbe\bin\server") + "\"
    $env:KBE_RES_PATH = "$kbeRes;$assets;$(Join-Path $assets 'scripts');$(Join-Path $assets 'res')"

    $routerOut = Join-Path $runDir "router.stdout.log"
    $routerErr = Join-Path $runDir "router.stderr.log"

    Write-Host "[ RUN  ] starting router.exe (serverPort=$RouterServerPort, pendingMax=$PendingBufferMax, pendingTimeoutMS=$PendingTimeoutMS)"

    $router = Start-Process -FilePath $routerExe `
        -ArgumentList @("--cid=99001", "--gus=250") `
        -WorkingDirectory $TestDir `
        -RedirectStandardOutput $routerOut -RedirectStandardError $routerErr `
        -PassThru -WindowStyle Hidden

    $clientRc = 1
    try {
        & $clientExe "--port=$RouterServerPort" "--pending-max=$PendingBufferMax" "--pending-timeout-ms=$PendingTimeoutMS"
        $clientRc = $LASTEXITCODE
    }
    finally {
        if ($router -and -not $router.HasExited) {
            try { Stop-Process -Id $router.Id -Force -ErrorAction SilentlyContinue } catch {}
            $router.WaitForExit(3000) | Out-Null
            Write-Host "         router stopped (pid was $($router.Id)); logs: run\router.stdout.log / run\router.stderr.log"
        }
    }

    if ($clientRc -eq 2) {
        # 退出码 2 = 环境不可用(router 没起来/端口不通)，属于 SKIP 而不是失败
        Write-Host "[ SKIP ] router not reachable on port $RouterServerPort (see run\router.stderr.log)"
        $skippedSuites++
    }
    elseif ($clientRc -eq 0) {
        $passedSuites++
    }
    else {
        $failedSuites++
    }
}

Write-Host ""
Write-Host "########## SUMMARY: $passedSuites passed, $failedSuites failed, $skippedSuites skipped ##########"

if ($failedSuites -gt 0) { exit 1 }
exit 0
