Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
  $PSNativeCommandUseErrorActionPreference = $true
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

function Invoke-NativeCommand {
  param(
    [Parameter(Mandatory = $true)][string]$FilePath,
    [string[]]$ArgumentList = @()
  )

  & $FilePath @ArgumentList
  $exitCode = $LASTEXITCODE
  if ($exitCode -ne 0) {
    throw "Native command failed with exit code ${exitCode}: $FilePath $($ArgumentList -join ' ')"
  }
}

function Assert-FileSha256 {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$ExpectedSha256
  )

  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "Required download is missing: $Path"
  }
  $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -cne $ExpectedSha256.ToLowerInvariant()) {
    throw "SHA-256 mismatch for ${Path}: expected $ExpectedSha256, got $actual"
  }
}

function Get-VerifiedDownload {
  param(
    [Parameter(Mandatory = $true)][string]$Uri,
    [Parameter(Mandatory = $true)][string]$Destination,
    [Parameter(Mandatory = $true)][string]$ExpectedSha256
  )

  if (Test-Path -LiteralPath $Destination -PathType Leaf) {
    try {
      Assert-FileSha256 -Path $Destination -ExpectedSha256 $ExpectedSha256
      return
    } catch {
      Remove-Item -LiteralPath $Destination -Force
    }
  }

  $temporary = "$Destination.download-$PID"
  Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
  $client = New-Object System.Net.WebClient
  try {
    $client.DownloadFile($Uri, $temporary)
    Assert-FileSha256 -Path $temporary -ExpectedSha256 $ExpectedSha256
    Move-Item -LiteralPath $temporary -Destination $Destination -Force
  } finally {
    $client.Dispose()
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
  }
}

function Assert-RequiredFiles {
  param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string[]]$RequiredFiles
  )

  foreach ($relativePath in $RequiredFiles) {
    $requiredPath = Join-Path $Root $relativePath
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
      throw "Staged dependency payload is missing required file: $requiredPath"
    }
  }
}

function Resolve-GitHubRunnerPythonBuildRoot {
  param(
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string[]]$RequiredFiles
  )

  if ($env:GITHUB_ACTIONS -cne 'true' -or
      [string]::IsNullOrWhiteSpace($env:RUNNER_TOOL_CACHE)) {
    return $null
  }
  $pythonCache = Join-Path $env:RUNNER_TOOL_CACHE 'Python'
  $versionCache = Join-Path $pythonCache $Version
  $candidate = Join-Path $versionCache 'x64'
  foreach ($relativePath in $RequiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $candidate $relativePath) -PathType Leaf)) {
      return $null
    }
  }
  return $candidate
}

function Install-ZipDependency {
  param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string[]]$RequiredFiles,
    [Parameter(Mandatory = $true)][string]$Uri,
    [Parameter(Mandatory = $true)][string]$ArchiveName,
    [Parameter(Mandatory = $true)][string]$ArchiveSha256
  )

  $archivePath = Join-Path ([IO.Path]::GetTempPath()) $ArchiveName
  Get-VerifiedDownload -Uri $Uri -Destination $archivePath -ExpectedSha256 $ArchiveSha256

  $stage = "$Root.gc-stage-$PID"
  Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
  try {
    $safeExtractor = Join-Path $PSScriptRoot '..\safe-extract.py'
    Invoke-NativeCommand -FilePath 'python.exe' -ArgumentList @(
      $safeExtractor, '--format', 'zip', '--archive', $archivePath,
      '--destination', $stage
    )
    Assert-RequiredFiles -Root $stage -RequiredFiles $RequiredFiles
    Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
    Move-Item -LiteralPath $stage -Destination $Root
  } finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
  }
}

