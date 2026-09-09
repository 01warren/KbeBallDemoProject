# cluster_static_check.ps1
# 静态一致性自检：不需要编译工具链。
# 用法:  powershell -ExecutionPolicy Bypass -File docs/cluster_static_check.ps1
# 覆盖: XML well-formed、<cluster> 默认段、枚举/名称表、端口常量、双平台工程清单、
#       machine/广播残留引用(排除注释与已移出构建的待删文件)。任一 FAIL 退出码为 1。

$root = Split-Path -Parent $PSScriptRoot   # docs\.. -> Sever 根
$fails = 0

function Read-Text($rel) {
    return [System.IO.File]::ReadAllText((Join-Path $root $rel))
}
function Chk($name, $ok, $detail) {
    if ($ok) { Write-Host "[PASS] $name" }
    else     { Write-Host "[FAIL] $name : $detail"; $script:fails++ }
}
function Strip-XmlComments($t) {
    return [regex]::Replace($t, '<!--.*?-->', ' ', 'Singleline')
}
function Strip-CodeComments($t) {
    $t = [regex]::Replace($t, '/\*.*?\*/', ' ', 'Singleline')
    return [regex]::Replace($t, '(?m)//[^\r\n]*', '')
}

Write-Host "== 1. 配置文件 XML well-formed =="
foreach ($f in @('kbe/res/server/kbengine_defaults.xml',
                 'kbe/res/sdk_templates/server/python_assets/res/server/kbengine.xml',
                 'kbe/src/server/cluster/cluster.vcxproj')) {
    try   { $null = [xml](Read-Text $f); Chk "XML parses: $f" $true '' }
    catch { Chk "XML parses: $f" $false $_.Exception.Message }
}

Write-Host "== 2. 引擎默认 <cluster> 段 =="
$dx = Read-Text 'kbe/res/server/kbengine_defaults.xml'
$dxc = Strip-XmlComments $dx
$clSeg = [regex]::Match($dxc, '<cluster>([\s\S]*?)</cluster>').Groups[1].Value
$addr  = [regex]::Match($clSeg, '<addresses>([\s\S]*?)</addresses>').Groups[1].Value
Chk 'defaults has <cluster>' ($dxc -match '<cluster>') ''
Chk 'defaults servicePort=20093' ($clSeg -match '<servicePort>\s*20093\s*</servicePort>') ''
Chk 'defaults peerPort=20094'    ($clSeg -match '<peerPort>\s*20094\s*</peerPort>') ''
Chk 'defaults has electionTimeout' ($clSeg -match '<electionTimeout>') ''
Chk 'defaults has componentLeaseSeconds' ($clSeg -match '<componentLeaseSeconds>') ''
Chk 'defaults addresses empty (dev self-bootstrap)' ($addr -notmatch '<item>') ''

Write-Host "== 3. 组件类型枚举与名称表 =="
$ch = Read-Text 'kbe/src/lib/common/common.h'
Chk 'CLUSTER_TYPE=15' ($ch -match 'CLUSTER_TYPE\s*=\s*15,') ''
Chk 'COMPONENT_END_TYPE=16' ($ch -match 'COMPONENT_END_TYPE\s*=\s*16,') ''
$cc = ([regex]::Matches($ch, '"[ ]*cluster[ ]*"')).Count
Chk "3 arrays each contain cluster (got $cc)" ($cc -eq 3) ''

Write-Host "== 4. 端口常量一致性 =="
$iface = Read-Text 'kbe/src/server/cluster/cluster_interface.h'
$sconf = Read-Text 'kbe/src/lib/server/serverconfig.h'
Chk 'cluster_interface 20093' ($iface -match 'DEFAULT_CLUSTER_SERVICE_PORT = 20093') ''
Chk 'cluster_interface 20094' ($iface -match 'DEFAULT_CLUSTER_PEER_PORT = 20094') ''
Chk 'serverconfig 20093'       ($sconf -match 'clusterServicePort = 20093') ''
Chk 'serverconfig 20094'       ($sconf -match 'clusterPeerPort = 20094') ''

