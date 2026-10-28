[CmdletBinding(DefaultParameterSetName = 'Iso')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Iso')][string]$IsoPath,
    [Parameter(Mandatory = $true, ParameterSetName = 'Iso')][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$IsoSha256,
    [Parameter(Mandatory = $true, ParameterSetName = 'Cloud')][string]$CloudDiskPath,
    [Parameter(Mandatory = $true, ParameterSetName = 'Cloud')][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$CloudSha256,
    [Parameter(Mandatory = $true, ParameterSetName = 'Cloud')][string]$SeedIso,
    [ValidatePattern('^batt-v3-[A-Za-z0-9-]+$')][string]$VmName = 'batt-v3-mysql',
    [ValidateRange(1024, 65535)][int]$SshPort = 2224,
    [string]$VBoxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe',
    [switch]$PlanOnly,
    [switch]$StartHeadless
)
$ErrorActionPreference = 'Stop'
# Never upgrade, clone, stop, or reconfigure the registered V2 machine.
$cloud = $PSCmdlet.ParameterSetName -eq 'Cloud'
if ($cloud) {
    $sourceDisk = (Resolve-Path -LiteralPath $CloudDiskPath).Path
    $iso = (Resolve-Path -LiteralPath $SeedIso).Path
    if ([IO.Path]::GetFileName($sourceDisk) -ne 'noble-server-cloudimg-amd64.vmdk') {
        throw 'Expected the Ubuntu Noble AMD64 cloud VMDK.'
    }
    if ((Get-FileHash -LiteralPath $sourceDisk -Algorithm SHA256).Hash -ne $CloudSha256) {
        throw 'Cloud disk SHA256 mismatch.'
    }
} else {
    $iso = (Resolve-Path -LiteralPath $IsoPath).Path
    if ([IO.Path]::GetFileName($iso) -notmatch '^ubuntu-24\.04(?:\.[0-9]+)?-live-server-amd64\.iso$') {
        throw 'Expected an Ubuntu 24.04 live-server AMD64 ISO.'
    }
    if ((Get-FileHash -LiteralPath $iso -Algorithm SHA256).Hash -ne $IsoSha256) {
        throw 'ISO SHA256 mismatch. Use the checksum verified from the Ubuntu release site.'
    }
}
if (-not (Test-Path -LiteralPath $VBoxManage -PathType Leaf)) { throw 'VBoxManage was not found.' }
$project = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$base = [IO.Path]::GetFullPath((Join-Path $project '.local\v3-vms'))
$target = [IO.Path]::GetFullPath((Join-Path $base $VmName))
if (-not $target.StartsWith($base + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'VM target escaped the private project directory.'
}
foreach ($directory in @($project, (Join-Path $project '.local'), $base)) {
    if ((Test-Path -LiteralPath $directory) -and
        ((Get-Item -LiteralPath $directory -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw 'VM parent must not be a junction or symlink.'
    }
}
if (Test-Path -LiteralPath $target) { throw 'VM target already exists; refusing to overwrite.' }
$registered = & $VBoxManage list vms
if ($LASTEXITCODE -ne 0) { throw 'Cannot list registered VMs.' }
if ($registered -match ('^"' + [regex]::Escape($VmName) + '" ')) { throw 'VM name already registered.' }
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, $SshPort)
try { $listener.Start() } finally { $listener.Stop() }
$disk = Join-Path $target 'system.vdi'
$commands = @(
    ,@('createvm', '--name', $VmName, '--ostype', 'Ubuntu_64', '--basefolder', $base, '--register')
    ,@('modifyvm', $VmName, '--memory', '4096', '--cpus', '2', '--nic1', 'nat', '--boot1', 'dvd', '--boot2', 'disk', '--audio-enabled', 'off', '--natpf1', "v3-ssh,tcp,127.0.0.1,$SshPort,,22")
    ,@('createmedium', 'disk', '--filename', $disk, '--size', '32768', '--format', 'VDI')
    ,@('storagectl', $VmName, '--name', 'SATA', '--add', 'sata', '--controller', 'IntelAhci')
    ,@('storageattach', $VmName, '--storagectl', 'SATA', '--port', '0', '--device', '0', '--type', 'hdd', '--medium', $disk)
    ,@('storageattach', $VmName, '--storagectl', 'SATA', '--port', '1', '--device', '0', '--type', 'dvddrive', '--medium', $iso)
)
if ($cloud) {
    # EFI cloud disk is cloned before use; the verified download stays immutable.
    $commands[1] += @('--firmware', 'efi')
    for ($index = 0; $index -lt $commands[1].Count; $index++) {
        if ($commands[1][$index] -eq '--boot1') { $commands[1][$index + 1] = 'disk' }
        if ($commands[1][$index] -eq '--boot2') { $commands[1][$index + 1] = 'none' }
    }
    $commands = @($commands[0], $commands[1],
        @('clonemedium', 'disk', $sourceDisk, $disk, '--format', 'VDI'),
        @('modifymedium', 'disk', $disk, '--resize', '32768'),
        $commands[3], $commands[4], $commands[5])
}
if ($StartHeadless) { $commands += ,@('startvm', $VmName, '--type', 'headless') }
if ($PlanOnly) {
    [pscustomobject]@{ status = 'PLAN_ONLY'; target = $target; ssh_port = $SshPort; commands = $commands } |
        ConvertTo-Json -Depth 5
    return
}
New-Item -ItemType Directory -Path $base -Force | Out-Null
foreach ($arguments in $commands) {
    & $VBoxManage @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "VM creation stopped at $($arguments[0]). Partial V3 files are retained for inspection; no cleanup was run."
    }
}
if ($cloud) {
    Write-Output "Created cloud VM $VmName at $target. Verify cloud-init and SSH on 127.0.0.1:$SshPort before recording admission."
} else {
    Write-Output "Created $VmName at $target. Install Ubuntu in VirtualBox, enable OpenSSH, then use 127.0.0.1:$SshPort. No OS is installed yet."
}