function Invoke-InstallerWithRetry {
  param(
    [Parameter(Mandatory = $true)][string]$FilePath,
    [Parameter(Mandatory = $true)][string[]]$ArgumentList,
    [Parameter(Mandatory = $true)][string]$LogPath,
    [Parameter(Mandatory = $true)][int[]]$RetryExitCodes,
    [ValidateRange(1, 5)][int]$MaximumAttempts = 2,
    [ValidateRange(0, 300)][int]$RetryDelaySeconds = 15
  )

  $installerArguments = @($ArgumentList) + @('/log', $LogPath)
  for ($attempt = 1; $attempt -le $MaximumAttempts; $attempt++) {
    $process = Start-Process `
      -FilePath $FilePath `
      -ArgumentList $installerArguments `
      -NoNewWindow `
      -Wait `
      -PassThru
    if ($process.ExitCode -eq 0) {
      return
    }
    $retryable = $RetryExitCodes -contains $process.ExitCode
    if (-not $retryable -or $attempt -eq $MaximumAttempts) {
      if (Test-Path -LiteralPath $LogPath -PathType Leaf) {
        Write-Warning "Installer log tail from ${LogPath}:"
        Get-Content -LiteralPath $LogPath -Tail 120 | Write-Warning
      }
      throw "Installer failed with exit code $($process.ExitCode) after $attempt attempt(s)"
    }
    Write-Warning (
      "Installer failed with retryable exit code $($process.ExitCode); " +
      "retrying after $RetryDelaySeconds seconds"
    )
    Start-Sleep -Seconds $RetryDelaySeconds
  }
}

function Install-VerifiedPythonBuild {
  param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$OpenSslVersion,
    [Parameter(Mandatory = $true)][string]$InstallerUri,
    [Parameter(Mandatory = $true)][string]$InstallerSha256
  )

  if ($Version -cne '3.13.14' -or $OpenSslVersion -cne '3.0.21') {
    throw 'Windows build Python identity is not the reviewed release'
  }
  $installer = Join-Path (
    [IO.Path]::GetTempPath()
  ) "python-$Version-amd64.exe"
  Get-VerifiedDownload `
    -Uri $InstallerUri `
    -Destination $installer `
    -ExpectedSha256 $InstallerSha256

  Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
  $installerArguments = @(
    '/quiet',
    'InstallAllUsers=1',
    "TargetDir=$Root",
    'PrependPath=0',
    'Include_launcher=0',
    'InstallLauncherAllUsers=0',
    'Include_doc=0',
    'Include_test=0',
    'Include_tcltk=0',
    'Include_pip=1',
    'Include_dev=1',
    'Include_exe=1',
    'Include_lib=1',
    'Include_symbols=0',
    'Include_debug=0',
    'Shortcuts=0'
  )
  $installerLog = Join-Path ([IO.Path]::GetTempPath()) "python-$Version-install.log"
  try {
    Invoke-InstallerWithRetry `
      -FilePath $installer `
      -ArgumentList $installerArguments `
      -LogPath $installerLog `
      -RetryExitCodes @(1603, 1618) `
      -MaximumAttempts 2 `
      -RetryDelaySeconds 15
  } catch {
    Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
    throw
  }

  $required = @(
    'python.exe',
    'python313.dll',
    'include\Python.h',
    'libs\python313.lib',
    'DLLs\_ssl.pyd'
  )
  $selectedRoot = $Root
  try {
    Assert-RequiredFiles -Root $selectedRoot -RequiredFiles $required
  } catch {
    $selectedRoot = Resolve-GitHubRunnerPythonBuildRoot `
      -Version $Version `
      -RequiredFiles $required
    if ([string]::IsNullOrWhiteSpace($selectedRoot)) {
      throw
    }
  }
  Assert-RequiredFiles -Root $selectedRoot -RequiredFiles $required
  Invoke-NativeCommand -FilePath (Join-Path $selectedRoot 'python.exe') -ArgumentList @(
    '-B', '-I', '-c',
    "import ssl, sys; assert sys.version.split()[0] == '$Version'; assert ssl.OPENSSL_VERSION.split()[1] == '$OpenSslVersion'"
  ) | Out-Null

  $env:PATH = "$selectedRoot\Scripts;$selectedRoot;$env:PATH"
  if ($null -ne (Get-Command Set-AppveyorBuildVariable -ErrorAction SilentlyContinue)) {
    Set-AppveyorBuildVariable -Name 'PATH' -Value $env:PATH
  }
}

function Install-RDependency {
  param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Uri,
    [Parameter(Mandatory = $true)][string]$InstallerSha256
  )

  $required = @('bin\R.exe', 'bin\Rscript.exe', 'library\base\DESCRIPTION')
  $installer = Join-Path ([IO.Path]::GetTempPath()) 'R-4.1.3-win.exe'
  Get-VerifiedDownload -Uri $Uri -Destination $installer -ExpectedSha256 $InstallerSha256
  Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
  $process = Start-Process -FilePath $installer -ArgumentList @(
    '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/DIR=$Root"
  ) -NoNewWindow -Wait -PassThru
  if ($process.ExitCode -ne 0) {
    Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
    throw "R installer failed with exit code $($process.ExitCode)"
  }
  Assert-RequiredFiles -Root $Root -RequiredFiles $required
  Invoke-NativeCommand -FilePath (Join-Path $Root 'bin\Rscript.exe') -ArgumentList @(
    '--vanilla', '-e', "stopifnot(as.character(getRversion()) == '4.1.3')"
  )
  Invoke-NativeCommand -FilePath (Join-Path $Root 'bin\R.exe') -ArgumentList @('--version')
}

