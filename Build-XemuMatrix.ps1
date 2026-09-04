[CmdletBinding()]
param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$SourceZip,

    [ValidateSet('All','Windows','Linux','MacOS')]
    [string]$Target = 'All',

    [string]$BuildSelection = '',

    [switch]$InteractiveSelect,

    [switch]$OutputPatchedSource,

    [string]$DebugToolsProfile = '',

    [string]$Version = '0.0.0-0-unofficial-local',

    [string]$OutputDir = '',

    [string]$MacHost = '',

    [switch]$SkipPdb,
    [switch]$PlanOnly,
    [switch]$KeepWork
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$HostIsWindows = ($env:OS -eq 'Windows_NT')
$HostIsMacOS = $false
if (-not $HostIsWindows) {
    try { $HostIsMacOS = ((& uname -s 2>$null) -eq 'Darwin') } catch { $HostIsMacOS = $false }
}

function Write-Step([string]$Text) {
    Write-Host "`n==> $Text" -ForegroundColor Cyan
}

$script:BuildResults = New-Object System.Collections.ArrayList
$script:DockerReady = $true
$script:DockerExe = 'docker'
$script:DebugToolsOverlayName = 'CMP-Official-Debug-Tools.zip'
$script:ValidDebugToolsProfiles = @('main','main+hdd','main+memory','full')

function Add-BuildResult(
    [string]$Platform,
    [string]$Arch,
    [string]$Config,
    [string]$Stage,
    [string]$Status,
    [string]$Message = ''
) {
    [void]$script:BuildResults.Add([pscustomobject]@{
        Platform = $Platform
        Arch = $Arch
        Configuration = $Config
        Stage = $Stage
        Status = $Status
        Message = $Message
    })
}

function Invoke-MatrixJob(
    [string]$Platform,
    [string]$Arch,
    [string]$Config,
    [string]$Stage,
    [scriptblock]$Action
) {
    try {
        & $Action
        Add-BuildResult $Platform $Arch $Config $Stage 'PASSED'
        return $true
    } catch {
        $message = $_.Exception.Message
        Add-BuildResult $Platform $Arch $Config $Stage 'FAILED' $message
        Write-Warning "$Platform $Arch $Config $Stage FAILED: $message"
        return $false
    }
}

function Write-BuildSummary([string]$OutRoot) {
    Write-Step 'Final build summary'

    if ($script:BuildResults.Count -eq 0) {
        Write-Host 'No build jobs were run.'
        return 0
    }

    $script:BuildResults | Format-Table Platform, Arch, Configuration, Stage, Status, Message -AutoSize | Out-Host

    $passed = @($script:BuildResults | Where-Object Status -eq 'PASSED').Count
    $failed = @($script:BuildResults | Where-Object Status -eq 'FAILED').Count
    $skipped = @($script:BuildResults | Where-Object Status -eq 'SKIPPED').Count

    Write-Host "Passed : $passed"
    Write-Host "Failed : $failed"
    Write-Host "Skipped: $skipped"

    $resultsPath = Join-Path $OutRoot 'build-results.json'
    @($script:BuildResults) | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
    Write-Host "Results: $resultsPath"

    if ($failed -gt 0) { return 1 }
    return 0
}

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Required command '$Name' was not found."
    }
}

function Invoke-External([string]$Exe, [string[]]$Arguments) {
    Write-Host ("> " + $Exe + " " + ($Arguments -join ' ')) -ForegroundColor DarkGray
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Exe failed with exit code $LASTEXITCODE"
    }
}

function Invoke-ExternalLogged(
    [string]$Exe,
    [string[]]$Arguments,
    [string]$LogPath
) {
    $logDir = Split-Path -Parent $LogPath
    if ($logDir) {
        New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    }

    Write-Host ("> " + $Exe + " " + ($Arguments -join ' ')) -ForegroundColor DarkGray
    Write-Host "Build log: $LogPath" -ForegroundColor DarkGray

    # Native tools such as Docker legitimately use stderr for informational
    # output even when they exit successfully. Windows PowerShell 5.1 turns
    # redirected native stderr into ErrorRecord objects; with the builder-wide
    # $ErrorActionPreference='Stop', a harmless line (for example debconf's
    # "delaying package configuration" notice) can otherwise terminate the job.
    # Temporarily allow native stderr while streaming stdout+stderr, then decide
    # success strictly from the native process exit code.
    $previousErrorActionPreference = $ErrorActionPreference
    $hasNativeErrorPreference = Test-Path Variable:\PSNativeCommandUseErrorActionPreference
    $previousNativeErrorPreference = $null
    if ($hasNativeErrorPreference) {
        $previousNativeErrorPreference = $PSNativeCommandUseErrorActionPreference
    }

    $exitCode = $null
    $invokeException = $null
    try {
        $ErrorActionPreference = 'Continue'
        if ($hasNativeErrorPreference) {
            $PSNativeCommandUseErrorActionPreference = $false
        }

        try {
            & $Exe @Arguments 2>&1 | Tee-Object -FilePath $LogPath
            $exitCode = $LASTEXITCODE
        } catch {
            $invokeException = $_
        }
    } finally {
        if ($hasNativeErrorPreference) {
            $PSNativeCommandUseErrorActionPreference = $previousNativeErrorPreference
        }
        $ErrorActionPreference = $previousErrorActionPreference
    }

    $footer = New-Object System.Collections.ArrayList
    [void]$footer.Add('')
    [void]$footer.Add('===== NATIVE COMMAND RESULT =====')
    [void]$footer.Add("Executable: $Exe")
    if ($null -ne $exitCode) {
        [void]$footer.Add("Exit code: $exitCode")
    } else {
        [void]$footer.Add('Exit code: <not available>')
    }
    if ($null -ne $invokeException) {
        [void]$footer.Add("PowerShell invocation error: $($invokeException.Exception.Message)")
    }
    [void]$footer.Add('=================================')
    $footer | Add-Content -LiteralPath $LogPath
    foreach ($line in $footer) {
        Write-Host $line -ForegroundColor DarkGray
    }

    if ($null -ne $invokeException) {
        throw "$Exe could not be executed. Full log: $LogPath`n$($invokeException.Exception.Message)"
    }

    if ($exitCode -ne 0) {
        $tail = @()
        try {
            $tail = @(Get-Content -LiteralPath $LogPath -Tail 30 -ErrorAction Stop)
        } catch {
            $tail = @()
        }
        $tailText = if ($tail.Count -gt 0) { ($tail -join "`n") } else { '<no captured output>' }
        throw "$Exe failed with exit code $exitCode. Full log: $LogPath`nLast output:`n$tailText"
    }
}

