<#
.SYNOPSIS
    Build the mafon native libraries (arm64-v8a libmpg123.a + libsonic.so, and
    optionally the libmpg.so MP3Decoder JNI wrapper) with the Android NDK.

.DESCRIPTION
    Drives CMake + Ninja with the NDK's android.toolchain.cmake.

    Outputs (staged by `cmake --install`):
      native\out\arm64-v8a\libmpg123.a          (static, -fPIC, generic C)
      native\out\arm64-v8a\libsonic.so          (org.vinuxproject.sonic JNI)
      native\out\arm64-v8a\include\{mpg123.h,fmt123.h,sonic.h}
      native\out\arm64-v8a\libmpg.so            (only when jni\mpg_jni.c exists)
      native\out\armeabi-v7a\libsonic.so        (+ headers)
      native\out\x86_64\{libmpg123.a,libsonic.so,libmpg.so} (+ headers)

    Android shared libraries are linked with -Wl,-z,max-page-size=16384
    (16 KB page-size compatible for Android 15+; see -MaxPageSize).

    libmpg.so behaviour:
      * native\jni\mpg_jni.c present  -> it is compiled and staged (real wrapper)
      * absent                       -> target still exists, built from a
                                        placeholder that is NOT staged
      * -BuildMpg                    -> force building the placeholder too
      * -SkipMpg                     -> never build the libmpg.so target

.PARAMETER Abi
    Android ABIs to build. Default: arm64-v8a, armeabi-v7a.

.PARAMETER Ndk
    Android NDK root. Default: $env:ANDROID_NDK_ROOT / $env:ANDROID_NDK_HOME,
    or pass -Ndk <path>. NDK r26b was used for the published binaries.

.PARAMETER Api
    Override the Android API level for all ABIs (default: arm64-v8a->21,
    armeabi-v7a->16).

.PARAMETER MaxPageSize
    ELF LOAD alignment for Android shared libraries (default 16384 = 16 KB
    pages, required on Android 15/16 16 KB-page devices; 0 = linker default).

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Clean
    .\build.ps1 -Abi arm64-v8a            # build just the app ABI
    .\build.ps1 -Abi x86_64               # emulator ABI (incl. libmpg.so when JNI source exists)
    .\build.ps1 -BuildMpg                 # also build the placeholder libmpg.so
#>
[CmdletBinding()]
param(
    [ValidateSet('arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86')]
    [string[]]$Abi = @('arm64-v8a', 'armeabi-v7a'),

    # NDK default: $env:ANDROID_NDK_ROOT / $env:ANDROID_NDK_HOME. Override with
    # -Ndk (e.g. -Ndk C:\android-ndk-r26b).
    [string]$Ndk = $(if ($env:ANDROID_NDK_ROOT) { $env:ANDROID_NDK_ROOT }
                     elseif ($env:ANDROID_NDK_HOME) { $env:ANDROID_NDK_HOME }
                     else { '' }),

    [string]$CMake = '',
    [string]$Ninja = '',
    # Python default: $env:MAFON_PYTHON, else `python` from PATH. It is only
    # used to locate pip-installed cmake/ninja (fallback when not on PATH).
    [string]$Python = $(if ($env:MAFON_PYTHON) { $env:MAFON_PYTHON } else { 'python' }),

    [int]$Api = 0,
    # 16 KB page size compatibility for Android 15+/16 (0 = linker default).
    [int]$MaxPageSize = 16384,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$BuildType = 'Release',

    [switch]$Clean,
    [switch]$SkipMpg,
    [switch]$BuildMpg,
    [switch]$NoVerify
)

$ErrorActionPreference = 'Continue'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildRoot = Join-Path $ScriptDir 'build'
$LogRoot   = Join-Path $ScriptDir 'buildlogs'

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host "=== $Text ===" -ForegroundColor Cyan
}
function Write-Ok([string]$Text)   { Write-Host "  [OK]   $Text" -ForegroundColor Green }
function Write-Fail([string]$Text) { Write-Host "  [FAIL] $Text" -ForegroundColor Red }
function Write-Info([string]$Text) { Write-Host "  [info] $Text" -ForegroundColor Gray }

function Resolve-Tool {
    param(
        [string]$Explicit,
        [string]$Name,
        [string[]]$Candidates,
        [string]$InstallHint
    )
    if ($Explicit) {
        if (Test-Path $Explicit) { return (Resolve-Path $Explicit).Path }
        throw "Cannot find '$Name' at '$Explicit'."
    }
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) { return (Resolve-Path $candidate).Path }
    }
    throw "Cannot find '$Name'. $InstallHint"
}

