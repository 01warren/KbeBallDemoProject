Get-Process -Name cluster -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
$smoke = 'F:\KBEngine\BallProject\KbeBallDemoProject\Sever\kbe\src\server\cluster\smoke'
$run = Join-Path $smoke 'run'
if (Test-Path $run) { Remove-Item -Recurse -Force $run -ErrorAction SilentlyContinue }
$p = Start-Process -FilePath 'powershell.exe' -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $smoke 'run_smoke.ps1') `
    -RedirectStandardOutput (Join-Path $smoke 'smoke_run.out.log') `
    -RedirectStandardError  (Join-Path $smoke 'smoke_run.err.log') -PassThru -WindowStyle Hidden
Write-Host ("runner pid=" + $p.Id)