Write-Host "== 5. 双平台工程清单 =="
$sln   = Read-Text 'kbe/src/kbengine.sln'
$mkSrv = Read-Text 'kbe/src/server/Makefile'
$mkLibSrv = Read-Text 'kbe/src/lib/server/Makefile'
$mkNet = Read-Text 'kbe/src/lib/network/Makefile'
Chk 'sln has cluster.vcxproj'    ($sln -match 'cluster\.vcxproj') ''
Chk 'sln no machine.vcxproj'     ($sln -notmatch 'machine\.vcxproj') ''
Chk 'server/Makefile has cluster' ($mkSrv -match '(^|\s)cluster') ''
Chk 'server/Makefile no machine'  ($mkSrv -notmatch '(^|\s)machine') ''
Chk 'lib/server/Makefile no obsolete units' ($mkLibSrv -notmatch 'id_component_querier|bundle_broadcast') ''
Chk 'lib/network/Makefile no obsolete units' ($mkNet -notmatch 'id_component_querier|bundle_broadcast') ''

Write-Host "== 6. machine/广播残留引用(构建内源码, 剥离注释; 排除待删文件) =="
$obsolete = @(
    'kbe/src/server/machine',
    'kbe/src/lib/server/id_component_querier.h',
    'kbe/src/lib/server/id_component_querier.cpp',
    'kbe/src/lib/network/bundle_broadcast.h',
    'kbe/src/lib/network/bundle_broadcast.inl',
    'kbe/src/lib/network/bundle_broadcast.cpp')
$obsoleteLower = @($obsolete | ForEach-Object { (Join-Path $root $_).ToLower() })
$hit = @()
$scanDirs = @('kbe/src/lib/server', 'kbe/src/lib/network', 'kbe/src/server')
$pat = 'id_component_querier|bundle_broadcast|onFindInterfaceAddr|KBE_PORT_BROADCAST_DISCOVERY|KBE_MACHINE_BROADCAST_SEND_PORT|#include\s*[<"]MachineInterface'
foreach ($d in $scanDirs) {
    Get-ChildItem (Join-Path $root $d) -Recurse -Include *.h,*.hpp,*.cpp,*.inl,*.c -ErrorAction SilentlyContinue | ForEach-Object {
        $full = $_.FullName.ToLower()
        $skip = $false
        foreach ($o in $obsoleteLower) {
            if ($full -eq $o -or $full.StartsWith($o + '\')) { $skip = $true; break }
        }
        if ($skip) { return }
        $t = Strip-CodeComments ([System.IO.File]::ReadAllText($_.FullName))
        if ($t -match $pat) { $hit += $_.FullName.Substring($root.Length + 1) }
    }
}
if ($hit.Count -eq 0) { Chk 'no machine/broadcast leftover source refs' $true '' }
else                  { Chk 'no machine/broadcast leftover source refs' $false ($hit -join '; ') }

Write-Host "== 7. 待物理删除文件 (INFO, 不影响构建; 需用户授权删除) =="
$still = @()
foreach ($d in $obsolete) { if (Test-Path (Join-Path $root $d)) { $still += $d } }
if ($still.Count -eq 0) { Write-Host "[INFO] obsolete files already deleted" }
else {
    Write-Host "[INFO] still present (removed from build manifests only):"
    foreach ($s in $still) { Write-Host "        $s" }
    Write-Host "        删除: powershell -Command `"Remove-Item -Recurse -Force `'$root\kbe\src\server\machine`',$root\kbe\src\lib\server\id_component_querier.h,$root\kbe\src\lib\server\id_component_querier.cpp,$root\kbe\src\lib\network\bundle_broadcast.h,$root\kbe\src\lib\network\bundle_broadcast.inl,$root\kbe\src\lib\network\bundle_broadcast.cpp`""
}

Write-Host ""
if ($fails -eq 0) { Write-Host '[RESULT] ALL PASS'; exit 0 }
else              { Write-Host "[RESULT] $fails FAILED"; exit 1 }