function Get-DockerCliPath {
    $cmd = Get-Command 'docker' -ErrorAction SilentlyContinue
    if ($cmd) {
        if ($cmd.Source) { return $cmd.Source }
        return 'docker'
    }

    if (-not $HostIsWindows) { return $null }

    $candidates = New-Object System.Collections.ArrayList
    if ($env:ProgramFiles) {
        [void]$candidates.Add((Join-Path $env:ProgramFiles 'Docker\Docker\resources\bin\docker.exe'))
    }
    if (${env:ProgramFiles(x86)}) {
        [void]$candidates.Add((Join-Path ${env:ProgramFiles(x86)} 'Docker\Docker\resources\bin\docker.exe'))
    }
    if ($env:LOCALAPPDATA) {
        [void]$candidates.Add((Join-Path $env:LOCALAPPDATA 'Docker\resources\bin\docker.exe'))
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }
    return $null
}

function Get-DockerDesktopPath {
    if (-not $HostIsWindows) { return $null }

    if ($env:XEMU_DOCKER_DESKTOP_EXE -and
        (Test-Path -LiteralPath $env:XEMU_DOCKER_DESKTOP_EXE)) {
        return $env:XEMU_DOCKER_DESKTOP_EXE
    }

    $cmd = Get-Command 'Docker Desktop.exe' -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }

    $candidates = New-Object System.Collections.ArrayList
    if ($env:ProgramFiles) {
        [void]$candidates.Add((Join-Path $env:ProgramFiles 'Docker\Docker\Docker Desktop.exe'))
    }
    if (${env:ProgramFiles(x86)}) {
        [void]$candidates.Add((Join-Path ${env:ProgramFiles(x86)} 'Docker\Docker\Docker Desktop.exe'))
    }
    if ($env:LOCALAPPDATA) {
        [void]$candidates.Add((Join-Path $env:LOCALAPPDATA 'Programs\Docker\Docker\Docker Desktop.exe'))
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return $candidate
        }
    }
    return $null
}

function Test-DockerServer([string]$DockerExe) {
    if ([string]::IsNullOrWhiteSpace($DockerExe)) { return $null }
    try {
        $serverVersion = & $DockerExe version --format '{{.Server.Version}}' 2>$null
        if ($LASTEXITCODE -eq 0 -and $serverVersion) {
            return (($serverVersion | Select-Object -First 1).ToString().Trim())
        }
    } catch {
        return $null
    }
    return $null
}

function Ensure-DockerReady {
    $dockerExe = Get-DockerCliPath
    if (-not $dockerExe) {
        Write-Warning 'Docker CLI was not found. Install Docker Desktop (or add docker.exe to PATH) before building Windows/Linux targets.'
        return $false
    }
    $script:DockerExe = $dockerExe

    $serverVersion = Test-DockerServer $script:DockerExe
    if ($serverVersion) {
        Write-Host "Docker engine ready (server $serverVersion)." -ForegroundColor Green
        return $true
    }

    if (-not $HostIsWindows) {
        Write-Warning 'Docker CLI is installed, but the Docker daemon is not responding.'
        return $false
    }

    $desktopExe = Get-DockerDesktopPath
    if (-not $desktopExe) {
        Write-Warning 'Docker Desktop is not running and Docker Desktop.exe could not be located automatically.'
        return $false
    }

    $desktopProcess = Get-Process -Name 'Docker Desktop' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $desktopProcess) {
        Write-Host "Docker engine is not ready. Starting Docker Desktop:" -ForegroundColor Yellow
        Write-Host "  $desktopExe" -ForegroundColor DarkYellow
        try {
            Start-Process -FilePath $desktopExe | Out-Null
        } catch {
            Write-Warning "Could not start Docker Desktop: $($_.Exception.Message)"
            return $false
        }
    } else {
        Write-Host 'Docker Desktop is already running; waiting for its engine to become ready.' -ForegroundColor Yellow
    }

    $timeoutSeconds = 180
    $deadline = [DateTime]::UtcNow.AddSeconds($timeoutSeconds)
    Write-Host "Waiting for Docker engine" -NoNewline
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Seconds 2
        $serverVersion = Test-DockerServer $script:DockerExe
        if ($serverVersion) {
            Write-Host ''
            Write-Host "Docker engine ready (server $serverVersion)." -ForegroundColor Green
            return $true
        }
        Write-Host '.' -NoNewline
    }
    Write-Host ''
    Write-Warning "Docker Desktop was started, but the Docker engine did not become ready within $timeoutSeconds seconds."
    return $false
}

