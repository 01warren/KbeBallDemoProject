$clientExe = 'F:\KBEngine\BallProject\KbeBallDemoProject\Sever\kbe\src\server\cluster\smoke\smoke_client.exe'
$servicePort = 20093
function Invoke-Smoke {
    param([string]$hostIp, [string[]]$argsList)
    & $clientExe $hostIp $servicePort @argsList
    return $LASTEXITCODE
}
foreach ($h in '127.0.0.1','127.0.0.2','127.0.0.3') {
    $rc = Invoke-Smoke $h @('leader')
    Write-Host ("HOST {0} rc={1}" -f $h, $rc)
}
