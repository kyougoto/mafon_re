# Sign the patched Mafon APK (zipalign + apksigner, v1+v2).
#
# Usage:
#   .\tools\sign_apk.ps1 -In build\mafon-1.0-arm64-unsigned.apk `
#                        -Out dist\mafon-1.0-arm64.apk
#
# Requires JDK 17+ and Android build-tools 34 (apksigner, zipalign).
# The keystore in tools\mafon_re.jks is a public, non-secret test key that is
# committed to this repository so that anybody can reproduce a byte-identical
# signing configuration and publish update-compatible re-signs.
param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Keystore = '',
    [string]$StorePass = 'mafon_re',
    [string]$Alias = 'mafon',
    [string]$Zipalign = $env:ZIPALIGN,
    [string]$Apksigner = $env:APKSIGNER,
    [string]$JavaHome = $env:JAVA_HOME
)

$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
if (-not $Keystore) { $Keystore = Join-Path $scriptDir 'mafon_re.jks' }

if (-not $JavaHome) {
    throw 'JAVA_HOME is not set. Point it at a JDK 17+ installation.'
}
if (-not (Test-Path $Keystore)) { throw "keystore not found: $Keystore" }
$env:JAVA_HOME = $JavaHome
$env:PATH = (Join-Path $JavaHome 'bin') + ';' + $env:PATH

if (-not $Zipalign) {
    throw 'ZIPALIGN is not set. Point it at Android build-tools zipalign.exe'
}
if (-not $Apksigner) {
    throw 'APKSIGNER is not set. Point it at Android build-tools apksigner.bat'
}

$work = [System.IO.Path]::GetDirectoryName([System.IO.Path]::GetFullPath($Out))
New-Item -ItemType Directory -Force -Path $work | Out-Null
$aligned = Join-Path $work ('mafon-aligned-' + [guid]::NewGuid().ToString('N') + '.apk')

try {
    # 4-byte alignment for the zip entries; -p also page-aligns uncompressed .so files.
    & $Zipalign -f -p 4 $In $aligned
    if ($LASTEXITCODE -ne 0) { throw "zipalign failed with exit code $LASTEXITCODE" }

    & $Apksigner sign `
        --ks $Keystore `
        --ks-pass ("pass:" + $StorePass) `
        --key-pass ("pass:" + $StorePass) `
        --ks-key-alias $Alias `
        --v1-signing-enabled true `
        --v2-signing-enabled true `
        --v3-signing-enabled true `
        --v4-signing-enabled false `
        --out $Out $aligned
    if ($LASTEXITCODE -ne 0) { throw "apksigner sign failed with exit code $LASTEXITCODE" }

    & $Apksigner verify --verbose --print-certs $Out
    if ($LASTEXITCODE -ne 0) { throw "apksigner verify failed with exit code $LASTEXITCODE" }

    & $Zipalign -c -p 4 $Out
    if ($LASTEXITCODE -ne 0) { throw "zipalign check failed with exit code $LASTEXITCODE" }

    Write-Host ("signed APK: {0} ({1:N0} bytes)" -f $Out, (Get-Item $Out).Length)
}
finally {
    Remove-Item -Force $aligned -ErrorAction SilentlyContinue
}
