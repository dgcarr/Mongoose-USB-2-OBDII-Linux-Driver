# Run alongside the Win32 harness; this script only reads configuration.
$ErrorActionPreference = 'Stop'
Write-Output "UTC: $([DateTime]::UtcNow.ToString('o'))"
Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, OSArchitecture
Get-PnpDevice | Where-Object { $_.InstanceId -like 'USB\VID_18E1&PID_0104*' } | Format-List *
$roots = @('HKLM:\SOFTWARE\PassThruSupport.04.04', 'HKLM:\SOFTWARE\WOW6432Node\PassThruSupport.04.04')
foreach ($root in $roots) {
    if (Test-Path $root) {
        Get-ChildItem $root | ForEach-Object {
            $entry = Get-ItemProperty $_.PSPath
            if (($entry | Out-String) -match 'Mongoose|monpj') { $entry | Format-List * }
        }
    }
}