function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments, [string]$LogPath)
    $output = & $Exe @Arguments 2>&1
    $code = $LASTEXITCODE
    if ($LogPath) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $LogPath) | Out-Null
        $output | Out-File -FilePath $LogPath -Encoding utf8
    }
    return [pscustomobject]@{
        ExitCode = $code
        Output   = (($output | Out-String).Trim())
    }
}

# Returns the p_align value of every LOAD program header of an ELF file,
# e.g. @('0x4000','0x4000') for 16 KB page-size-compatible Android libraries.
function Get-LoadAligns {
    param([string]$Path)
    $r = Invoke-Native -Exe (Join-Path $LlvmBin 'llvm-readelf.exe') -Arguments @('-lW', $Path)
    return @($r.Output -split "`n" |
        Where-Object { $_ -match '^\s+LOAD\s' } |
        ForEach-Object { ($_.Trim() -split '\s+')[-1] })
}

# ---------------------------------------------------------------------------
# Tool discovery
# ---------------------------------------------------------------------------
# Python is optional: it is only a hint for locating pip-installed tools.
if (-not ($Python -and (Test-Path $Python))) {
    $pyCmd = Get-Command python -ErrorAction SilentlyContinue
    if ($pyCmd) { $Python = $pyCmd.Source }
}
$PythonDir = ''
if ($Python -and (Test-Path $Python)) {
    $PythonDir = Split-Path -Parent (Resolve-Path $Python).Path
}

$cmakeCandidates = @()
$ninjaCandidates = @()
if ($PythonDir) {
    $cmakeCandidates += (Join-Path $PythonDir 'Scripts\cmake.exe')
    $ninjaCandidates += (Join-Path $PythonDir 'Scripts\ninja.exe')
}
# No hard-coded host hints: cmake/ninja must be on PATH, on the Python Scripts
# directory, or passed explicitly with -CMake / -Ninja.

$CMakeExe = Resolve-Tool -Explicit $CMake -Name 'cmake' `
    -Candidates $cmakeCandidates `
    -InstallHint 'Install it with: & "<python>" -m pip install cmake'
$NinjaExe = Resolve-Tool -Explicit $Ninja -Name 'ninja' `
    -Candidates $ninjaCandidates `
    -InstallHint 'Install it with: & "<python>" -m pip install ninja'

if (-not $Ndk) {
    throw "Android NDK not specified. Pass -Ndk <path> or set ANDROID_NDK_ROOT / ANDROID_NDK_HOME (NDK r26b was used for the published binaries)."
}
if (-not (Test-Path $Ndk)) { throw "Android NDK not found at '$Ndk'." }
$Ndk = (Resolve-Path $Ndk).Path
$ToolchainFile = Join-Path $Ndk 'build\cmake\android.toolchain.cmake'
$LlvmBin = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
if (-not (Test-Path $ToolchainFile)) { throw "NDK toolchain file missing: $ToolchainFile" }
if (-not (Test-Path (Join-Path $LlvmBin 'llvm-readelf.exe'))) {
    throw "LLVM binutils missing under $LlvmBin"
}

$apiFor = @{ 'arm64-v8a' = 21; 'armeabi-v7a' = 16; 'x86_64' = 21; 'x86' = 16 }
$machineFor = @{
    'arm64-v8a'   = @{ ElfClass = 'ELF64'; Machine = 'AArch64'; Format = 'elf64-littleaarch64' }
    'armeabi-v7a' = @{ ElfClass = 'ELF32'; Machine = 'ARM';     Format = 'elf32-littlearm' }
    'x86_64'      = @{ ElfClass = 'ELF64'; Machine = 'X86-64';  Format = 'elf64-x86-64' }
    'x86'         = @{ ElfClass = 'ELF32'; Machine = 'Intel 80386'; Format = 'elf32-i386' }
}

Write-Host "mafon native build" -ForegroundColor White
Write-Info "CMake       : $CMakeExe"
Write-Info "Ninja       : $NinjaExe"
Write-Info "NDK         : $Ndk"
Write-Info "Build type  : $BuildType"
Write-Info "ABIs        : $($Abi -join ', ')"
Write-Info "MaxPageSize : $(if ($MaxPageSize -gt 0) { "$MaxPageSize (16 KB-compatible shared libs)" } else { 'linker default' })"

