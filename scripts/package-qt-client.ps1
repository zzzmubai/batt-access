param(
    [Parameter(Mandatory = $true)][string]$Build,
    [string]$SdkRoot = 'E:\batt-v3-tools'
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$identifier = [guid]::NewGuid().ToString('N')
$package = Join-Path $SdkRoot "ops-package-$identifier"
$output = Join-Path $project "artifacts\batt-ops-v3.1-$identifier"
New-Item -ItemType Directory -Path $package | Out-Null
Copy-Item -LiteralPath (Join-Path $Build 'batt-ops.exe') -Destination $package
$env:PATH = "$SdkRoot\Tools\mingw1310_64\bin;$SdkRoot\6.8.3\mingw_64\bin;" + $env:PATH
& "$SdkRoot\6.8.3\mingw_64\bin\windeployqt.exe" --release --compiler-runtime "$package\batt-ops.exe" *> (Join-Path $package 'deployment.txt')
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed; see deployment.txt in retained package.' }
Copy-Item -LiteralPath (Join-Path $project 'clients\qt\THIRD_PARTY.md') -Destination $package
# Launch with only Windows system directories on PATH: no installed Qt/MinGW DLL lookup.
$savedPath = $env:PATH
try {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    $process = Start-Process -FilePath "$package\batt-ops.exe" -ArgumentList '--smoke-test' -PassThru -WindowStyle Hidden
    if (-not $process.WaitForExit(15000)) { $process.Kill(); throw 'Packaged app startup timed out.' }
    if ($process.ExitCode -ne 0) { throw "Packaged app exited with $($process.ExitCode)." }
} finally { $env:PATH = $savedPath }
Copy-Item -LiteralPath $package -Destination $output -Recurse
@{ result='QT_PACKAGE_PASS'; directory=$output; executable=(Join-Path $output 'batt-ops.exe'); startup='Windows system PATH only' } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'package-summary.json') -Encoding UTF8
Compress-Archive -LiteralPath $output -DestinationPath ($output + '.zip')
Get-Content -LiteralPath (Join-Path $output 'package-summary.json')
