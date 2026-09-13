<#
.SYNOPSIS
One experiment = one stimulus, three synchronised records.

Starts a USBPcap capture, runs one step script through mongoose-reference.exe,
stops the capture, and snapshots the vendor debug log if it is active.
Everything for a run lands in analysis/captures/windows/<timestamp>-<name>/.

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
    # USBPcap can take seconds to attach. At 1500 ms a capture recorded only the
    # close and silently missed the entire open sequence, so this is deliberately
    # generous - see docs/WINDOWS-FINDINGS.md.
    [int]$SettleMs = 5000
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not (Test-Path $Script))  { throw "Step script not found: $Script" }
if (-not (Test-Path $Harness)) { throw "Harness not built: $Harness (see docs/CAPTURING.md)" }
if (-not (Test-Path $Dll))     { throw "Vendor DLL not found: $Dll" }

$usbpcap = 'C:\Program Files\USBPcap\USBPcapCMD.exe'
if (-not (Test-Path $usbpcap)) { throw "USBPcapCMD not found at $usbpcap" }

# USBPcap capture needs no elevation here, but the driver must have attached to
# the root hubs, which only happens on the boot after install.
#
# The candidate list is generated rather than read from --extcap-interfaces:
# that switch prints nothing when USBPcapCMD is invoked from a script file.
# USBPcap numbers its control devices one per root hub, so probe exactly that
# many - running more capture processes than there are hubs makes them contend
# and several then record nothing at all.
$cache = Join-Path $repo 'build\windows\usbpcap-interface.txt'
$hubs  = @(Get-PnpDevice -ErrorAction SilentlyContinue |
           Where-Object { $_.InstanceId -like 'USB\ROOT_HUB*' }).Count
if ($hubs -lt 1) { $hubs = 4 }
$ifaces = 1..$hubs | ForEach-Object { '\\.\USBPcap' + $_ }
if (-not $Interface -and (Test-Path $cache)) {
    $remembered = (Get-Content $cache -Raw).Trim()
    if ($remembered) { $Interface = $remembered }
}
if ($Interface) { $ifaces = @($Interface) }

$device = Get-PnpDevice -ErrorAction SilentlyContinue |
          Where-Object { $_.InstanceId -like '*VID_18E1&PID_0104*' } | Select-Object -First 1
if (-not $device) { throw 'MongoosePro adapter not found in PnP. Is the USB cable connected?' }
if ($device.Status -ne 'OK') { throw "Adapter present but not ready: $($device.Status) / $($device.Problem)" }
$parent = (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName DEVPKEY_Device_Parent).Data
Write-Host "Adapter : $($device.InstanceId)"
Write-Host "Root hub: $parent"

if (-not $Name) { $Name = [IO.Path]::GetFileNameWithoutExtension($Script) }
$stamp  = Get-Date -Format 'yyyyMMddTHHmmss'
$outDir = Join-Path $repo "analysis\captures\windows\$stamp-$Name"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$log  = Join-Path $outDir 'api.log'
$meta = Join-Path $outDir 'metadata.txt'

# Vendor debug log, if the DebugEnable switch turned out to work.
$drewLogs = 'C:\DrewTech\Logs'
$before = @()
if (Test-Path $drewLogs) { $before = Get-ChildItem $drewLogs -File | Select-Object -ExpandProperty FullName }

# Capture every root hub at once rather than trusting a remembered hub index:
# USB enumeration order can change across a reboot, and an empty pcap is
# indistinguishable from a quiet one. Only the hub that saw traffic is kept.
# -A is required; without a device selection USBPcap captures nothing at all.
Write-Host "Output  : $outDir"
$captures = @()
foreach ($iface in $ifaces) {
    $tag  = ($iface -replace '[^A-Za-z0-9]', '')
    $file = Join-Path $outDir "raw-$tag.pcap"
    $psi = New-Object Diagnostics.ProcessStartInfo
    $psi.FileName  = $usbpcap
    $psi.Arguments = "-d `"$iface`" -o `"$file`" -s 65535 -A"
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow  = $true
    $captures += @{ iface = $iface; file = $file; proc = [Diagnostics.Process]::Start($psi) }
}
Start-Sleep -Milliseconds $SettleMs   # let the filters attach before any traffic

$harnessExit = -1
try {
    & $Harness $Dll --script $Script --gap $Gap 2>&1 |
        Tee-Object -Variable harnessOutput |
        Write-Host
    $harnessExit = $LASTEXITCODE
} finally {
    Start-Sleep -Milliseconds $SettleMs   # let trailing frames land
    foreach ($c in $captures) {
        if (-not $c.proc.HasExited) { $c.proc.Kill(); $c.proc.WaitForExit(3000) | Out-Null }
    }
}
# Written explicitly as UTF-8: Tee-Object -FilePath under Windows PowerShell
# produces UTF-16, which the correlator would have to special-case.
$harnessOutput | Out-File -FilePath $log -Encoding utf8

# A pcap holding only the 24-byte global header saw no packets.
$sizes = @()
$kept  = $null
foreach ($c in $captures) {
    $size = if (Test-Path $c.file) { (Get-Item $c.file).Length } else { 0 }
    $sizes += "$($c.iface) = $size bytes"
    $err = $c.proc.StandardError.ReadToEnd().Trim()
    if ($err) { $sizes += "  stderr: $($err -replace "`r?`n", ' | ')" }
    if ($size -gt 24) {
        if (-not $kept -or $size -gt (Get-Item $kept).Length) { $kept = $c.file }
    } elseif (Test-Path $c.file) {
        Remove-Item $c.file -Force
    }
}
$wire = Join-Path $outDir 'wire.pcap'
if ($kept) {
    # Remember which hub the adapter is on so later runs use a single capture
    # process. USB enumeration order can change, so a miss re-probes every hub.
    $winner = ($captures | Where-Object { $_.file -eq $kept }).iface
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $cache) | Out-Null
    Set-Content -Path $cache -Value $winner -Encoding ASCII
    Move-Item $kept $wire -Force
} elseif (Test-Path $cache) {
    Remove-Item $cache -Force   # cached hub went quiet; re-probe next run
}
Get-ChildItem $outDir -Filter 'raw-*.pcap' -ErrorAction SilentlyContinue |
    ForEach-Object { Write-Host "Also kept $($_.Name) ($($_.Length) bytes) - more than one hub saw traffic" }

$notes = @()
$notes += "capture       = $stamp-$Name"
$notes += "step_script   = $Script"
$notes += "harness_exit  = $harnessExit"
$notes += "gap_ms        = $Gap"
$notes += "usbpcap_ifaces= $($ifaces -join ' ')"
foreach ($s in $sizes) { $notes += "  $s" }
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

Write-Host ''
Write-Host "harness exit = $harnessExit"
foreach ($s in $sizes) { Write-Host "  $s" }
if (Test-Path $wire) {
    Write-Host "wire.pcap    = $((Get-Item $wire).Length) bytes"
} else {
    Write-Warning 'No hub saw any packets. Check that the adapter is connected and that -A reached USBPcap.'
}
Write-Host "Record ignition state and vehicle in $meta"
