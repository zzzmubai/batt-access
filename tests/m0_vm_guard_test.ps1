# Offline checks: synthetic ISO and a VBox stub; never creates/registers a VM.
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $project ('build-m0\vm-guards-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$iso = Join-Path $fixture 'ubuntu-24.04.4-live-server-amd64.iso'
[IO.File]::WriteAllText($iso, 'guard fixture, not a bootable image')
$hash = (Get-FileHash -LiteralPath $iso -Algorithm SHA256).Hash
$vbox = Join-Path $fixture 'vbox-stub.ps1'
@'
if (($args.Count -ne 2) -or $args[0] -ne 'list' -or $args[1] -ne 'vms') {
    throw 'Mutating VBox command reached the stub.'
}
'"ubuntu" {98db3f0f-a1f1-4292-bbc0-ec272270c39e}'
'"batt-v3-existing" {00000000-0000-0000-0000-000000000000}'
exit 0
'@ | Set-Content -LiteralPath $vbox -Encoding UTF8
$creator = Join-Path $project 'scripts\new-v3-vm.ps1'
$portProbe = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$portProbe.Start()
$freePort = $portProbe.LocalEndpoint.Port
$portProbe.Stop()
$checks = 0
function Assert-Rejected([scriptblock]$Action, [string]$Expected) {
    $rejected = $false
    try { & $Action | Out-Null } catch {
        if ($_.Exception.Message -notmatch $Expected) { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'Expected rejection did not occur.' }
    $script:checks++
}
$parameters = @{ IsoPath = $iso; IsoSha256 = $hash; VBoxManage = $vbox; SshPort = $freePort; PlanOnly = $true }
$plan = (& $creator @parameters) | ConvertFrom-Json
if ($plan.status -ne 'PLAN_ONLY' -or $plan.commands.Count -ne 6) { throw 'Invalid plan.' }
if (Test-Path -LiteralPath $plan.target) { throw 'Plan-only created a VM directory.' }
if (($plan.commands | ConvertTo-Json -Depth 4) -match '98db3f0f|modifyvm.*ubuntu') { throw 'V2 target was included.' }
$checks += 3
$cloudDisk = Join-Path $fixture 'noble-server-cloudimg-amd64.vmdk'
[IO.File]::WriteAllText($cloudDisk, 'guard fixture, not a virtual disk')
$cloudHash = (Get-FileHash -LiteralPath $cloudDisk -Algorithm SHA256).Hash
$cloudParameters = @{ CloudDiskPath = $cloudDisk; CloudSha256 = $cloudHash; SeedIso = $iso;
    VBoxManage = $vbox; SshPort = $freePort; PlanOnly = $true; StartHeadless = $true }
$cloudPlan = (& $creator @cloudParameters) | ConvertFrom-Json
if ($cloudPlan.commands.Count -ne 8 -or $cloudPlan.commands[2][0] -ne 'clonemedium' -or
    $cloudPlan.commands[7][0] -ne 'startvm') { throw 'Cloud image was not cloned or headless plan is invalid.' }
if (Test-Path -LiteralPath $cloudPlan.target) { throw 'Cloud plan created VM files.' }
$checks += 2
$bad = $parameters.Clone(); $bad.IsoSha256 = '0' * 64
Assert-Rejected { & $creator @bad } 'SHA256 mismatch'
$badCloud = $cloudParameters.Clone(); $badCloud.CloudSha256 = '0' * 64
Assert-Rejected { & $creator @badCloud } 'SHA256 mismatch'
$existing = $parameters.Clone(); $existing.VmName = 'batt-v3-existing'
Assert-Rejected { & $creator @existing } 'already registered'
$missing = $parameters.Clone(); $missing.IsoPath = Join-Path $fixture 'missing.iso'
Assert-Rejected { & $creator @missing } 'does not exist|Cannot find path'
$occupied = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $freePort)
try {
    $occupied.Start()
    Assert-Rejected { & $creator @parameters } 'socket|address|套接字|地址'
} finally { $occupied.Stop() }
Write-Output "M0_VM_GUARDS_PASS checks=$checks (stub only; no VM created)"
