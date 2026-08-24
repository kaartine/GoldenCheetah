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
      throw "Staged Python payload is missing required file: $requiredPath"
    }
  }
}

function Assert-NoFileContainsText {
  param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$Text
  )

  $bytePreservingEncoding = [Text.Encoding]::GetEncoding(28591)
  $needle = $bytePreservingEncoding.GetString(
    [Text.Encoding]::UTF8.GetBytes($Text)
  )
  foreach ($file in Get-ChildItem -LiteralPath $Root -File -Recurse -Force) {
    $contents = $bytePreservingEncoding.GetString(
      [IO.File]::ReadAllBytes($file.FullName)
    )
    if ($contents.Contains($needle)) {
      throw "Python payload retains its staging path in $($file.FullName)"
    }
  }
}

function Get-StagedFileRecord {
  param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$RelativePath
  )

  if ($RelativePath -notmatch '^[A-Za-z0-9_. -]+(?:[\\/][A-Za-z0-9_. -]+)*$' -or
      $RelativePath -match '(^|[\\/])\.\.([\\/]|$)') {
    throw "Invalid staged runtime path: $RelativePath"
  }
  $separators = [char[]]@(
    [IO.Path]::DirectorySeparatorChar,
    [IO.Path]::AltDirectorySeparatorChar
  )
  $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd($separators) +
    [IO.Path]::DirectorySeparatorChar
  $path = [IO.Path]::GetFullPath((Join-Path $Root $RelativePath))
  if (-not $path.StartsWith($rootPath, [StringComparison]::OrdinalIgnoreCase) -or
      -not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw "Required staged runtime file is missing: $RelativePath"
  }
  $item = Get-Item -LiteralPath $path -Force
  if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Staged runtime file is a reparse point: $RelativePath"
  }
  return [ordered]@{
    path = $RelativePath.Replace('\', '/')
    sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
  }
}

function Get-PythonAbiTag {
  param([Parameter(Mandatory = $true)][string]$Version)

  if ($Version -notmatch '^([0-9]+)\.([0-9]+)\.[0-9]+$') {
    throw "Invalid Python version: $Version"
  }
  return "$($Matches[1])$($Matches[2])"
}

function Get-PythonRuntimeFiles {
  param([Parameter(Mandatory = $true)][string]$Version)

  $abi = Get-PythonAbiTag -Version $Version
  return @(
    '_ssl.pyd',
    'libcrypto-3.dll',
    'libssl-3.dll',
    'PYTHON LICENSE.txt',
    'python.exe',
    "python$abi._pth",
    "python$abi.dll",
    "python$abi.zip"
  )
}

function Assert-PythonOpenSslVersion {
  param(
    [Parameter(Mandatory = $true)][string]$ReportedVersion,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion
  )

  if ($ExpectedVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$' -or
      $ReportedVersion -notmatch '^OpenSSL ([0-9]+\.[0-9]+\.[0-9]+)(?:\s|$)') {
    throw "Embedded Python loaded an unexpected OpenSSL runtime: $ReportedVersion"
  }
  $semanticVersion = $Matches[1]
  if ($semanticVersion -cne $ExpectedVersion) {
    throw "Embedded Python OpenSSL mismatch: expected $ExpectedVersion, got $semanticVersion"
  }
  return $ReportedVersion
}

function Get-PythonOpenSslVersion {
  param(
    [Parameter(Mandatory = $true)][string]$Root,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion
  )

  $output = @(Invoke-NativeCommand `
    -FilePath (Join-Path $Root 'python.exe') `
    -ArgumentList @(
      '-B', '-I', '-c',
      'import ssl; print(ssl.OPENSSL_VERSION)'
    ))
  if ($output.Count -eq 0) {
    throw 'Embedded Python did not report its OpenSSL runtime'
  }
  $version = ([string]$output[-1]).Trim()
  return (Assert-PythonOpenSslVersion `
      -ReportedVersion $version `
      -ExpectedVersion $ExpectedVersion)
}