function Get-YamlMatrixFromText([string]$text) {
    $archMatch = [regex]::Match($text, '(?m)^\s*arch:\s*\[([^\]]+)\]')
    $cfgMatch  = [regex]::Match($text, '(?m)^\s*configuration:\s*\[([^\]]+)\]')
    if (-not $archMatch.Success -or -not $cfgMatch.Success) {
        throw 'Could not find arch/configuration matrix in workflow YAML'
    }
    $arch = @($archMatch.Groups[1].Value.Split(',') | ForEach-Object { $_.Trim().Trim([char]39).Trim([char]34) })
    $cfg  = @($cfgMatch.Groups[1].Value.Split(',') | ForEach-Object { $_.Trim().Trim([char]39).Trim([char]34) })
    return [pscustomobject]@{ Arch=$arch; Configuration=$cfg; Text=$text }
}

function Get-WindowsToolchainImages([string]$YamlText) {
    $gcc = [regex]::Match($YamlText, "-gcc:sha-([0-9a-fA-F]+)")
    $llvm = [regex]::Match($YamlText, "\|\|\s*':sha-([0-9a-fA-F]+)'")
    if (-not $gcc.Success -or -not $llvm.Success) {
        throw 'Could not determine Windows toolchain image tags from build-windows.yml'
    }
    return [pscustomobject]@{
        x86_64 = "ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-$($gcc.Groups[1].Value)"
        arm64  = "ghcr.io/xemu-project/xemu-win64-toolchain:sha-$($llvm.Groups[1].Value)"
    }
}

function Get-ClangMajor([string]$YamlText) {
    $m = [regex]::Match($YamlText, 'clang-(\d+)')
    if ($m.Success) { return $m.Groups[1].Value }
    return '21'
}


function New-MatrixSubset($Matrix, [string[]]$Arch, [string[]]$Configuration) {
    return [pscustomobject]@{
        Arch = @($Arch)
        Configuration = @($Configuration)
        Text = $Matrix.Text
    }
}

function Get-AvailableBuildSelections($WindowsMatrix, $LinuxMatrix, $MacMatrix) {
    $items = New-Object System.Collections.ArrayList
    [void]$items.Add([pscustomobject]@{
        Key = 'ALL'
        Label = 'ALL - Windows + Linux + macOS matrix'
        Platform = 'All'
        Arch = ''
        Configuration = ''
        Universal = $false
    })

    foreach ($arch in $WindowsMatrix.Arch) {
        foreach ($config in $WindowsMatrix.Configuration) {
            [void]$items.Add([pscustomobject]@{
                Key = "Windows/$arch/$config"
                Label = "Windows $arch $config"
                Platform = 'Windows'
                Arch = $arch
                Configuration = $config
                Universal = $false
            })
        }
    }

    foreach ($arch in $LinuxMatrix.Arch) {
        foreach ($config in $LinuxMatrix.Configuration) {
            [void]$items.Add([pscustomobject]@{
                Key = "Linux/$arch/$config"
                Label = "Linux $arch $config"
                Platform = 'Linux'
                Arch = $arch
                Configuration = $config
                Universal = $false
            })
        }
    }

    foreach ($arch in $MacMatrix.Arch) {
        foreach ($config in $MacMatrix.Configuration) {
            [void]$items.Add([pscustomobject]@{
                Key = "MacOS/$arch/$config"
                Label = "macOS $arch $config"
                Platform = 'MacOS'
                Arch = $arch
                Configuration = $config
                Universal = $false
            })
        }
    }

    if (($MacMatrix.Arch -contains 'x86_64') -and ($MacMatrix.Arch -contains 'arm64')) {
        foreach ($config in $MacMatrix.Configuration) {
            [void]$items.Add([pscustomobject]@{
                Key = "MacOS/universal/$config"
                Label = "macOS Universal $config (builds x86_64 + arm64 prerequisites)"
                Platform = 'MacOS'
                Arch = 'universal'
                Configuration = $config
                Universal = $true
            })
        }
    }

    return @($items)
}

function Resolve-BuildSelection([string]$Requested, $Selections, [switch]$Interactive) {
    if ($Interactive) {
        Write-Host ''
        Write-Host 'Available Xemu build types' -ForegroundColor Cyan
        Write-Host '----------------------------------------'
        for ($i = 0; $i -lt $Selections.Count; $i++) {
            Write-Host ('[{0,2}] {1}' -f ($i + 1), $Selections[$i].Label)
        }
        Write-Host ''
        while ($true) {
            $answer = Read-Host 'Select build number'
            $number = 0
            if ([int]::TryParse($answer, [ref]$number) -and
                $number -ge 1 -and $number -le $Selections.Count) {
                return $Selections[$number - 1]
            }
            Write-Warning "Please enter a number from 1 to $($Selections.Count)."
        }
    }

    if ([string]::IsNullOrWhiteSpace($Requested)) {
        return $null
    }

    $match = @($Selections | Where-Object {
        $_.Key.Equals($Requested, [System.StringComparison]::OrdinalIgnoreCase) -or
        $_.Label.Equals($Requested, [System.StringComparison]::OrdinalIgnoreCase)
    }) | Select-Object -First 1
    if (-not $match) {
        $valid = ($Selections | ForEach-Object Key) -join ', '
        throw "Unknown -BuildSelection '$Requested'. Valid values: $valid"
    }
    return $match
}

