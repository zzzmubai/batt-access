param(
    [string]$HostName = '127.0.0.1',
    [ValidateRange(1,65535)][int]$Port = 2222,
    [string]$LoginUser = 'book',
    [string]$RemoteDir = '/home/book/batt-access-v2',
    [string]$IdentityFile = '',
    [string]$KnownHostsFile = ''
)
$ErrorActionPreference = 'Stop'
if ($HostName -notmatch '^[A-Za-z0-9.-]+$' -or $LoginUser -notmatch '^[A-Za-z0-9_-]+$') {
    throw 'Use a plain host name and Linux user name.'
}
if ($RemoteDir -notmatch '^/home/[A-Za-z0-9_-]+/[A-Za-z0-9_./-]+$' -or $RemoteDir.Contains('..')) {
    throw 'RemoteDir must be a project directory under /home/<user>, with no spaces or .. .'
}
$projectDir = Split-Path -Parent $PSScriptRoot
$archive = Join-Path ([System.IO.Path]::GetTempPath()) ('batt-access-' + [guid]::NewGuid().ToString('N') + '.tar.gz')
$sshOptions = @('-o', 'IdentitiesOnly=yes')
if ($IdentityFile) { $sshOptions += @('-i', $IdentityFile) }
if ($KnownHostsFile) { $sshOptions += @('-o', ('UserKnownHostsFile=' + $KnownHostsFile)) }
$target = $LoginUser + '@' + $HostName
try {
    & tar.exe -czf $archive --exclude='./build' --exclude='./build-*' --exclude='./.git' --exclude='./.local' --exclude='./artifacts' --exclude='__pycache__' --exclude='*.tar.gz' --exclude='*.db' --exclude='*.db-*' --exclude='*.db.lock' -C $projectDir .
    if ($LASTEXITCODE -ne 0) { throw 'Source archive creation failed.' }
    & ssh.exe @sshOptions -p $Port $target "mkdir -p '$RemoteDir'"
    if ($LASTEXITCODE -ne 0) { throw 'SSH connection or directory creation failed.' }
    & scp.exe @sshOptions -P $Port $archive ($target + ':' + $RemoteDir + '/.batt-access-upload.tar.gz')
    if ($LASTEXITCODE -ne 0) { throw 'Upload failed.' }
    & ssh.exe @sshOptions -p $Port $target "tar -xzf '$RemoteDir/.batt-access-upload.tar.gz' -C '$RemoteDir'"
    if ($LASTEXITCODE -ne 0) { throw 'Source extraction failed.' }
    Write-Output "Sources synchronized to ${target}:$RemoteDir (build output and databases excluded)."
} finally {
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive -ErrorAction SilentlyContinue }
}