function Use-WindowsSchannelTlsBackend {
  param([Parameter(Mandatory = $true)][string]$Destination)

  if (-not (Test-Path -LiteralPath $Destination -PathType Container)) {
    throw "Windows staging directory is missing: $Destination"
  }
  $destinationItem = Get-Item -LiteralPath $Destination -Force
  if (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'Windows staging directory must not be a reparse point'
  }

  $schannel = Join-Path $Destination 'tls\qschannelbackend.dll'
  if (-not (Test-Path -LiteralPath $schannel -PathType Leaf)) {
    throw 'Qt Schannel TLS backend is missing from the staged payload'
  }
  $schannelItem = Get-Item -LiteralPath $schannel -Force
  if (($schannelItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'Qt Schannel TLS backend must not be a reparse point'
  }

  $forbidden = @(
    'tls\qopensslbackend.dll',
    'tls\qopensslbackendd.dll',
    'libcrypto-1_1-x64.dll',
    'libssl-1_1-x64.dll',
    'libcrypto-3-x64.dll',
    'libssl-3-x64.dll'
  )
  foreach ($relativePath in $forbidden) {
    Remove-Item -LiteralPath (Join-Path $Destination $relativePath) `
      -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath (Join-Path $Destination $relativePath)) {
      throw "OpenSSL Qt backend file remains in Schannel staging: $relativePath"
    }
  }
  Assert-WindowsSchannelTlsBackend -Destination $Destination
}

function Assert-WindowsSchannelTlsBackend {
  param([Parameter(Mandatory = $true)][string]$Destination)

  if (-not (Test-Path -LiteralPath $Destination -PathType Container)) {
    throw "Windows staging directory is missing: $Destination"
  }
  $destinationItem = Get-Item -LiteralPath $Destination -Force
  if (($destinationItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'Windows staging directory must not be a reparse point'
  }

  $schannel = Join-Path $Destination 'tls\qschannelbackend.dll'
  if (-not (Test-Path -LiteralPath $schannel -PathType Leaf)) {
    throw 'Qt Schannel TLS backend is missing from the staged payload'
  }
  $schannelItem = Get-Item -LiteralPath $schannel -Force
  if (($schannelItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'Qt Schannel TLS backend must not be a reparse point'
  }

  foreach ($relativePath in @(
    'tls\qopensslbackend.dll',
    'tls\qopensslbackendd.dll',
    'libcrypto-1_1-x64.dll',
    'libssl-1_1-x64.dll',
    'libcrypto-3-x64.dll',
    'libssl-3-x64.dll'
  )) {
    if (Test-Path -LiteralPath (Join-Path $Destination $relativePath)) {
      throw "OpenSSL Qt backend file is present in final staging: $relativePath"
    }
  }
}

function Write-WindowsRuntimeProvenance {
  param(
    [Parameter(Mandatory = $true)][string]$Destination,
    [Parameter(Mandatory = $true)][string]$PythonVersion,
    [Parameter(Mandatory = $true)][string]$PythonArchiveUri,
    [Parameter(Mandatory = $true)][string]$PythonArchiveSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedOpenSslVersion
  )

  Assert-WindowsSchannelTlsBackend -Destination $Destination
  if ($PythonVersion -notmatch '^3\.[0-9]+\.[0-9]+$' -or
      $PythonArchiveUri -notmatch '^https://[^\s]+\.zip$' -or
      $PythonArchiveSha256 -notmatch '^[0-9a-f]{64}$' -or
      $ExpectedOpenSslVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw 'Pinned Python runtime identity is invalid'
  }
  $pythonFiles = @(Get-PythonRuntimeFiles -Version $PythonVersion)
  $records = @(
    $pythonFiles | ForEach-Object {
      Get-StagedFileRecord -Root $Destination -RelativePath $_
    }
  )
  $opensslVersion = Get-PythonOpenSslVersion `
    -Root $Destination `
    -ExpectedVersion $ExpectedOpenSslVersion
  $schannelRecord = Get-StagedFileRecord `
    -Root $Destination -RelativePath 'tls\qschannelbackend.dll'
  $provenance = [ordered]@{
    format = 'goldencheetah-windows-runtime-provenance-1'
    qt_tls = [ordered]@{
      backend = 'schannel'
      plugin = $schannelRecord
    }
    python = [ordered]@{
      version = $PythonVersion
      archive_url = $PythonArchiveUri
      archive_sha256 = $PythonArchiveSha256
      openssl_version = $opensslVersion
      files = $records
    }
  }
  $path = Join-Path $Destination 'GoldenCheetah.windows-provenance.json'
  if (Test-Path -LiteralPath $path) {
    $item = Get-Item -LiteralPath $path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
      throw 'Windows runtime provenance must not be a reparse point'
    }
  }
  $temporary = "$path.tmp-$PID"
  try {
    [IO.File]::WriteAllText(
      $temporary,
      (($provenance | ConvertTo-Json -Depth 6) + "`n"),
      (New-Object Text.UTF8Encoding($false))
    )
    Move-Item -LiteralPath $temporary -Destination $path -Force
  } finally {
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
  }
}

function Assert-WindowsRuntimeProvenance {
  param(
    [Parameter(Mandatory = $true)][string]$Destination,
    [Parameter(Mandatory = $true)][string]$PythonVersion,
    [Parameter(Mandatory = $true)][string]$PythonArchiveUri,
    [Parameter(Mandatory = $true)][string]$PythonArchiveSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedOpenSslVersion
  )

  Assert-WindowsSchannelTlsBackend -Destination $Destination
  $path = Join-Path $Destination 'GoldenCheetah.windows-provenance.json'
  if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
    throw 'Windows runtime provenance is missing'
  }
  $item = Get-Item -LiteralPath $path -Force
  if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'Windows runtime provenance must not be a reparse point'
  }
  $document = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
  $topLevel = @($document.PSObject.Properties.Name | Sort-Object)
  if (($topLevel -join "`n") -cne "format`npython`nqt_tls" -or
      $document.format -cne 'goldencheetah-windows-runtime-provenance-1' -or
      $document.qt_tls.backend -cne 'schannel' -or
      $document.python.version -cne $PythonVersion -or
      $document.python.archive_url -cne $PythonArchiveUri -or
      $document.python.archive_sha256 -cne $PythonArchiveSha256) {
    throw 'Windows runtime provenance identity does not match the build inputs'
  }

  $plugin = Get-StagedFileRecord `
    -Root $Destination -RelativePath 'tls\qschannelbackend.dll'
  if ($document.qt_tls.plugin.path -cne $plugin.path -or
      $document.qt_tls.plugin.sha256 -cne $plugin.sha256) {
    throw 'Windows runtime provenance does not match the Schannel plugin'
  }
  $expectedFiles = @(Get-PythonRuntimeFiles -Version $PythonVersion)
  $observedPaths = @($document.python.files | ForEach-Object { $_.path })
  if (($observedPaths -join "`n") -cne ($expectedFiles -join "`n")) {
    throw 'Windows runtime provenance has an unexpected Python file set'
  }
  foreach ($record in $document.python.files) {
    $actual = Get-StagedFileRecord -Root $Destination -RelativePath $record.path
    if ($record.sha256 -cne $actual.sha256) {
      throw "Windows runtime provenance digest mismatch: $($record.path)"
    }
  }
  $opensslVersion = Get-PythonOpenSslVersion `
    -Root $Destination `
    -ExpectedVersion $ExpectedOpenSslVersion
  if ($document.python.openssl_version -cne $opensslVersion) {
    throw 'Windows runtime provenance does not match the loaded Python OpenSSL'
  }
}

function Test-WindowsInstallerPayload {
  param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$PythonVersion,
    [Parameter(Mandatory = $true)][string]$PythonArchiveUri,
    [Parameter(Mandatory = $true)][string]$PythonArchiveSha256,
    [Parameter(Mandatory = $true)][string]$ExpectedOpenSslVersion
  )

  if (-not (Test-Path -LiteralPath $Installer -PathType Leaf)) {
    throw "NSIS installer is missing: $Installer"
  }
  $installRoot = Join-Path (
    [IO.Path]::GetTempPath()
  ) "goldencheetah-installed-$PID-$([Guid]::NewGuid().ToString('N'))"
  if (Test-Path -LiteralPath $installRoot) {
    throw "Temporary NSIS installation path already exists: $installRoot"
  }

  try {
    Invoke-NativeCommand -FilePath $Installer -ArgumentList @(
      '/S',
      "/D=$installRoot"
    )
    $application = Join-Path $installRoot 'GoldenCheetah.exe'
    $python = Join-Path $installRoot 'python.exe'
    $provenance = Join-Path $installRoot 'GoldenCheetah.windows-provenance.json'
    foreach ($required in @($application, $python, $provenance)) {
      if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Installed NSIS payload is missing: $required"
      }
    }

    Invoke-NativeCommand -FilePath $application -ArgumentList @(
      '--version'
    ) | Out-Null
    $applicationReport = @(
      Invoke-NativeCommand -FilePath $application -ArgumentList @(
        '--goldencheetah-build-provenance'
      )
    )
    $applicationText = ($applicationReport -join "`n") + "`n"
    if ($applicationText -notmatch '(?m)^goldencheetah_build_provenance=1$' -or
        $applicationText -notmatch '(?m)^application=GoldenCheetah$' -or
        $applicationText -notmatch '(?m)^source_revision=[0-9a-f]{40}$') {
      throw 'Installed GoldenCheetah provenance entrypoint failed'
    }

    Invoke-NativeCommand -FilePath $python -ArgumentList @(
      '-B', '-I', '-c',
      "import ssl, sys; assert sys.version.split()[0] == '$PythonVersion'; assert ssl.OPENSSL_VERSION.split()[1] == '$ExpectedOpenSslVersion'; import jinja2, lmfit, numpy, pandas, plotly, scipy"
    ) | Out-Null
    Assert-WindowsRuntimeProvenance `
      -Destination $installRoot `
      -PythonVersion $PythonVersion `
      -PythonArchiveUri $PythonArchiveUri `
      -PythonArchiveSha256 $PythonArchiveSha256 `
      -ExpectedOpenSslVersion $ExpectedOpenSslVersion
  } finally {
    $uninstaller = Join-Path $installRoot 'uninst.exe'
    if (Test-Path -LiteralPath $uninstaller -PathType Leaf) {
      try {
        Invoke-NativeCommand -FilePath $uninstaller -ArgumentList @('/S')
      } catch {
        Write-Warning "NSIS fixture uninstall failed: $($_.Exception.Message)"
      }
    }
    Remove-Item -LiteralPath $installRoot -Recurse -Force `
      -ErrorAction SilentlyContinue
  }
}

if ($env:GC_BUILD_ACQUISITION_HELPERS_ONLY -eq '1') {
  return
}

$repositoryRoot = (Get-Location).Path
$oauthChecker = Join-Path $repositoryRoot 'appveyor/check-unconfigured-oauth.py'
$pythonVersion = '3.13.14'
$pythonOpenSslVersion = '3.0.21'
$pythonArchive = "python-$pythonVersion-embed-amd64.zip"
$pythonSha256 = '90b4e5b9898b72d744650524bff92377c367f44bd5fbd09e3148656c080ad907'
$getPipFile = 'get-pip-20260805.py'
$getPipSha256 = 'fb24e693bab954209a063d90953621412ccad4a500905a726286e038f508ddf6'
$pipVersion = '26.2.1'
$pipWheel = "pip-$pipVersion-py3-none-any.whl"
$pipSha256 = '71138adf1f4ca900cdb7d289c21b7494329f2332b6d85f0e1c42108c0384ed3e'
$dependencyRelease = 'https://github.com/kaartine/GoldenCheetah/releases/download/appimage-build-deps-v1'
$pythonArchiveUri = "https://www.python.org/ftp/python/$pythonVersion/$pythonArchive"
$requirements = (Resolve-Path 'src\Python\requirements-appimage.lock').Path
$pythonRequired = @(
  '_ssl.pyd',
  'libcrypto-3.dll',
  'libssl-3.dll',
  'python.exe',
  'python313.dll',
  'python313._pth',
  'LICENSE.txt',
  'lib\site-packages\numpy\__init__.py',
  'lib\site-packages\pandas\__init__.py',
  'lib\site-packages\scipy\__init__.py',
  'lib\site-packages\plotly\__init__.py',
  'lib\site-packages\jinja2\__init__.py'
)
$pythonRoot = 'C:\Python'

$downloadRoot = Join-Path ([IO.Path]::GetTempPath()) 'goldencheetah-python-downloads'
New-Item -ItemType Directory -Path $downloadRoot -Force | Out-Null
$pythonArchivePath = Join-Path $downloadRoot $pythonArchive
$getPipPath = Join-Path $downloadRoot $getPipFile
$pipWheelPath = Join-Path $downloadRoot $pipWheel
Get-VerifiedDownload -Uri $pythonArchiveUri -Destination $pythonArchivePath -ExpectedSha256 $pythonSha256
Get-VerifiedDownload -Uri "$dependencyRelease/$getPipFile" -Destination $getPipPath -ExpectedSha256 $getPipSha256
Get-VerifiedDownload `
  -Uri "https://files.pythonhosted.org/packages/f3/6e/1736e5b4ae2b778ef2f81c47d797de9f891d4d8acb047a24ca37a60294dd/$pipWheel" `
  -Destination $pipWheelPath `
  -ExpectedSha256 $pipSha256

$stage = "$pythonRoot.gc-stage-$PID"
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
try {
  $safeExtractor = Join-Path $repositoryRoot 'appveyor\safe-extract.py'
  Invoke-NativeCommand -FilePath 'python.exe' -ArgumentList @(
    $safeExtractor, '--format', 'zip', '--archive', $pythonArchivePath,
    '--destination', $stage
  )
  $sitePackages = Join-Path $stage 'lib\site-packages'
  New-Item -ItemType Directory -Path $sitePackages -Force | Out-Null
  $pthPath = Join-Path $stage 'python313._pth'
  $pth = [IO.File]::ReadAllText($pthPath)
  if ($pth -notmatch '(?m)^#import site$') {
    throw "Embedded Python path file has an unexpected format: $pthPath"
  }
  $pth = $pth -replace '(?m)^#import site$', 'import site'
  [IO.File]::WriteAllText($pthPath, $pth, [Text.Encoding]::ASCII)

  $env:PYTHONDONTWRITEBYTECODE = '1'
  Invoke-NativeCommand -FilePath (Join-Path $stage 'python.exe') -ArgumentList @(
    '-B', $getPipPath,
    '--disable-pip-version-check',
    '--no-warn-script-location',
    '--no-index',
    "--find-links=$downloadRoot",
    '--no-setuptools',
    '--no-wheel',
    "pip==$pipVersion"
  )
  Invoke-NativeCommand -FilePath (Join-Path $stage 'python.exe') -ArgumentList @(
    '-B', '-I', '-m', 'pip', 'install',
    '--isolated',
    '--disable-pip-version-check',
    '--no-input',
    '--no-cache-dir',
    '--no-compile',
    '--ignore-installed',
    '--require-hashes',
    '--only-binary', ':all:',
    '--index-url=https://pypi.org/simple',
    '-r', $requirements,
    '-t', $sitePackages
  )
  foreach ($scriptDirectory in @(
    (Join-Path $stage 'Scripts'),
    (Join-Path $sitePackages 'bin')
  )) {
    Remove-Item -LiteralPath $scriptDirectory -Recurse -Force -ErrorAction SilentlyContinue
  }
  Get-ChildItem -LiteralPath $stage -Directory -Filter '__pycache__' -Recurse -Force |
    Remove-Item -Recurse -Force
  Get-ChildItem -LiteralPath $stage -File -Filter '*.pyc' -Recurse -Force |
    Remove-Item -Force

  Assert-RequiredFiles -Root $stage -RequiredFiles $pythonRequired
  Invoke-NativeCommand -FilePath (Join-Path $stage 'python.exe') -ArgumentList @(
    '-B', '-I', '-c',
    "import ssl, sys; assert sys.version.split()[0] == '$pythonVersion'; assert ssl.OPENSSL_VERSION.split()[1] == '$pythonOpenSslVersion'; import numpy, pandas, scipy, plotly, jinja2"
  ) | Out-Null
  Invoke-NativeCommand -FilePath (Join-Path $stage 'python.exe') -ArgumentList @(
    '-B', '-I', '-m', 'pip', '--disable-pip-version-check', 'check'
  ) | Out-Null
  Assert-NoFileContainsText -Root $stage -Text $stage
  Remove-Item -LiteralPath $pythonRoot -Recurse -Force -ErrorAction SilentlyContinue
  Move-Item -LiteralPath $stage -Destination $pythonRoot
} finally {
  Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}

Invoke-NativeCommand -FilePath 'python.exe' -ArgumentList @(
  $oauthChecker, (Join-Path $repositoryRoot 'src\release\GoldenCheetah.exe')
)
Set-Location 'src\release'
Invoke-NativeCommand -FilePath 'windeployqt.exe' -ArgumentList @(
  '--release', '--qmldir', '..\Train\qml', 'GoldenCheetah.exe'
)
Use-WindowsSchannelTlsBackend -Destination (Get-Location).Path
Copy-Item 'C:\LIBS\10_Precompiled_DLL\usbexpress_3.5.1\USBXpress\USBXpress_API\Host\x64\SiUSBXp.dll' .
Copy-Item 'C:\LIBS\10_Precompiled_DLL\libsamplerate64\lib\libsamplerate-0.dll' .
Get-ChildItem -LiteralPath 'C:\Python' -Force |
  Copy-Item -Destination . -Recurse -Force
Copy-Item 'C:\Python\LICENSE.txt' 'PYTHON LICENSE.txt'
Copy-Item 'C:\tools\vcpkg\installed\x64-windows\bin\gsl*.dll' .

Copy-Item '..\Resources\win32\ReadMe.txt' .
'GoldenCheetah is licensed under the GNU General Public License v2' | Set-Content 'license.txt'
Add-Content 'license.txt' ''
Get-Content '..\..\COPYING' | Add-Content 'license.txt'
Copy-Item '..\Resources\win32\gc.ico' .
Copy-Item '..\Resources\win32\GC3.8-Master-W64-QT6.nsi' .

Write-WindowsRuntimeProvenance `
  -Destination (Get-Location).Path `
  -PythonVersion $pythonVersion `
  -PythonArchiveUri $pythonArchiveUri `
  -PythonArchiveSha256 $pythonSha256 `
  -ExpectedOpenSslVersion $pythonOpenSslVersion

Invoke-NativeCommand -FilePath 'python.exe' -ArgumentList @(
  $oauthChecker, (Join-Path (Get-Location).Path 'GoldenCheetah.exe')
)
Assert-WindowsRuntimeProvenance `
  -Destination (Get-Location).Path `
  -PythonVersion $pythonVersion `
  -PythonArchiveUri $pythonArchiveUri `
  -PythonArchiveSha256 $pythonSha256 `
  -ExpectedOpenSslVersion $pythonOpenSslVersion
Invoke-NativeCommand -FilePath 'makensis.exe' -ArgumentList @('.\GC3.8-Master-W64-QT6.nsi')
$installer = Join-Path $repositoryRoot 'GoldenCheetah_v3.8_x64.exe'
Move-Item 'GoldenCheetah_v3.8_64bit_Windows.exe' $installer -Force
Set-Location $repositoryRoot
Test-WindowsInstallerPayload `
  -Installer $installer `
  -PythonVersion $pythonVersion `
  -PythonArchiveUri $pythonArchiveUri `
  -PythonArchiveSha256 $pythonSha256 `
  -ExpectedOpenSslVersion $pythonOpenSslVersion