function Initialize-VcpkgDependency {
  param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Commit,
    [Parameter(Mandatory = $true)][string]$ManifestRoot
  )

  # Never execute hooks or binaries restored from an AppVeyor cache.
  Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Path $Root | Out-Null
  Invoke-NativeCommand -FilePath 'git.exe' -ArgumentList @('-C', $Root, 'init')
  Invoke-NativeCommand -FilePath 'git.exe' -ArgumentList @(
    '-C', $Root, 'remote', 'add', 'origin', 'https://github.com/microsoft/vcpkg.git'
  )

  Invoke-NativeCommand -FilePath 'git.exe' -ArgumentList @(
    '-C', $Root, 'fetch', '--force', '--depth=1', 'origin', $Commit
  )
  Invoke-NativeCommand -FilePath 'git.exe' -ArgumentList @(
    '-C', $Root, 'checkout', '--detach', '--force', 'FETCH_HEAD'
  )
  $head = (Invoke-NativeCommand -FilePath 'git.exe' -ArgumentList @(
    '-C', $Root, 'rev-parse', 'HEAD'
  ) | Select-Object -Last 1).Trim()
  if ($head -cne $Commit) {
    throw "vcpkg checkout mismatch: expected $Commit, got $head"
  }

  Invoke-NativeCommand -FilePath (Join-Path $Root 'bootstrap-vcpkg.bat') -ArgumentList @('-disableMetrics')

  $installRoot = Join-Path $Root 'installed'
  $required = @(
    'x64-windows\include\gsl\gsl_math.h',
    'x64-windows\lib\gsl.lib',
    'x64-windows\lib\gslcblas.lib',
    'x64-windows\bin\gsl.dll',
    'x64-windows\bin\gslcblas.dll'
  )

  Invoke-NativeCommand -FilePath (Join-Path $Root 'vcpkg.exe') -ArgumentList @(
    'install',
    "--x-manifest-root=$ManifestRoot",
    "--x-install-root=$installRoot",
    '--triplet=x64-windows',
    '--disable-metrics',
    '--clean-after-build'
  )
  Assert-RequiredFiles -Root $installRoot -RequiredFiles $required
}

if ($env:GC_BUILD_ACQUISITION_HELPERS_ONLY -eq '1') {
  return
}

$pythonBuildVersion = '3.13.14'
$pythonBuildOpenSslVersion = '3.0.21'
$pythonBuildInstallerSha256 = 'c54d9b9bbb8a36e6489363ddd01139707fd781d72f1f9e90c7ec65d0061368e0'
Install-VerifiedPythonBuild `
  -Root 'C:\Python313-x64' `
  -Version $pythonBuildVersion `
  -OpenSslVersion $pythonBuildOpenSslVersion `
  -InstallerUri "https://www.python.org/ftp/python/$pythonBuildVersion/python-$pythonBuildVersion-amd64.exe" `
  -InstallerSha256 $pythonBuildInstallerSha256

$libsSha256 = '4d80f4166dee19a7e4ad0a17278b0dd5c9338c9bdcb38fb5856f091dce179a49'
$libsRequired = @(
  'win_flex.exe',
  'win_bison.exe',
  '10_Precompiled_DLL\D2XX\CDM\ftd2xx.h',
  '10_Precompiled_DLL\D2XX\CDM\Static\amd64\ftd2xx.lib',
  '10_Precompiled_DLL\libusb-win32-bin-1.2.6.0\include\lusb0_usb.h',
  '10_Precompiled_DLL\libusb-win32-bin-1.2.6.0\lib\msvc_x64\libusb.lib',
  '10_Precompiled_DLL\libsamplerate64\include\samplerate.h',
  '10_Precompiled_DLL\libsamplerate64\lib\libsamplerate-0.lib',
  '10_Precompiled_DLL\libsamplerate64\lib\libsamplerate-0.dll',
  '10_Precompiled_DLL\usbexpress_3.5.1\USBXpress\USBXpress_API\Host\SiUSBXp.h',
  '10_Precompiled_DLL\usbexpress_3.5.1\USBXpress\USBXpress_API\Host\x64\SiUSBXp.lib',
  '10_Precompiled_DLL\usbexpress_3.5.1\USBXpress\USBXpress_API\Host\x64\SiUSBXp.dll'
)
Install-ZipDependency `
  -Root 'C:\LIBS' `
  -RequiredFiles $libsRequired `
  -Uri 'https://github.com/GoldenCheetah/WindowsSDK/releases/download/v0.1.1/gc-ci-libs.zip' `
  -ArchiveName 'gc-ci-libs-0.1.1.zip' `
  -ArchiveSha256 $libsSha256

$jomSha256 = '128fdd846fe24f8594eed37d1d8929a0ea78df563537c0c1b1861a635013fff8'
Install-ZipDependency `
  -Root 'C:\JOM' `
  -RequiredFiles @('jom.exe', 'ibjom.bat') `
  -Uri 'https://download.qt.io/official_releases/jom/jom_1_1_3.zip' `
  -ArchiveName 'jom-1.1.3.zip' `
  -ArchiveSha256 $jomSha256

$rSha256 = '79d1afdc3ca50fe3ca8510939b691aeb231e0cfebfbb8c26b1f0e1f09a7ab87c'
Install-RDependency `
  -Root 'C:\R' `
  -Uri 'https://cran.r-project.org/bin/windows/base/old/4.1.3/R-4.1.3-win.exe' `
  -InstallerSha256 $rSha256

Initialize-VcpkgDependency `
  -Root 'C:\tools\vcpkg' `
  -Commit 'ef7dbf94b9198bc58f45951adcf1f041fcbc5ea0' `
  -ManifestRoot $PSScriptRoot
