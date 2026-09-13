<#
.SYNOPSIS
One experiment = one stimulus, three synchronised records.

Starts a USBPcap capture on the adapter's root hub, runs one step script through
mongoose-reference.exe, stops the capture, and snapshots the vendor debug log if
it is active. Everything for a run lands in analysis/captures/windows/<name>/.

USBPcap needs Administrator, so run this from an elevated shell.

.EXAMPLE
  .\tools\capture.ps1 -Script tools\scripts\b1-open-close.txt
  .\tools\capture.ps1 -Script tools\scripts\d4-sustained-load.txt -Gap 0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Script,
    [string]$Name,
    [string]$Interface,
    [int]$Gap = 2000,
    [string]$Dll = 'C:\Program Files (x86)\Drew Technologies, Inc\J2534\MongoosePro JLR\monpj432.dll',
    [string]$Harness = 'build\windows\mongoose-reference.exe',
    [int]$SettleMs = 1500
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Test-Path $Script))  { throw "Step script not found: $Script" }
if (-not (Test-Path $Harness)) { throw "Harness not built: $Harness (see docs/CAPTURING.md)" }
if (-not (Test-Path $Dll))     { throw "Vendor DLL not found: $Dll" }

$identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'USBPcap requires Administrator. Re-run this script from an elevated shell.'
}

$usbpcap = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
if (-not (Test-Path $usbpcap)) { throw "USBPcapCMD not found at $usbpcap" }

# The adapter's root hub, resolved live rather than hard-coded: find the device,
# walk to its parent hub, then match that hub against the USBPcap filter list.
$device = Get-PnpDevice -ErrorAction SilentlyContinue |
          Where-Object { $_.InstanceId -like '*VID_18E1&PID_0104*' } | Select-Object -First 1
if (-not $device) { throw 'MongoosePro adapter not found in PnP. Is the USB cable connected?' }
if ($device.Status -ne 'OK') { throw "Adapter present but not ready: $($device.Status) / $($device.Problem)" }
$parent = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_Parent).Data
Write-Host "Adapter : $($device.InstanceId)"
Write-Host "Root hub: $parent"

if (-not $Interface) {
    # USBPcapCMD -h lists each filter device with the devices beneath it.
    $listing = & $usbpcap -h 2>&1 | Out-String
    $current = $null
    foreach ($line in ($listing -split "`r?`n")) {
        if ($line -match '^\s*(\\\\\.\\USBPcap\d+)') { $current = $Matches[1] }
        if ($current -and $line -match [regex]::Escape($device.InstanceId.Split('\')[-1])) { $Interface = $current; break }
    }
    if (-not $Interface) {
        Write-Warning 'Could not auto-resolve the USBPcap interface. Listing follows; pass -Interface explicitly.'
        Write-Host $listing
        throw 'Unresolved USBPcap interface.'
    }
}
Write-Host "Capture : $Interface"

if (-not $Name) { $Name = [IO.Path]::GetFileNameWithoutExtension($Script) }
$stamp  = Get-Date -Format 'yyyyMMddTHHmmss'
$outDir = Join-Path $repo "analysis\captures\windows\$stamp-$Name"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$pcap = Join-Path $outDir 'wire.pcap'
$log  = Join-Path $outDir 'api.log'
$meta = Join-Path $outDir 'metadata.txt'

# Vendor debug log, if the DebugEnable switch turned out to work.
$drewLogs = 'C:\DrewTech\Logs'
$before = @()
if (Test-Path $drewLogs) { $before = Get-ChildItem $drewLogs -File | Select-Object -ExpandProperty FullName }

Write-Host "Output  : $outDir"
$capture = Start-Process -FilePath $usbpcap -ArgumentList @('-d', $Interface, '-o', $pcap, '-s', '65535') `
                         -PassThru -WindowStyle Hidden
Start-Sleep -Milliseconds $SettleMs   # let the capture attach before any traffic

try {
    & $Harness $Dll --script $Script --gap $Gap 2>&1 | Tee-Object -FilePath $log
    $harnessExit = $LASTEXITCODE
} finally {
    Start-Sleep -Milliseconds $SettleMs   # let trailing frames land
    if (-not $capture.HasExited) { Stop-Process -Id $capture.Id -Force }
}

# Metadata: the plan requires ignition state, firmware version and driver hash
# recorded with every capture set.
$notes = @()
$notes += "capture       = $stamp-$Name"
$notes += "step_script   = $Script"
$notes += "harness_exit  = $harnessExit"
$notes += "gap_ms        = $Gap"
$notes += "usbpcap_iface = $Interface"
$notes += "adapter       = $($device.InstanceId)"
$notes += "root_hub      = $parent"
$notes += "dll           = $Dll"
$notes += "dll_sha256    = $((Get-FileHash $Dll -Algorithm SHA256).Hash)"
$sys = 'C:\Windows\System32\drivers\dtmonpro.sys'
if (Test-Path $sys) { $notes += "sys_sha256    = $((Get-FileHash $sys -Algorithm SHA256).Hash)" }
$notes += "os            = $((Get-CimInstance Win32_OperatingSystem).Caption) $([Environment]::OSVersion.Version)"
$notes += "captured_utc  = $((Get-Date).ToUniversalTime().ToString('o'))"
$notes += 'ignition      = FILL IN (off / on-engine-off / engine-running / bench-no-vehicle)'
$notes += 'vehicle       = FILL IN (e.g. 2017 Volvo XC60 D5 AWD)'
$notes | Set-Content -Path $meta -Encoding UTF8

if (Test-Path $drewLogs) {
    $new = Get-ChildItem $drewLogs -File | Where-Object { $before -notcontains $_.FullName }
    foreach ($file in $new) { Copy-Item $file.FullName (Join-Path $outDir "vendor-$($file.Name)") }
    if ($new) { Write-Host "Vendor log: copied $($new.Count) file(s)" }
}

$size = if (Test-Path $pcap) { (Get-Item $pcap).Length } else { 0 }
Write-Host ''
Write-Host "harness exit = $harnessExit"
Write-Host "wire.pcap    = $size bytes"
if ($size -eq 0) { Write-Warning 'Capture file is empty - the USBPcap filter may not be attached to that hub (reboot after install).' }
Write-Host "Record ignition state and vehicle in $meta"