function Get-WorkflowTextsFromZip([string]$ZipPath) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $buildEntry = $zip.Entries | Where-Object { $_.FullName -match '(^|/)\.github/workflows/build\.yml$' } | Select-Object -First 1
        if (-not $buildEntry) { throw 'Could not find .github/workflows/build.yml inside the ZIP.' }
        $suffix = '.github/workflows/build.yml'
        $prefix = $buildEntry.FullName.Substring(0, $buildEntry.FullName.Length - $suffix.Length)

        function Read-ZipEntryText([string]$RelativePath) {
            $entryName = $prefix + $RelativePath.Replace('\\','/')
            $entry = $zip.GetEntry($entryName)
            if (-not $entry) { throw "Missing workflow in ZIP: $entryName" }
            $reader = New-Object System.IO.StreamReader($entry.Open())
            try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
        }

        return [pscustomobject]@{
            Prefix = $prefix
            Build = Read-ZipEntryText '.github/workflows/build.yml'
            Windows = Read-ZipEntryText '.github/workflows/build-windows.yml'
            Linux = Read-ZipEntryText '.github/workflows/build-linux.yml'
            MacOS = Read-ZipEntryText '.github/workflows/build-macos.yml'
        }
    } finally {
        $zip.Dispose()
    }
}

function Get-FixOverlayManifest([string]$FixRoot) {
    if (-not (Test-Path -LiteralPath $FixRoot)) { return @() }
    $rootFull = (Resolve-Path -LiteralPath $FixRoot).Path
    $items = @(
        Get-ChildItem -LiteralPath $rootFull -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -ne '_README.txt' } |
            Sort-Object FullName |
            ForEach-Object {
                $rel = $_.FullName.Substring($rootFull.Length) -replace '^[\\/]+',''
                $rel
            }
    )
    return $items
}

function Get-FixOverlayDetails([string]$FixRoot) {
    if (-not (Test-Path -LiteralPath $FixRoot)) { return @() }
    $rootFull = (Resolve-Path -LiteralPath $FixRoot).Path
    return @(
        Get-ChildItem -LiteralPath $rootFull -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -ne '_README.txt' } |
            Sort-Object FullName |
            ForEach-Object {
                $rel = $_.FullName.Substring($rootFull.Length) -replace '^[\\/]+',''
                $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLowerInvariant()
                [pscustomobject]@{
                    file = $rel
                    sha256 = $hash
                    bytes = $_.Length
                }
            }
    )
}

function Get-DebugToolsOverlayFile([string]$FixRoot) {
    if (-not (Test-Path -LiteralPath $FixRoot)) { return $null }

    $overlayMatches = @(
        Get-ChildItem -LiteralPath $FixRoot -File -ErrorAction SilentlyContinue |
            Where-Object {
                $_.Name.Equals(
                    $script:DebugToolsOverlayName,
                    [System.StringComparison]::OrdinalIgnoreCase
                )
            }
    )
    if ($overlayMatches.Count -eq 0) { return $null }
    return $overlayMatches[0]
}

function Resolve-DebugToolsProfile(
    [string]$Requested,
    [bool]$OverlayAvailable,
    [switch]$Interactive
) {
    if (-not $OverlayAvailable) {
        if (-not [string]::IsNullOrWhiteSpace($Requested)) {
            throw "-DebugToolsProfile requires FIX\$($script:DebugToolsOverlayName)."
        }
        return ''
    }

    if ($Interactive) {
        Write-Host ''
        Write-Host 'Select Debug Tools build profile' -ForegroundColor Cyan
        Write-Host '----------------------------------------'
        Write-Host '[1] main        - Current Game + Cheat Engine only'
        Write-Host '[2] main+hdd    - Main + HDD Directory / Kernel RPC'
        Write-Host '[3] main+memory - Main + Memory Viewer / Search / x86 Debugger'
        Write-Host '[4] full        - All Debug Tools additions'
        Write-Host ''

        while ($true) {
            $answer = (Read-Host 'Select profile number [4]').Trim()
            if ([string]::IsNullOrWhiteSpace($answer) -or $answer -eq '4') {
                return 'full'
            }
            switch ($answer) {
                '1' { return 'main' }
                '2' { return 'main+hdd' }
                '3' { return 'main+memory' }
            }
            Write-Warning 'Please enter a number from 1 to 4.'
        }
    }

    if ([string]::IsNullOrWhiteSpace($Requested)) {
        return 'full'
    }

    $normalized = $Requested.Trim().ToLowerInvariant()
    if ($script:ValidDebugToolsProfiles -notcontains $normalized) {
        $valid = $script:ValidDebugToolsProfiles -join ', '
        throw "Unknown -DebugToolsProfile '$Requested'. Valid values: $valid"
    }
    return $normalized
}

function Confirm-PatchedSourceExport {
    while ($true) {
        $answer = (Read-Host 'Output the patched source ZIP too? [y/N]').Trim()
        if ([string]::IsNullOrWhiteSpace($answer) -or $answer -match '^(?i:n|no)$') { return $false }
        if ($answer -match '^(?i:y|yes)$') { return $true }
        Write-Warning 'Please enter Y or N.'
    }
}

function Get-SafeFileComponent([string]$Text) {
    $safe = $Text -replace '[^A-Za-z0-9._-]+','-'
    $safe = $safe.Trim('-')
    if ([string]::IsNullOrWhiteSpace($safe)) { return 'local' }
    return $safe
}

