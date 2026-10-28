param([ValidatePattern('^v3-cloud(?:-[a-z0-9]+)?$')][string]$Name = 'v3-cloud')
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$key = Join-Path $project ".local\$Name\id_ed25519"
if (-not (Test-Path -LiteralPath $key -PathType Leaf)) { throw 'Private key does not exist.' }
# Windows OpenSSH rejects OWNER RIGHTS ACEs produced by Python's private mkdir.
# Use explicit owner/SYSTEM/Administrators only; never grant Users or Everyone.
$owner = [Security.Principal.WindowsIdentity]::GetCurrent().User
$acl = [Security.AccessControl.FileSecurity]::new()
$acl.SetAccessRuleProtection($true, $false)
$acl.SetOwner($owner)
foreach ($sid in @($owner, [Security.Principal.SecurityIdentifier]::new('S-1-5-18'),
    [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))) {
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,
        [Security.AccessControl.FileSystemRights]::FullControl, [Security.AccessControl.AccessControlType]::Allow))
}
Set-Acl -LiteralPath $key -AclObject $acl