# ---------------------------------------------------------------------------
# Which targets to build / how to treat the libmpg.so JNI target
# ---------------------------------------------------------------------------
$JniSource = Join-Path $ScriptDir 'jni\mpg_jni.c'
$mpgMode = 'off'
if ($SkipMpg) {
    $mpgMode = 'off'
} elseif (Test-Path $JniSource) {
    $mpgMode = 'real'
} elseif ($BuildMpg) {
    $mpgMode = 'placeholder'
} else {
    $mpgMode = 'off'
}
switch ($mpgMode) {
    'real'        { Write-Info "libmpg.so   : REAL JNI wrapper from $JniSource" }
    'placeholder' { Write-Info "libmpg.so   : placeholder (jni\mpg_jni.c absent, -BuildMpg) - not staged" }
    default       { Write-Info "libmpg.so   : skipped (drop jni\mpg_jni.c here and re-run to build)" }
}

$ExpectedSonicJni = @(
    'initNative', 'closeNative', 'putBytesNative', 'receiveBytesNative',
    'setPitchNative', 'getPitchNative', 'setRateNative', 'getRateNative',
    'getSampleRateNative', 'setSampleRateNative',
    'getNumChannelsNative', 'setNumChannelsNative',
    'getSpeedNative', 'setSpeedNative',
    'getVolumeNative', 'setVolumeNative',
    'getChordPitchNative', 'setChordPitchNative',
    'availableBytesNative', 'flushNative'
)
$RequiredMpg123Symbols = @(
    'mpg123_init', 'mpg123_new', 'mpg123_open', 'mpg123_getformat',
    'mpg123_format_none', 'mpg123_format', 'mpg123_read', 'mpg123_seek',
    'mpg123_length', 'mpg123_outblock', 'mpg123_plain_strerror',
    'mpg123_feature', 'mpg123_rates', 'mpg123_encodings'
)

$failed = $false
$built = New-Object System.Collections.ArrayList