function Invoke-PatchedSourceExport(
    [string]$SourceZipResolved,
    [string]$BuilderRoot,
    [string]$OutRoot,
    [string]$Version,
    [string]$DebugToolsProfile
) {
    if (-not $script:DockerReady) {
        Add-BuildResult 'Source' '-' '-' 'Patched source export' 'FAILED' 'Docker is unavailable or the Docker daemon is not running.'
        return $false
    }

    $sourceOut = Join-Path $OutRoot 'source'
    New-Item -ItemType Directory -Force -Path $sourceOut | Out-Null
    $safeVersion = Get-SafeFileComponent $Version
    $sourceName = "xemu-$safeVersion-PATCHED-source.zip"

    return (Invoke-MatrixJob -Platform 'Source' -Arch '-' -Config '-' -Stage 'Patched source export' -Action {
        Write-Step 'Export patched source tree'
        $args = @(
            'run','--rm','--platform','linux/amd64',
            '-e',"XEMU_LOCAL_VERSION=$Version",
            '-e',"XEMU_PATCHED_SOURCE_NAME=$sourceName"
        )
        if (-not [string]::IsNullOrWhiteSpace($DebugToolsProfile)) {
            $args += @('-e',"XEMU_DEBUG_TOOLS_PROFILE=$DebugToolsProfile")
        }
        $args += @(
            '--mount',"type=bind,source=${SourceZipResolved},target=/input/source.zip,readonly",
            '--mount',"type=bind,source=${BuilderRoot},target=/builder,readonly",
            '--mount',"type=bind,source=${sourceOut},target=/source-out",
            'ubuntu:22.04',
            'bash','/builder/common/export-patched-source.sh'
        )
        Invoke-External $script:DockerExe $args
        $sourcePath = Join-Path $sourceOut $sourceName
        if (-not (Test-Path -LiteralPath $sourcePath)) {
            throw "Patched source ZIP was not produced: $sourcePath"
        }
        Write-Host "Patched source: $sourcePath" -ForegroundColor Green
    })
}

function New-TargetDir([string]$Root, [string]$Platform, [string]$Arch, [string]$Config) {
    $p = Join-Path $Root "$Platform/$Arch/$Config"
    New-Item -ItemType Directory -Force -Path $p | Out-Null
    return (Resolve-Path $p).Path
}

function Invoke-WindowsBuild([string]$SourceZipResolved, [string]$BuilderRoot, [string]$OutRoot, $Matrix, $Images, [string]$DebugToolsProfile) {
    foreach ($arch in $Matrix.Arch) {
        foreach ($config in $Matrix.Configuration) {
            if (-not $script:DockerReady) {
                Add-BuildResult 'Windows' $arch $config 'Build' 'FAILED' 'Docker is unavailable or the Docker daemon is not running.'
                continue
            }

            Invoke-MatrixJob -Platform 'Windows' -Arch $arch -Config $config -Stage 'Build' -Action {
                $image = if ($arch -eq 'x86_64') { $Images.x86_64 } elseif ($arch -eq 'arm64') { $Images.arm64 } else { throw "Unsupported Windows arch in YAML: $arch" }
                $out = New-TargetDir $OutRoot 'windows' $arch $config
                Write-Step "Windows $arch $config ($image)"
                $args = @(
                    'run','--rm','--platform','linux/amd64',
                    '-e',"XEMU_LOCAL_VERSION=$Version",
                    '-e',"XEMU_ARCH=$arch",
                    '-e',"XEMU_CONFIGURATION=$config"
                )
                if (-not [string]::IsNullOrWhiteSpace($DebugToolsProfile)) {
                    $args += @('-e',"XEMU_DEBUG_TOOLS_PROFILE=$DebugToolsProfile")
                }
                $args += @(
                    '--mount',"type=bind,source=${SourceZipResolved},target=/input/source.zip,readonly",
                    '--mount',"type=bind,source=${BuilderRoot},target=/builder,readonly",
                    '--mount',"type=bind,source=${out},target=/out",
                    '-v',"xemu-ccache-win-${arch}-${config}:/tmp/xemu-ccache",
                    '-v',"xemu-lto-win-${arch}-${config}:/tmp/xemu-lto-cache",
                    $image,
                    'bash','/builder/docker/build-windows-container.sh'
                )
                $buildLog = Join-Path $out 'windows-build.log'
                Invoke-ExternalLogged $script:DockerExe $args $buildLog
            } | Out-Null
        }
    }
}

function Invoke-LinuxBuild([string]$SourceZipResolved, [string]$BuilderRoot, [string]$OutRoot, $Matrix, [string]$ClangMajor, [string]$DebugToolsProfile) {
    foreach ($arch in $Matrix.Arch) {
        foreach ($config in $Matrix.Configuration) {
            if (-not $script:DockerReady) {
                Add-BuildResult 'Linux' $arch $config 'Build' 'FAILED' 'Docker is unavailable or the Docker daemon is not running.'
                continue
            }

            Invoke-MatrixJob -Platform 'Linux' -Arch $arch -Config $config -Stage 'Build' -Action {
                $platform = if ($arch -eq 'x86_64') { 'linux/amd64' } elseif ($arch -eq 'aarch64') { 'linux/arm64/v8' } else { throw "Unsupported Linux arch in YAML: $arch" }
                $out = New-TargetDir $OutRoot 'linux' $arch $config
                Write-Step "Linux $arch $config ($platform)"
                $args = @(
                    'run','--rm','--platform',$platform,
                    '-e',"XEMU_LOCAL_VERSION=$Version",
                    '-e',"XEMU_ARCH=$arch",
                    '-e',"XEMU_CONFIGURATION=$config",
                    '-e',"LLVM_MAJOR=$ClangMajor",
                    '-e','APPIMAGE_EXTRACT_AND_RUN=1'
                )
                if (-not [string]::IsNullOrWhiteSpace($DebugToolsProfile)) {
                    $args += @('-e',"XEMU_DEBUG_TOOLS_PROFILE=$DebugToolsProfile")
                }
                $args += @(
                    '--mount',"type=bind,source=${SourceZipResolved},target=/input/source.zip,readonly",
                    '--mount',"type=bind,source=${BuilderRoot},target=/builder,readonly",
                    '--mount',"type=bind,source=${out},target=/out",
                    '-v',"xemu-ccache-linux-${arch}-${config}:/tmp/xemu-ccache",
                    '-v',"xemu-lto-linux-${arch}-${config}:/tmp/xemu-lto-cache",
                    'ubuntu:22.04',
                    'bash','/builder/docker/build-linux-container.sh'
                )
                $buildLog = Join-Path $out 'linux-build.log'
                Invoke-ExternalLogged $script:DockerExe $args $buildLog
            } | Out-Null
        }
    }
}

