[CmdletBinding()]
Param(
    [Parameter(Mandatory)]
    [ValidateNotNullOrEmpty()]
    [string]
    $Tool,

    [Parameter()]
    [string]
    $WindowsKitPath
)

$ErrorActionPreference = 'Stop'

if (-not $WindowsKitPath) {
    $winSdk10Root = Get-ItemPropertyValue -Path 'HKLM:\Software\Microsoft\Windows Kits\Installed Roots' -Name 'KitsRoot10'
    $WindowsKitPath = Join-Path $winSdk10Root 'bin\10.0.26100.0'
}

if (-not (Test-Path -LiteralPath $WindowsKitPath -PathType Container)) {
    throw "Could not find Windows SDK 10.0.26100.0 at `"$WindowsKitPath`"."
}

$architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString().ToLowerInvariant()
if ($architecture -notin @('arm64', 'x64', 'x86')) {
    throw "Windows SDK tools are not supported on OS architecture '$architecture'."
}

$toolPath = Join-Path $WindowsKitPath "$architecture\$Tool"
if (-not (Test-Path -LiteralPath $toolPath -PathType Leaf)) {
    throw "Could not find the native $architecture Windows SDK tool at `"$toolPath`"."
}

$toolPath
