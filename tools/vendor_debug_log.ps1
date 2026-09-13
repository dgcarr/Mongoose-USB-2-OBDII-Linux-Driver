<#
.SYNOPSIS
Toggle the vendor's own debug log (test A3).

monpj432.dll contains the UTF-16 strings "DebugEnable", "C:\DrewTech\Logs" and the
filename template "%s\%s_%04d-%02d-%02d_%02d-%02d-%02d_%04d.txt", alongside
diagnostics such as "Contains %d frames / %d bytes" and
"Frame total: Contained %ld / Dropped %ld frames". If DebugEnable is the gate,
the vendor writes logs naming its own structures - stronger evidence than
anything inferred from bytes.

This is a cheap bet, not a dependency. -Disable restores the prior state exactly:
a value absent before is removed, not set to zero.

.EXAMPLE
  .\tools\vendor_debug_log.ps1 -Enable
  .\tools\vendor_debug_log.ps1 -Status
  .\tools\vendor_debug_log.ps1 -Disable
#>
[CmdletBinding(DefaultParameterSetName = 'Status')]
param(
    [Parameter(ParameterSetName = 'Enable')][switch]$Enable,
    [Parameter(ParameterSetName = 'Disable')][switch]$Disable,
    [Parameter(ParameterSetName = 'Status')][switch]$Status,
    [Parameter(ParameterSetName = 'Enable')][ValidateSet('DWord', 'String')][string]$Kind = 'DWord'
)

$ErrorActionPreference = 'Stop'
$key    = 'HKLM:\SOFTWARE\WOW6432Node\PassThruSupport.04.04\Drew Technologies Inc. - MongoosePro JLR'
$value  = 'DebugEnable'
$logs   = 'C:\DrewTech\Logs'
$backup = Join-Path $PSScriptRoot '..\analysis\captures\windows\passthru-key-backup.reg'

if (-not (Test-Path $key)) { throw "PassThru key not found: $key" }

function Get-State {
    $item = Get-ItemProperty -Path $key -Name $value -ErrorAction SilentlyContinue
    if ($null -eq $item) { return $null }
    return $item.$value
}

if ($Enable) {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Writing under HKLM requires Administrator. Re-run from an elevated shell.'
    }
    $backupDir = Split-Path -Parent $backup
    New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
    if (-not (Test-Path $backup)) {
        $native = 'HKLM\SOFTWARE\WOW6432Node\PassThruSupport.04.04\Drew Technologies Inc. - MongoosePro JLR'
        & reg.exe export $native $backup /y | Out-Null
        Write-Host "Backed up the key to $backup"
    } else {
        Write-Host "Backup already present at $backup (left untouched)"
    }
    if ($null -ne (Get-State)) {
        Write-Warning "$value already exists with value '$(Get-State)'. Leaving it alone."
    } else {
        $data = if ($Kind -eq 'DWord') { 1 } else { '1' }
        New-ItemProperty -Path $key -Name $value -Value $data -PropertyType $Kind -Force | Out-Null
        Write-Host "Set $value = $data ($Kind)"
    }
    Write-Host "Now run a capture and check whether $logs appears."
    Write-Host 'If nothing appears, try -Kind String, then give up on this route.'
}
elseif ($Disable) {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Writing under HKLM requires Administrator. Re-run from an elevated shell.'
    }
    if ($null -eq (Get-State)) {
        Write-Host "$value is not set; nothing to remove."
    } else {
        Remove-ItemProperty -Path $key -Name $value
        Write-Host "Removed $value (the value was absent before test A3, so removal is the correct restore)."
    }
}

$state = Get-State
Write-Host ''
Write-Host "key        : $key"
Write-Host "$value : $(if ($null -eq $state) { '<absent>' } else { $state })"
if (Test-Path $logs) {
    $files = Get-ChildItem $logs -File -ErrorAction SilentlyContinue
    Write-Host "$logs : present, $($files.Count) file(s)"
    $files | Sort-Object LastWriteTime -Descending | Select-Object -First 5 |
        ForEach-Object { Write-Host "  $($_.LastWriteTime.ToString('s'))  $($_.Length,9)  $($_.Name)" }
} else {
    Write-Host "$logs : absent"
}