function Invoke-PdbPackaging([string]$OutRoot, $Matrix) {
    if (-not $HostIsWindows) {
        Write-Warning 'PDB generation mirrors the GitHub windows-latest step and is only run automatically on a Windows host.'
        foreach ($arch in $Matrix.Arch) {
            foreach ($config in $Matrix.Configuration) {
                Add-BuildResult 'Windows' $arch $config 'PDB package' 'SKIPPED' 'PDB packaging requires a Windows host.'
            }
        }
        return
    }

    $toolRoot = Join-Path $OutRoot '_tools/cv2pdb-0.52'
    $cv2pdbExe = Join-Path $toolRoot 'cv2pdb64.exe'
    if (-not (Test-Path $cv2pdbExe)) {
        try {
            Write-Step 'Downloading cv2pdb 0.52 for Windows PDB generation'
            New-Item -ItemType Directory -Force -Path $toolRoot | Out-Null
            $zip = Join-Path $OutRoot '_tools/cv2pdb-0.52.zip'
            Invoke-WebRequest -Uri 'https://github.com/rainers/cv2pdb/releases/download/v0.52/cv2pdb-0.52.zip' -OutFile $zip
            Expand-Archive -LiteralPath $zip -DestinationPath $toolRoot -Force
            $found = Get-ChildItem -Path $toolRoot -Filter cv2pdb64.exe -Recurse | Select-Object -First 1
            if (-not $found) { throw 'cv2pdb64.exe not found after extraction.' }
            if ($found.FullName -ne $cv2pdbExe) { Copy-Item $found.FullName $cv2pdbExe -Force }
        } catch {
            $message = "cv2pdb setup failed: $($_.Exception.Message)"
            Write-Warning $message
            foreach ($arch in $Matrix.Arch) {
                foreach ($config in $Matrix.Configuration) {
                    Add-BuildResult 'Windows' $arch $config 'PDB package' 'FAILED' $message
                }
            }
            return
        }
    }

    foreach ($arch in $Matrix.Arch) {
        foreach ($config in $Matrix.Configuration) {
            $dist = Join-Path $OutRoot "windows/$arch/$config/dist"
            $exe = Join-Path $dist 'xemu.exe'
            if (-not (Test-Path $exe)) {
                Add-BuildResult 'Windows' $arch $config 'PDB package' 'SKIPPED' 'xemu.exe was not produced by the build job.'
                Write-Warning "Skipping PDB: missing $exe"
                continue
            }

            Invoke-MatrixJob -Platform 'Windows' -Arch $arch -Config $config -Stage 'PDB package' -Action {
                Write-Step "Generate PDB/package Windows $arch $config"
                Push-Location $dist
                try {
                    Invoke-External $cv2pdbExe @('xemu.exe')
                } finally { Pop-Location }

                $suffix = if ($config -eq 'debug') { '-dbg' } else { '' }
                $pkgDir = Join-Path $OutRoot "windows/$arch/$config/packages"
                New-Item -ItemType Directory -Force -Path $pkgDir | Out-Null
                $binZip = Join-Path $pkgDir "xemu-${Version}${suffix}-windows-${arch}.zip"
                $pdbZip = Join-Path $pkgDir "xemu-${Version}${suffix}-windows-${arch}-pdb.zip"
                if (Test-Path $binZip) { Remove-Item $binZip -Force }
                if (Test-Path $pdbZip) { Remove-Item $pdbZip -Force }
                $nonPdb = @(Get-ChildItem -LiteralPath $dist -File | Where-Object Extension -ne '.pdb' | ForEach-Object FullName)
                $pdbs = @(Get-ChildItem -LiteralPath $dist -File -Filter '*.pdb' | ForEach-Object FullName)
                if ($nonPdb.Count) { Compress-Archive -Path $nonPdb -DestinationPath $binZip -CompressionLevel Optimal }
                if ($pdbs.Count) { Compress-Archive -Path $pdbs -DestinationPath $pdbZip -CompressionLevel Optimal }
            } | Out-Null
        }
    }
}

