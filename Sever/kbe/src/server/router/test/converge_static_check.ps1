# ---------------------------------------------------------------------------
# Static convergence check for the "route everything through the Router" work.
#
# The unit tests cover the pure logic (ActorRegistry / RouterMail). This script
# guards the *wiring*: every high frequency send point that must go through the
# Router in Router mode. It needs no compiler and no running cluster, so it can
# run on any checkout / in CI as a cheap regression gate.
#
#   usage: powershell -ExecutionPolicy Bypass -File converge_static_check.ps1
#   exit : 0 = all checks passed, 1 = at least one check failed
# ---------------------------------------------------------------------------

$TestDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SrcRoot = (Resolve-Path (Join-Path $TestDir "..\..\..")).Path

$script:passed = 0
$script:failed = 0

function Check-File {
    param(
        [string]$Id,
        [string]$RelativePath,
        [string]$Pattern,
        [string]$Desc,
        [int]$MinMatches = 1
    )

    $full = Join-Path $SrcRoot $RelativePath

    if (-not (Test-Path $full)) {
        Write-Host "[  MISS  ] $Id  $Desc  (file not found: $RelativePath)"
        $script:failed++
        return
    }

    $content = Get-Content $full -Raw
    $count = ([regex]::Matches($content, $Pattern)).Count

    if ($count -ge $MinMatches) {
        Write-Host "[   OK   ] $Id  $Desc  ($count match(es))"
        $script:passed++
    }
    else {
        Write-Host "[ FAILED ] $Id  $Desc  (expected >= $MinMatches, got $count)"
        $script:failed++
    }
}

Write-Host "==== router convergence static check ===="
Write-Host "src root: $SrcRoot"
Write-Host ""

# ---- RouterMail contract (the shared wire contract) ----
Check-File "S01" "lib\server\router_mail.h" "stripClientForwardEnvelope" "RouterMail exposes the relay-envelope stripper"
Check-File "S02" "lib\server\router_mail.h" "BODY_CLIENT_MESSAGE\s*=\s*0x07" "BODY_CLIENT_MESSAGE tag is stable"

# ---- cellapp: dispatch + ghost/client convergence ----
Check-File "S03" "server\cellapp\cellapp.cpp" "case RouterMail::BODY_COMPONENT_MESSAGE" "cellapp dispatches BODY_COMPONENT_MESSAGE"
Check-File "S04" "server\cellapp\cellapp.cpp" "RouterMail::stripClientForwardEnvelope" "cellapp strips the baseapp relay envelope"
Check-File "S05" "server\cellapp\cellapp.cpp" "static_assert\(RouterMail::LEGACY_MSG_ID_SIZE" "legacy header size constants are asserted"
Check-File "S06" "server\cellapp\ghost_manager.cpp" "RouterMail::isEnabled\(\)" "GhostManager is Router-aware"
Check-File "S07" "server\cellapp\ghost_manager.cpp" "sendMailToComponent" "GhostManager delivers via Router component Actor"
Check-File "S08" "server\cellapp\witness.cpp" "sendBundleToClientActor" "Witness sends client updates through the Router" 5

# ---- baseapp: cell delivery + migration buffer bypass ----
Check-File "S09" "server\baseapp\entity.cpp" "RouterMail::isEnabled\(\)" "baseapp entity is Router-aware" 3
Check-File "S10" "server\baseapp\entity.cpp" "sendMailToComponent" "baseapp sendToCellapp delivers via Router"
Check-File "S11" "server\baseapp\entity.cpp" "onMigrationCellappOver\(targetCellAppID\)" "migration over path is reachable in Router mode"
Check-File "S12" "server\baseapp\entity_messages_forward_handler.cpp" "RouterMail::isEnabled\(\)" "legacy forward buffers are Router-aware"

# ---- safety valve: every convergence is guarded by the switch ----
Check-File "S13" "server\cellapp\cellapp.h" "sendBundleToClientActor" "cellapp exposes the client Actor helper"
Check-File "S14" "server\baseapp\baseapp.cpp" "RouterMail::BODY_FORWARD_TO_CLIENT" "baseapp keeps the legacy relay dispatcher"

# ---- Actor lifecycle / reconnect rebuild ----
Check-File "S15" "server\baseapp\baseapp.cpp" "entityMailbox\(eid, \(uint32\)CLIENT_TYPE\)" "baseapp binds the client proxy Actor"
Check-File "S16" "server\baseapp\baseapp.cpp" "routerRegisterActor" "baseapp (re)registers entity Actors" 3
Check-File "S17" "server\cellapp\cellapp.cpp" "routerRegisterActor" "cellapp (re)registers entity Actors on router ready" 2
Check-File "S18" "server\cellapp\entity.cpp" "routerSuspendActor" "cell migration suspends the entity Actor"
Check-File "S19" "lib\server\serverapp.cpp" "sendMailToComponent" "ServerApp offers the component Actor delivery helper"
Check-File "S20" "lib\entitydef\entitycallabstract.cpp" "RouterMail::isEnabled\(\)" "getChannel() returns NULL in Router mode (safety valve)"

# ---- L3 proto client: zero-dependency, so its duplicated constants must stay in sync ----
# 客户端刻意不 include common.h(以保持只依赖自包含的 router_interface.h)，因此这里
# 把它在本地重复定义的 COMPONENT_TYPE 与 lib/common/common.h 的真实值做等值比对，
# 防止两边漂移导致寻址/快速失败用例静默跑错目标。
function Check-ConstantSync {
    param(
        [string]$Id,
        [string]$ClientPattern,
        [string]$CommonPattern,
        [string]$Desc
    )

    $clientFile = Join-Path $SrcRoot "server\router\test\router_proto_client.cpp"
    $commonFile = Join-Path $SrcRoot "lib\common\common.h"

    if (-not (Test-Path $clientFile) -or -not (Test-Path $commonFile)) {
        Write-Host "[  MISS  ] $Id  $Desc  (source not found)"
        $script:failed++
        return
    }

    $cm = [regex]::Match((Get-Content $clientFile -Raw), $ClientPattern)
    $km = [regex]::Match((Get-Content $commonFile -Raw), $CommonPattern)

    if ($cm.Success -and $km.Success -and ($cm.Groups[1].Value -eq $km.Groups[1].Value)) {
        Write-Host "[   OK   ] $Id  $Desc  (= $($cm.Groups[1].Value))"
        $script:passed++
    }
    else {
        Write-Host "[ FAILED ] $Id  $Desc  (client='$($cm.Groups[1].Value)' common='$($km.Groups[1].Value)')"
        $script:failed++
    }
}

Check-ConstantSync "S21" "CT_CELLAPP\s*=\s*(\d+)" "CELLAPP_TYPE\s*=\s*(\d+)" "proto client CELLAPP constant matches common.h"
Check-ConstantSync "S22" "CT_BASEAPP\s*=\s*(\d+)" "BASEAPP_TYPE\s*=\s*(\d+)" "proto client BASEAPP constant matches common.h"
Check-ConstantSync "S23" "CT_CLIENT\s*=\s*(\d+)"  "CLIENT_TYPE\s*=\s*(\d+)"  "proto client CLIENT constant matches common.h"

Write-Host ""
Write-Host "==== converge_static_check: $($script:passed) passed, $($script:failed) failed ===="

if ($script:failed -gt 0) { exit 1 }
exit 0
