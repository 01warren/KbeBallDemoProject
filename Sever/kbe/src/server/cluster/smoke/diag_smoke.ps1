# diag_smoke.ps1 -- 三副本诊断冒烟 (leader 选举 + 注册/续租/查找/全查), 保留副本进程便于查日志
$ErrorActionPreference = 'Continue'
$smokeDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$kbe = 'f:\KBEngine\BallProject\KbeBallDemoProject\Sever\kbe'
$binServer = Join-Path $kbe 'bin\server'
$clusterExe = Join-Path $binServer 'cluster.exe'
$clientExe  = Join-Path $smokeDir 'smoke_client.exe'
$runDir     = Join-Path $smokeDir 'run'

$hosts = '127.0.0.1','127.0.0.2','127.0.0.3'
$servicePort = 20093

function Log  { param($m) Write-Host ("[LOG] " + $m) }

function Start-Replica {
    param([string]$tag, [string]$cidArg)
    $assets = Join-Path $smokeDir ("assets_" + $tag)
    $cwd    = Join-Path $runDir $tag
    New-Item -ItemType Directory -Force -Path $cwd | Out-Null
    $env:KBE_ROOT = $kbe
    $env:KBE_BIN_PATH = $binServer + '\'
    $env:KBE_RES_PATH = (Join-Path $kbe 'res') + ';' + $assets + ';' + (Join-Path $assets 'res')
    $outFile = Join-Path $runDir ($tag + '.stdout.log')
    $errFile = Join-Path $runDir ($tag + '.stderr.log')
    $p = Start-Process -FilePath $clusterExe -ArgumentList $cidArg -WorkingDirectory $cwd `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -WindowStyle Hidden
    Log ("started {0} pid={1}" -f $tag, $p.Id)
}

function Invoke-Client {
    param([string]$hostIp, [string[]]$argsList)
    $output = & $clientExe $hostIp $servicePort @argsList 2>&1 | Out-String
    return @{ rc = $LASTEXITCODE; out = $output }
}

function Get-Leader {
    param([int]$maxSec)
    $deadline = (Get-Date).AddSeconds($maxSec)
    while ((Get-Date) -lt $deadline) {
        foreach ($h in $hosts) {
            $r = Invoke-Client $h @('leader')
            if ($r.rc -eq 0) { return $h }
        }
        Start-Sleep -Milliseconds 500
    }
    return ''
}

if (Test-Path $runDir) { Remove-Item -Recurse -Force $runDir }
New-Item -ItemType Directory -Force -Path $runDir | Out-Null
Start-Replica 'a' '--cid=30001'
Start-Replica 'b' '--cid=30002'
Start-Replica 'c' '--cid=30003'
Start-Sleep -Seconds 5

$leader = Get-Leader 15
Log ("leader={0}" -f $leader)
if (-not $leader) { Log 'NO LEADER FOUND'; exit 2 }

$checks = 0; $fails = 0
function Step {
    param([string]$name, [string]$hostIp, [string[]]$cmdArgs, [int]$expect)
    $script:checks++
    $r = Invoke-Client $hostIp $cmdArgs
    $ok = ($r.rc -eq $expect)
    if (-not $ok) { $script:fails++ }
    Log ("[{0}] {1} => rc={2} expect={3}" -f ($(if($ok){'PASS'}else{'FAIL'})), $name, $r.rc, $expect)
    $r.out -split "`n" | ForEach-Object { if ($_.Trim().Length) { Log ("      " + $_.Trim()) } }
}

Step 'leader query'       $leader @('leader')          0
Step 'reg baseapp'        $leader @('reg','1','2','20001','43210','0')  0
Step 'renew baseapp'      $leader @('renew','1','2','20001','43210')     0
Step 'find exact baseapp' $leader @('find','1','2','20001')              0
Step 'find by type'       $leader @('find','1','2','0')                  0
Step 'queryall uid=1'     $leader @('queryall','1')                      0
Step 'follower redirect'  $hosts[1] @('leader')                          2

Log ("RESULT checks={0} fails={1}" -f $checks, $fails)
if ($fails -gt 0) { exit 1 }
exit 0