function Invoke-MacBuild([string]$SourceZipResolved, [string]$BuilderRoot, [string]$OutRoot, $Matrix, [string]$DebugToolsProfile) {
    $archCsv = ($Matrix.Arch -join ',')
    $cfgCsv = ($Matrix.Configuration -join ',')
    $fixRoot = Join-Path $BuilderRoot 'FIX'

    if ($HostIsMacOS) {
        Invoke-MatrixJob -Platform 'macOS' -Arch 'matrix' -Config 'all' -Stage 'Build' -Action {
            Write-Step 'Running macOS matrix natively (Docker cannot provide a macOS/Xcode runner)'
            $args = @(
                (Join-Path $BuilderRoot 'macos/build-macos-native.sh'),
                $SourceZipResolved,
                (Join-Path $OutRoot 'macos'),
                $Version,
                $archCsv,
                $cfgCsv,
                $fixRoot,
                $DebugToolsProfile
            )
            Invoke-External 'bash' $args
        } | Out-Null
        return
    }

    if ([string]::IsNullOrWhiteSpace($MacHost)) {
        Write-Warning 'macOS jobs require a real Mac (GitHub uses macos-15). Supply -MacHost user@mac to run them over SSH, or run this builder on a Mac.'
        Add-BuildResult 'macOS' 'matrix' 'all' 'Build' 'SKIPPED' 'No real Mac host configured.'
        return
    }

    Invoke-MatrixJob -Platform 'macOS' -Arch 'matrix' -Config 'all' -Stage 'Build' -Action {
        Require-Command 'ssh'
        Require-Command 'scp'
        $remoteBase = "/tmp/xemu-local-matrix-$([guid]::NewGuid().ToString('N'))"
        try {
            Write-Step "Copying source ZIP, FIX overlay, and macOS builder to $MacHost"
            Invoke-External 'ssh' @($MacHost, "mkdir -p '$remoteBase/macos' '$remoteBase/common'")
            Invoke-External 'scp' @($SourceZipResolved, "${MacHost}:${remoteBase}/source.zip")
            Invoke-External 'scp' @((Join-Path $BuilderRoot 'macos/build-macos-native.sh'), "${MacHost}:${remoteBase}/macos/build-macos-native.sh")
            Invoke-External 'scp' @((Join-Path $BuilderRoot 'common/apply-fix-overlay.sh'), "${MacHost}:${remoteBase}/common/apply-fix-overlay.sh")
            Invoke-External 'scp' @((Join-Path $BuilderRoot 'common/set-debug-tools-profile.sh'), "${MacHost}:${remoteBase}/common/set-debug-tools-profile.sh")
            if (Test-Path -LiteralPath $fixRoot) {
                Invoke-External 'scp' @('-r', $fixRoot, "${MacHost}:${remoteBase}/")
            } else {
                Invoke-External 'ssh' @($MacHost, "mkdir -p '$remoteBase/FIX'")
            }
            Write-Step 'Running macOS matrix on remote Mac'
            Invoke-External 'ssh' @($MacHost, "chmod +x '$remoteBase/macos/build-macos-native.sh' '$remoteBase/common/apply-fix-overlay.sh' '$remoteBase/common/set-debug-tools-profile.sh' && '$remoteBase/macos/build-macos-native.sh' '$remoteBase/source.zip' '$remoteBase/out' '$Version' '$archCsv' '$cfgCsv' '$remoteBase/FIX' '$DebugToolsProfile'")
            $localMacOut = Join-Path $OutRoot 'macos'
            New-Item -ItemType Directory -Force -Path $localMacOut | Out-Null
            Invoke-External 'scp' @('-r', "${MacHost}:${remoteBase}/out/.", $localMacOut)
        } finally {
            try { Invoke-External 'ssh' @($MacHost, "rm -rf '$remoteBase'") } catch { Write-Warning "Could not clean remote workspace $remoteBase" }
        }
    } | Out-Null
}

$sourceZipResolved = (Resolve-Path -LiteralPath $SourceZip).Path
$builderRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$fixRoot = Join-Path $builderRoot 'FIX'
if (-not (Test-Path -LiteralPath $fixRoot)) {
    New-Item -ItemType Directory -Force -Path $fixRoot | Out-Null
}
$fixManifest = @(Get-FixOverlayManifest $fixRoot)
$fixDetails = @(Get-FixOverlayDetails $fixRoot)
$debugToolsOverlay = Get-DebugToolsOverlayFile $fixRoot
$debugToolsAvailable = ($null -ne $debugToolsOverlay)
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path (Split-Path -Parent $sourceZipResolved) 'xemu-local-build-artifacts'
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$outRoot = (Resolve-Path $OutputDir).Path

Write-Step 'Reading GitHub build matrices directly from the source ZIP'
$workflows = Get-WorkflowTextsFromZip $sourceZipResolved
Write-Host "ZIP source root: $($workflows.Prefix)"

$winMatrix = Get-YamlMatrixFromText $workflows.Windows
$linuxMatrix = Get-YamlMatrixFromText $workflows.Linux
$macMatrix = Get-YamlMatrixFromText $workflows.MacOS
$images = Get-WindowsToolchainImages $winMatrix.Text
$clangMajor = Get-ClangMajor $linuxMatrix.Text