# ---------------------------------------------------------------------------
# Build loop
# ---------------------------------------------------------------------------
foreach ($abiName in $Abi) {
    $apiLevel = if ($Api -gt 0) { $Api } else { $apiFor[$abiName] }
    $buildDir = Join-Path $BuildRoot $abiName
    $outDir   = Join-Path (Join-Path $ScriptDir 'out') $abiName
    $expect   = $machineFor[$abiName]

    Write-Step "$abiName (android-$apiLevel)"

    if ($Clean) {
        Write-Info "removing $buildDir and $outDir"
        Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
        Remove-Item -Recurse -Force $outDir   -ErrorAction SilentlyContinue
    }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    # 64-bit ABIs get the static libmpg123 (and may build libmpg.so); the v7
    # helper build only needs libsonic.
    $buildMpg123 = if ($abiName -in @('arm64-v8a', 'x86_64')) { 'ON' } else { 'OFF' }
    $thisMpgMode = if ($buildMpg123 -eq 'ON') { $mpgMode } else { 'off' }
    if ($buildMpg123 -ne 'ON' -and $mpgMode -ne 'off') {
        Write-Info "libmpg123/libmpg.so are only built for 64-bit ABIs; skipping for $abiName"
    }
    $buildMpgTarget = if ($thisMpgMode -eq 'off') { 'OFF' } else { 'ON' }

    $configureArgs = @(
        '-G', 'Ninja',
        '-S', $ScriptDir,
        '-B', $buildDir,
        "-DCMAKE_TOOLCHAIN_FILE=$ToolchainFile",
        "-DANDROID_ABI=$abiName",
        "-DANDROID_PLATFORM=android-$apiLevel",
        "-DCMAKE_BUILD_TYPE=$BuildType",
        "-DCMAKE_MAKE_PROGRAM=$NinjaExe",
        "-DCMAKE_INSTALL_PREFIX=$outDir",
        "-DMAFON_BUILD_MPG123=$buildMpg123",
        "-DMAFON_BUILD_SONIC=ON",
        "-DMAFON_BUILD_MPG=$buildMpgTarget",
        "-DMAFON_MAX_PAGE_SIZE=$MaxPageSize"
    )

    $logBase = Join-Path (Join-Path $LogRoot $abiName) 'build'
    $res = Invoke-Native -Exe $CMakeExe -Arguments $configureArgs -LogPath "$logBase-configure.log"
    if ($res.ExitCode -ne 0) {
        Write-Fail "configure failed (exit $($res.ExitCode)); see $logBase-configure.log"
        $res.Output -split "`n" | Select-Object -Last 25 | ForEach-Object { Write-Host "    $_" }
        $failed = $true
        continue
    }
    Write-Ok "configure"

    $res = Invoke-Native -Exe $CMakeExe -Arguments @('--build', $buildDir, '--config', $BuildType) -LogPath "$logBase-compile.log"
    if ($res.ExitCode -ne 0) {
        Write-Fail "build failed (exit $($res.ExitCode)); see $logBase-compile.log"
        $res.Output -split "`n" | Select-Object -Last 30 | ForEach-Object { Write-Host "    $_" }
        $failed = $true
        continue
    }
    Write-Ok "compile + link"

    $res = Invoke-Native -Exe $CMakeExe -Arguments @('--install', $buildDir) -LogPath "$logBase-install.log"
    if ($res.ExitCode -ne 0) {
        Write-Fail "install/stage failed (exit $($res.ExitCode)); see $logBase-install.log"
        $failed = $true
        continue
    }
    Write-Ok "stage -> $outDir"

    if ($NoVerify) { continue }

    # ---------------------------------------------------------------
    # Verification
    # ---------------------------------------------------------------
    $sonicSo = Join-Path $outDir 'libsonic.so'
    if (Test-Path $sonicSo) {
        $h = Invoke-Native -Exe (Join-Path $LlvmBin 'llvm-readelf.exe') -Arguments @('-h', $sonicSo)
        $d = Invoke-Native -Exe (Join-Path $LlvmBin 'llvm-readelf.exe') -Arguments @('-d', $sonicSo)
        $n = Invoke-Native -Exe (Join-Path $LlvmBin 'llvm-nm.exe') -Arguments @('-D', '--defined-only', $sonicSo)

        if ($h.Output -match "Class:\s+$($expect.ElfClass)" -and
            $h.Output -match "Machine:\s+.*$($expect.Machine)\b") {
            Write-Ok "libsonic.so ELF: $($expect.ElfClass) / $($expect.Machine)"
        } else {
            Write-Fail "libsonic.so has unexpected ELF header"
            $failed = $true
        }
        if ($d.Output -match 'SONAME.*\[libsonic\.so\]') {
            Write-Ok "libsonic.so SONAME=libsonic.so; NEEDED=$(($d.Output -split "`n" | Select-String 'NEEDED.*\[(.+?)\]' | ForEach-Object { $_.Matches[0].Groups[1].Value }) -join ', ')"
        } else {
            Write-Fail "libsonic.so SONAME missing"
            $failed = $true
        }
        if ($MaxPageSize -gt 0) {
            $wantAlign = '0x{0:x}' -f $MaxPageSize
            $aligns = @(Get-LoadAligns $sonicSo)
            if ($aligns.Count -gt 0 -and @($aligns | Where-Object { $_ -ne $wantAlign }).Count -eq 0) {
                Write-Ok "libsonic.so LOAD p_align=$wantAlign ($([int]($MaxPageSize/1024)) KB page compatible)"
            } else {
                Write-Fail "libsonic.so LOAD p_align is not $wantAlign (got: $($aligns -join ', '))"
                $failed = $true
            }
        }
        $missing = @()
        foreach ($sym in $ExpectedSonicJni) {
            if ($n.Output -notmatch "Java_org_vinuxproject_sonic_Sonic_$sym\b") { $missing += $sym }
        }
        if ($missing.Count -eq 0) {
            Write-Ok "libsonic.so exports all $($ExpectedSonicJni.Count) Java_org_vinuxproject_sonic_Sonic_* symbols"
        } else {
            Write-Fail "libsonic.so missing JNI symbols: $($missing -join ', ')"
            $failed = $true
        }
    } else {
        Write-Fail "libsonic.so was not produced"
        $failed = $true
    }

    if ($buildMpg123 -eq 'ON') {
        $mpg123Lib = Join-Path $outDir 'libmpg123.a'
        if (Test-Path $mpg123Lib) {
            $f = Invoke-Native -Exe (Join-Path $LlvmBin 'llvm-objdump.exe') -Arguments @('-f', $mpg123Lib)
            $loaded = @($f.Output -split "`n" | Select-String 'file format\s+(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object -Unique)
            if ($loaded.Count -eq 1 -and $loaded[0] -eq $expect.Format) {
                Write-Ok "libmpg123.a members: all $($expect.Format)"
            } else {
                Write-Fail "libmpg123.a mixes formats: $($loaded -join ', ')"
                $failed = $true
            }
            $nm = Invoke-Native -Exe (Join-Path $LlvmBin 'llvm-nm.exe') -Arguments @('--defined-only', $mpg123Lib)
            $miss = @()
            foreach ($sym in $RequiredMpg123Symbols) {
                if ($nm.Output -notmatch " T $sym\b") { $miss += $sym }
            }
            if ($miss.Count -eq 0) {
                Write-Ok "libmpg123.a defines the full required mpg123_* API ($($RequiredMpg123Symbols.Count) symbols checked)"
            } else {
                Write-Fail "libmpg123.a missing: $($miss -join ', ')"
                $failed = $true
            }
        } else {
            Write-Fail "libmpg123.a was not produced"
            $failed = $true
        }
    }

    $mpgSo = Join-Path $outDir 'libmpg.so'
    if ($thisMpgMode -eq 'real' -and (Test-Path $mpgSo)) {
        $h = Invoke-Native -Exe (Join-Path $LlvmBin 'llvm-readelf.exe') -Arguments @('-h', $mpgSo)
        $n = Invoke-Native -Exe (Join-Path $LlvmBin 'llvm-nm.exe') -Arguments @('-D', '--defined-only', $mpgSo)
        $u = Invoke-Native -Exe (Join-Path $LlvmBin 'llvm-nm.exe') -Arguments @('-D', '--undefined-only', $mpgSo)
        if ($h.Output -match "Machine:\s+.*$($expect.Machine)\b") {
            Write-Ok "libmpg.so ELF: $($expect.ElfClass) / $($expect.Machine)"
        } else {
            Write-Fail "libmpg.so has unexpected ELF header"
            $failed = $true
        }
        if ($MaxPageSize -gt 0) {
            $wantAlign = '0x{0:x}' -f $MaxPageSize
            $aligns = @(Get-LoadAligns $mpgSo)
            if ($aligns.Count -gt 0 -and @($aligns | Where-Object { $_ -ne $wantAlign }).Count -eq 0) {
                Write-Ok "libmpg.so LOAD p_align=$wantAlign ($([int]($MaxPageSize/1024)) KB page compatible)"
            } else {
                Write-Fail "libmpg.so LOAD p_align is not $wantAlign (got: $($aligns -join ', '))"
                $failed = $true
            }
        }
        if ($n.Output -match ' T Java_nobleworks_libmpg_MP3Decoder_') {
            Write-Ok "libmpg.so exports MP3Decoder JNI symbols"
        } else {
            Write-Fail "libmpg.so does not export Java_nobleworks_libmpg_MP3Decoder_* (is mpg_jni.c the right wrapper?)"
            $failed = $true
        }
        $und = @($u.Output -split "`n" | Select-String 'mpg123_')
        if ($und.Count -eq 0) {
            Write-Ok "libmpg.so has no unresolved mpg123_* symbols (static lib linked in)"
        } else {
            Write-Fail "libmpg.so has unresolved mpg123_* symbols"
            $failed = $true
        }
    }

    foreach ($hdr in @('mpg123.h', 'fmt123.h', 'sonic.h')) {
        if (Test-Path (Join-Path (Join-Path $outDir 'include') $hdr)) {
            Write-Ok "staged header include\$hdr"
        } else {
            Write-Fail "missing staged header include\$hdr"
            $failed = $true
        }
    }

    foreach ($artifact in @('libsonic.so', 'libmpg123.a', 'libmpg.so')) {
        $p = Join-Path $outDir $artifact
        if (Test-Path $p) {
            $null = $built.Add([pscustomobject]@{
                Abi = $abiName; File = $artifact
                Bytes = (Get-Item $p).Length
                Path = $p
            })
        }
    }
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Step 'Summary'
if ($built.Count -gt 0) {
    $built | Format-Table Abi, File, @{ n = 'Bytes'; e = { '{0:N0}' -f $_.Bytes } }, Path -AutoSize |
        Out-String | Write-Host
}

if ($failed) {
    Write-Host 'BUILD FAILED' -ForegroundColor Red
    exit 1
}
Write-Host 'BUILD SUCCEEDED' -ForegroundColor Green
Write-Host ''
Write-Host 'Artifacts are staged under: '
foreach ($abiName in $Abi) { Write-Host ("  " + (Join-Path (Join-Path $ScriptDir 'out') $abiName)) }
exit 0

