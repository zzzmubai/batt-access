[CmdletBinding()]
param(
    [string]$SdkRoot = 'E:\batt-v3-tools',
    [Parameter(Mandatory = $true)][string]$CMake,
    [Parameter(Mandatory = $true)][string]$Ninja
)
$ErrorActionPreference = 'Stop'
$sdk = (Resolve-Path -LiteralPath $SdkRoot).Path
if ($sdk -match '[^\x20-\x7e]') { throw 'Qt SDK and source snapshot require an ASCII path.' }
$project = Split-Path -Parent $PSScriptRoot
$identifier = [guid]::NewGuid().ToString('N')
$snapshot = Join-Path $sdk "ops-source-$identifier"
$build = Join-Path $sdk "ops-build-$identifier"
$evidence = Join-Path $project "artifacts\qt-ops-$identifier"
$source = Join-Path $project 'clients\qt'
Copy-Item -LiteralPath $source -Destination $snapshot -Recurse
foreach ($file in Get-ChildItem -LiteralPath $source -File) {
    if ((Get-FileHash -LiteralPath $file.FullName).Hash -ne
        (Get-FileHash -LiteralPath (Join-Path $snapshot $file.Name)).Hash) { throw 'Source copy differs.' }
}
New-Item -ItemType Directory -Path $evidence | Out-Null
$env:PATH = (Join-Path $sdk 'Tools\mingw1310_64\bin') + ';' + (Join-Path $sdk '6.8.3\mingw_64\bin') + ';' + $env:PATH
& $CMake -S $snapshot -B $build -G Ninja "-DCMAKE_MAKE_PROGRAM=$Ninja" `
    "-DCMAKE_CXX_COMPILER=$sdk/Tools/mingw1310_64/bin/g++.exe" "-DCMAKE_PREFIX_PATH=$sdk/6.8.3/mingw_64" *> (Join-Path $evidence 'configure.txt')
if ($LASTEXITCODE -ne 0) { throw "Qt configure failed; evidence: $evidence" }
& $CMake --build $build -j2 *> (Join-Path $evidence 'build.txt')
if ($LASTEXITCODE -ne 0) { throw "Qt build failed; evidence: $evidence" }
$ctest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
& $ctest --test-dir $build --output-on-failure -V *> (Join-Path $evidence 'test.txt')
if ($LASTEXITCODE -ne 0) { throw "Qt tests failed; evidence: $evidence" }
@{ result = 'QT_OPS_PASS'; source = $snapshot; build = $build; evidence = $evidence; qt = '6.8.3'; compiler = 'MinGW 13.1.0' } |
    ConvertTo-Json | Tee-Object -FilePath (Join-Path $evidence 'summary.json')
