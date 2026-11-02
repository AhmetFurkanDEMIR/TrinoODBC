<#
.SYNOPSIS
    Registers (or unregisters) trinoodbc.dll with the Windows ODBC Driver Manager without an installer.

.DESCRIPTION
    Writes the same registry entries as packaging/windows/installer.iss:
      HKLM\SOFTWARE\ODBC\ODBCINST.INI\ODBC Drivers      <Name> = Installed
      HKLM\SOFTWARE\ODBC\ODBCINST.INI\<Name>            Driver, Setup, APILevel=2, ConnectFunctions=YYN,
                                                        DriverODBCVer=03.80, FileUsage=0, SQLLevel=1, UsageCount=1
    A 32-bit DLL on 64-bit Windows is registered under the WOW6432Node view (what the 32-bit ODBC
    Administrator and 32-bit applications read). Running the script twice is safe: values are
    overwritten, nothing is duplicated. Requires an elevated (Administrator) PowerShell.

.PARAMETER DriverPath
    Path to trinoodbc.dll (default: trinoodbc.dll next to this script, then ..\..\dist\windows-x64).
.PARAMETER Name
    Driver name shown in the ODBC Administrator (default: "Trino ODBC Driver").
.PARAMETER Unregister
    Remove the registration instead of adding it.
.PARAMETER Description
    Optional description stored with the driver entry.

.EXAMPLE
    .\register_driver.ps1 -DriverPath C:\TrinoODBC\trinoodbc.dll
.EXAMPLE
    .\register_driver.ps1 -Unregister
#>
[CmdletBinding()]
param(
    [string]$DriverPath = "",
    [string]$Name = "Trino ODBC Driver",
    [switch]$Unregister,
    [string]$Description = "Trino ODBC Driver (ODBC 3.8, Unicode)"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# Reads the PE header to find out whether the DLL is 32-bit (needs the WOW6432Node registry view).
function Get-PeMachine([string]$Path) {
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $reader = New-Object System.IO.BinaryReader($stream)
        if ($stream.Length -lt 0x40) { throw "$Path is not a PE file" }
        $stream.Seek(0x3C, [System.IO.SeekOrigin]::Begin) | Out-Null
        $peOffset = $reader.ReadUInt32()
        if ($peOffset + 6 -gt $stream.Length) { throw "$Path is not a PE file" }
        $stream.Seek($peOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "$Path is not a PE file (missing PE signature)" }
        return $reader.ReadUInt16()
    } finally {
        $stream.Dispose()
    }
}

if (-not (Test-IsAdministrator)) {
    throw "This script must run in an elevated PowerShell (Run as Administrator): ODBC drivers live under HKLM."
}
if ($Name.Trim().Length -eq 0) { throw "-Name must not be empty" }
# Same minimum as installer.iss (MinVersion=10.0.18362): the driver's time zone support needs the
# combined System32 icu.dll that Windows ships since Windows 10 1903.
if (-not $Unregister -and [Environment]::OSVersion.Version -lt [Version]'10.0.18362') {
    Write-Warning ("Windows $([Environment]::OSVersion.Version) is older than the supported minimum (Windows 10 1903, build 18362): " +
                   "IANA time zone names may not resolve and the session time zone may fall back to the server's.")
}

if ($DriverPath -eq "") {
    $candidates = @(
        (Join-Path $PSScriptRoot 'trinoodbc.dll'),
        (Join-Path $PSScriptRoot '..\..\dist\windows-x64\trinoodbc.dll'),
        (Join-Path $PSScriptRoot '..\..\dist\windows-x86\trinoodbc.dll'),
        (Join-Path $PSScriptRoot '..\..\dist\windows-arm64\trinoodbc.dll')
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { $DriverPath = $c; break }
    }
    if ($DriverPath -eq "" -and -not $Unregister) {
        throw "trinoodbc.dll not found; pass -DriverPath"
    }
}

$use32BitView = $false
if ($DriverPath -ne "") {
    if (-not $Unregister -and -not (Test-Path -LiteralPath $DriverPath)) {
        throw "Driver file not found: $DriverPath"
    }
    if (Test-Path -LiteralPath $DriverPath) {
        $DriverPath = (Resolve-Path -LiteralPath $DriverPath).Path
        # IMAGE_FILE_MACHINE_I386 = 0x014C: a 32-bit driver on 64-bit Windows lives in the WOW6432Node view.
        $machine = Get-PeMachine $DriverPath
        $use32BitView = ($machine -eq 0x014C) -and [Environment]::Is64BitOperatingSystem
    }
}

$baseKey = if ($use32BitView) { 'HKLM:\SOFTWARE\WOW6432Node\ODBC\ODBCINST.INI' } else { 'HKLM:\SOFTWARE\ODBC\ODBCINST.INI' }
$driversKey = Join-Path $baseKey 'ODBC Drivers'
$driverKey = Join-Path $baseKey $Name

if ($Unregister) {
    $removed = $false
    foreach ($view in @('HKLM:\SOFTWARE\ODBC\ODBCINST.INI', 'HKLM:\SOFTWARE\WOW6432Node\ODBC\ODBCINST.INI')) {
        $dk = Join-Path $view $Name
        $lk = Join-Path $view 'ODBC Drivers'
        if (Test-Path -LiteralPath $dk) {
            Remove-Item -LiteralPath $dk -Recurse -Force
            $removed = $true
        }
        if ((Test-Path -LiteralPath $lk) -and ($null -ne (Get-ItemProperty -LiteralPath $lk -Name $Name -ErrorAction SilentlyContinue))) {
            Remove-ItemProperty -LiteralPath $lk -Name $Name -Force
            $removed = $true
        }
    }
    if ($removed) {
        Write-Host "Unregistered ODBC driver '$Name'."
    } else {
        Write-Host "ODBC driver '$Name' was not registered; nothing to do."
    }
    exit 0
}

# ---- register ---------------------------------------------------------------------------------------
foreach ($key in @($baseKey, $driversKey, $driverKey)) {
    if (-not (Test-Path -LiteralPath $key)) { New-Item -Path $key -Force | Out-Null }
}
New-ItemProperty -LiteralPath $driversKey -Name $Name -Value 'Installed' -PropertyType String -Force | Out-Null

$values = [ordered]@{
    'Driver'           = $DriverPath
    'Setup'            = $DriverPath
    'Description'      = $Description
    'APILevel'         = '2'
    'ConnectFunctions' = 'YYN'
    'DriverODBCVer'    = '03.80'
    'FileUsage'        = '0'
    'SQLLevel'         = '1'
}
foreach ($entry in $values.GetEnumerator()) {
    New-ItemProperty -LiteralPath $driverKey -Name $entry.Key -Value $entry.Value -PropertyType String -Force | Out-Null
}
New-ItemProperty -LiteralPath $driverKey -Name 'UsageCount' -Value 1 -PropertyType DWord -Force | Out-Null

$view = if ($use32BitView) { '32-bit (WOW6432Node)' } else { 'native' }
Write-Host "Registered ODBC driver '$Name' -> $DriverPath ($view registry view)."
Write-Host "Create a DSN with the ODBC Data Source Administrator, or connect with:"
Write-Host "  Driver={$Name};Host=<coordinator>;Port=8080;UID=<user>;Catalog=<catalog>"