$availableSelections = @(Get-AvailableBuildSelections $winMatrix $linuxMatrix $macMatrix)
$selectedBuild = Resolve-BuildSelection -Requested $BuildSelection -Selections $availableSelections -Interactive:$InteractiveSelect
$exportPatchedSource = [bool]$OutputPatchedSource
if ($InteractiveSelect) {
    $exportPatchedSource = Confirm-PatchedSourceExport
}
$effectiveDebugToolsProfile = Resolve-DebugToolsProfile `
    -Requested $DebugToolsProfile `
    -OverlayAvailable $debugToolsAvailable `
    -Interactive:$InteractiveSelect

$runWindows = $false
$runLinux = $false
$runMacOS = $false
$selectedWinMatrix = $winMatrix
$selectedLinuxMatrix = $linuxMatrix
$selectedMacMatrix = $macMatrix
$effectiveSelection = ''

if ($selectedBuild) {
    $effectiveSelection = $selectedBuild.Key
    if ($selectedBuild.Platform -eq 'All') {
        $runWindows = $true
        $runLinux = $true
        $runMacOS = $true
    } elseif ($selectedBuild.Platform -eq 'Windows') {
        $runWindows = $true
        $selectedWinMatrix = New-MatrixSubset -Matrix $winMatrix -Arch @($selectedBuild.Arch) -Configuration @($selectedBuild.Configuration)
    } elseif ($selectedBuild.Platform -eq 'Linux') {
        $runLinux = $true
        $selectedLinuxMatrix = New-MatrixSubset -Matrix $linuxMatrix -Arch @($selectedBuild.Arch) -Configuration @($selectedBuild.Configuration)
    } elseif ($selectedBuild.Platform -eq 'MacOS') {
        $runMacOS = $true
        if ($selectedBuild.Universal) {
            # A universal bundle requires both architecture builds first.
            $selectedMacMatrix = New-MatrixSubset -Matrix $macMatrix -Arch @('x86_64','arm64') -Configuration @($selectedBuild.Configuration)
        } else {
            $selectedMacMatrix = New-MatrixSubset -Matrix $macMatrix -Arch @($selectedBuild.Arch) -Configuration @($selectedBuild.Configuration)
        }
    }
} else {
    $effectiveSelection = $Target
    $runWindows = ($Target -in @('All','Windows'))
    $runLinux = ($Target -in @('All','Linux'))
    $runMacOS = ($Target -in @('All','MacOS'))
}

Write-Host "Selected build: $effectiveSelection" -ForegroundColor Green

$debugToolsOverlayPath = $null
if ($debugToolsAvailable) {
    $debugToolsOverlayPath = $debugToolsOverlay.FullName
}

$plan = [ordered]@{
    source_zip = $sourceZipResolved
    source_zip_root = $workflows.Prefix
    version = $Version
    build_selection = $effectiveSelection
    output_patched_source = $exportPatchedSource
    debug_tools = [ordered]@{
        selector_available = $debugToolsAvailable
        overlay = $debugToolsOverlayPath
        profile = $effectiveDebugToolsProfile
    }
    fix_overlay = [ordered]@{ path=$fixRoot; enabled=($fixManifest.Count -gt 0); files=$fixManifest; details=$fixDetails }
    windows = [ordered]@{ enabled=$runWindows; arch=$selectedWinMatrix.Arch; configuration=$selectedWinMatrix.Configuration; images=$images }
    linux = [ordered]@{ enabled=$runLinux; arch=$selectedLinuxMatrix.Arch; configuration=$selectedLinuxMatrix.Configuration; llvm_major=$clangMajor }
    macos = [ordered]@{ enabled=$runMacOS; arch=$selectedMacMatrix.Arch; configuration=$selectedMacMatrix.Configuration; runner='real macOS host required' }
}
$planPath = Join-Path $outRoot 'build-plan.json'
$plan | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $planPath -Encoding UTF8
Write-Host "Build plan: $planPath"
if ($runWindows) { Write-Host "Windows: $($selectedWinMatrix.Arch -join ', ') x $($selectedWinMatrix.Configuration -join ', ')" }
if ($runLinux) { Write-Host "Linux:   $($selectedLinuxMatrix.Arch -join ', ') x $($selectedLinuxMatrix.Configuration -join ', ')" }
if ($runMacOS) { Write-Host "macOS:   $($selectedMacMatrix.Arch -join ', ') x $($selectedMacMatrix.Configuration -join ', ')" }
if ($fixManifest.Count -gt 0) {
    Write-Host "FIX:     enabled ($($fixManifest.Count) item(s))" -ForegroundColor Yellow
    foreach ($item in $fixDetails) {
        Write-Host ("         {0}  SHA-256 {1}" -f $item.file, $item.sha256) -ForegroundColor DarkYellow
    }
} else {
    Write-Host 'FIX:     empty (base source ZIP will be built unchanged)'
}
Write-Host "Patched source output: $exportPatchedSource"
if ($debugToolsAvailable) {
    Write-Host "Debug Tools overlay: $($debugToolsOverlay.Name)" -ForegroundColor Yellow
    Write-Host "Debug Tools profile: $effectiveDebugToolsProfile" -ForegroundColor Green
} else {
    Write-Host "Debug Tools profile selector: unavailable ($($script:DebugToolsOverlayName) not found)"
}
$fixManifestPath = Join-Path $outRoot 'fix-overlay-manifest.txt'
$fixManifestLines = @(
    "FIX folder: $fixRoot",
    "Enabled: $($fixManifest.Count -gt 0)",
    "Debug Tools overlay detected: $debugToolsAvailable",
    "Debug Tools profile: $effectiveDebugToolsProfile",
    ''
)
if ($fixDetails.Count -gt 0) {
    $fixManifestLines += @($fixDetails | ForEach-Object { "{0} | SHA-256 {1} | {2} bytes" -f $_.file, $_.sha256, $_.bytes })
}
$fixManifestLines | Set-Content -LiteralPath $fixManifestPath -Encoding UTF8

if ($PlanOnly) { return }

if ($runWindows -or $runLinux -or $exportPatchedSource) {
    Write-Step 'Checking Docker / Docker Desktop'
    $script:DockerReady = Ensure-DockerReady
    if (-not $script:DockerReady) {
        Write-Warning 'Docker is unavailable. Docker-backed jobs will be recorded as FAILED, but the requested matrix will continue.'
    }
}

if ($exportPatchedSource) {
    [void](Invoke-PatchedSourceExport $sourceZipResolved $builderRoot $outRoot $Version $effectiveDebugToolsProfile)
}

if ($runWindows) {
    Invoke-WindowsBuild $sourceZipResolved $builderRoot $outRoot $selectedWinMatrix $images $effectiveDebugToolsProfile
    if (-not $SkipPdb) { Invoke-PdbPackaging $outRoot $selectedWinMatrix }
}
if ($runLinux) {
    Invoke-LinuxBuild $sourceZipResolved $builderRoot $outRoot $selectedLinuxMatrix $clangMajor $effectiveDebugToolsProfile
}
if ($runMacOS) {
    Invoke-MacBuild $sourceZipResolved $builderRoot $outRoot $selectedMacMatrix $effectiveDebugToolsProfile
}

Write-Step 'Build matrix complete'
Write-Host "Artifacts: $outRoot"
$finalExitCode = Write-BuildSummary $outRoot
exit $finalExitCode
