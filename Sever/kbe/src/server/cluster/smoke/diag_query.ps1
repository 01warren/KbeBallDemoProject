param([string]$cmd = 'leader')
$run = 'F:\KBEngine\BallProject\KbeBallDemoProject\Sever\kbe\src\server\cluster\smoke\run'
$exe = 'F:\KBEngine\BallProject\KbeBallDemoProject\Sever\kbe\src\server\cluster\smoke\smoke_client.exe'
foreach ($h in '127.0.0.1','127.0.0.2','127.0.0.3') {
    $argsList = @($cmd)
    switch ($cmd) {
        'reg'   { $argsList = @('reg','1','2','20001','43210','0') }
        'renew' { $argsList = @('renew','1','2','20001','43210') }
        'find'  { $argsList = @('find','1','2','20001') }
        'qall'  { $argsList = @('queryall','1') }
    }
    $output = & $exe $h 20093 @argsList 2>&1
    $rc = $LASTEXITCODE
    Write-Host ("==== HOST {0} cmd={1} rc={2} ====" -f $h, $cmd, $rc)
    $output | ForEach-Object { Write-Host ("    " + $_) }
}
