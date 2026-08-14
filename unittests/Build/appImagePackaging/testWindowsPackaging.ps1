param(
  [Parameter(Mandatory = $true)][string]$RepositoryRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$installScript = Join-Path $RepositoryRoot 'appveyor/windows/install.ps1'
$packagingScript = Join-Path $RepositoryRoot 'appveyor/windows/after_build.ps1'
$openSslLock = Join-Path $RepositoryRoot 'appveyor/windows/openssl-runtime.lock.json'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) "gc-windows-packaging-test-$PID"
$dependencyRoot = Join-Path $temporaryRoot 'dependency'
$payloadRoot = Join-Path $temporaryRoot 'payload'

try {
  New-Item -ItemType Directory -Path $dependencyRoot | Out-Null
  [IO.File]::WriteAllText(
    (Join-Path $dependencyRoot '.gc-dependency-complete'),
    'forgeable marker'
  )
  [IO.File]::WriteAllText((Join-Path $dependencyRoot 'hostile.exe'), 'stale')

  $env:GC_BUILD_ACQUISITION_HELPERS_ONLY = '1'
  . $installScript

  function Get-VerifiedDownload {
    param($Uri, $Destination, $ExpectedSha256)
    [IO.File]::WriteAllText($Destination, 'verified archive fixture')
  }
  function Invoke-NativeCommand {
    param($FilePath, $ArgumentList)
    $destinationIndex = [Array]::IndexOf($ArgumentList, '--destination')
    if ($FilePath -notmatch 'python' -or $destinationIndex -lt 0) {
      throw 'Unexpected extraction command in fixture'
    }
    $stage = $ArgumentList[$destinationIndex + 1]
    $payload = Join-Path $stage 'tools'
    New-Item -ItemType Directory -Path $payload | Out-Null
    [IO.File]::WriteAllText((Join-Path $payload 'trusted.exe'), 'trusted')
  }
  Install-ZipDependency `
    -Root $dependencyRoot `
    -RequiredFiles @('trusted.exe') `
    -Uri 'https://invalid.example/dependency.zip' `
    -ArchiveName "gc-cache-fixture-$PID.zip" `
    -ArchiveSha256 ('0' * 64) `
    -PayloadSubdirectory 'tools'
  if (-not (Test-Path -LiteralPath (Join-Path $dependencyRoot 'trusted.exe') -PathType Leaf) -or
      (Test-Path -LiteralPath (Join-Path $dependencyRoot 'tools')) -or
      (Test-Path -LiteralPath (Join-Path $dependencyRoot 'hostile.exe')) -or
      (Test-Path -LiteralPath (Join-Path $dependencyRoot '.gc-dependency-complete'))) {
    throw 'Verified dependency reconstruction retained a forgeable cached payload'
  }

  . $packagingScript
  if (Test-Path -LiteralPath $openSslLock) {
    throw 'Windows packaging still has a separate OpenSSL runtime lock'
  }
  $reportedOpenSsl = 'OpenSSL 3.0.21 27 May 2026'
  if ((Assert-PythonOpenSslVersion `
        -ReportedVersion $reportedOpenSsl `
        -ExpectedVersion '3.0.21') -cne $reportedOpenSsl) {
    throw 'Exact Python OpenSSL validation changed its reported value'
  }
  $rejected = $false
  try {
    Assert-PythonOpenSslVersion `
      -ReportedVersion 'OpenSSL 3.0.20 11 Feb 2025' `
      -ExpectedVersion '3.0.21' | Out-Null
  } catch {
    $rejected = $true
  }
  if (-not $rejected) {
    throw 'Python OpenSSL validation accepted the wrong patch release'
  }

  New-Item -ItemType Directory -Path (Join-Path $payloadRoot 'tls') -Force | Out-Null
  foreach ($relativePath in @(
    '_ssl.pyd',
    'libcrypto-3.dll',
    'libssl-3.dll',
    'PYTHON LICENSE.txt',
    'python.exe',
    'python313._pth',
    'python313.dll',
    'python313.zip',
    'tls\qschannelbackend.dll',
    'tls\qopensslbackend.dll',
    'libcrypto-1_1-x64.dll',
    'libssl-1_1-x64.dll',
    'libcrypto-3-x64.dll',
    'libssl-3-x64.dll'
  )) {
    $path = Join-Path $payloadRoot $relativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null
    [IO.File]::WriteAllText($path, "authenticated $relativePath")
  }

  Use-WindowsSchannelTlsBackend -Destination $payloadRoot
  if (-not (Test-Path -LiteralPath (Join-Path $payloadRoot 'tls\qschannelbackend.dll') -PathType Leaf)) {
    throw 'Schannel backend was removed from the staged payload'
  }
  foreach ($forbidden in @(
    'tls\qopensslbackend.dll',
    'libcrypto-1_1-x64.dll',
    'libssl-1_1-x64.dll',
    'libcrypto-3-x64.dll',
    'libssl-3-x64.dll'
  )) {
    if (Test-Path -LiteralPath (Join-Path $payloadRoot $forbidden)) {
      throw "Schannel staging retained an OpenSSL backend file: $forbidden"
    }
  }

  function Get-PythonOpenSslVersion {
    param([string]$Root, [string]$ExpectedVersion)
    return 'OpenSSL 3.0.21 27 May 2026'
  }
  $pythonVersion = '3.13.14'
  $pythonOpenSslVersion = '3.0.21'
  $pythonArchiveUri = 'https://example.invalid/python-3.13.14-embed-amd64.zip'
  $pythonArchiveSha256 = 'a' * 64
  Write-WindowsRuntimeProvenance `
    -Destination $payloadRoot `
    -PythonVersion $pythonVersion `
    -PythonArchiveUri $pythonArchiveUri `
    -PythonArchiveSha256 $pythonArchiveSha256 `
    -ExpectedOpenSslVersion $pythonOpenSslVersion
  Assert-WindowsRuntimeProvenance `
    -Destination $payloadRoot `
    -PythonVersion $pythonVersion `
    -PythonArchiveUri $pythonArchiveUri `
    -PythonArchiveSha256 $pythonArchiveSha256 `
    -ExpectedOpenSslVersion $pythonOpenSslVersion

  $provenancePath = Join-Path $payloadRoot 'GoldenCheetah.windows-provenance.json'
  $provenance = Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json
  if ($provenance.format -cne 'goldencheetah-windows-runtime-provenance-1' -or
      $provenance.qt_tls.backend -cne 'schannel' -or
      $provenance.python.openssl_version -cne 'OpenSSL 3.0.21 27 May 2026' -or
      $provenance.python.archive_sha256 -cne $pythonArchiveSha256) {
    throw 'Windows runtime provenance does not identify Schannel and pinned Python'
  }
  $expectedPythonFiles = @(
    '_ssl.pyd', 'libcrypto-3.dll', 'libssl-3.dll', 'PYTHON LICENSE.txt',
    'python.exe', 'python313._pth', 'python313.dll', 'python313.zip'
  ) | Sort-Object
  $observedPythonFiles = @($provenance.python.files.path | Sort-Object)
  if (($expectedPythonFiles -join "`n") -cne ($observedPythonFiles -join "`n")) {
    throw 'Windows runtime provenance does not cover the final Python TLS payload'
  }

  [IO.File]::WriteAllText((Join-Path $payloadRoot 'libssl-3.dll'), 'substituted')
  $rejected = $false
  try {
    Assert-WindowsRuntimeProvenance `
      -Destination $payloadRoot `
      -PythonVersion $pythonVersion `
      -PythonArchiveUri $pythonArchiveUri `
      -PythonArchiveSha256 $pythonArchiveSha256 `
      -ExpectedOpenSslVersion $pythonOpenSslVersion
  } catch {
    $rejected = $true
  }
  if (-not $rejected) {
    throw 'Windows runtime provenance accepted a substituted OpenSSL DLL'
  }
  [IO.File]::WriteAllText(
    (Join-Path $payloadRoot 'libssl-3.dll'),
    'authenticated libssl-3.dll'
  )

  [IO.File]::WriteAllText(
    (Join-Path $payloadRoot 'tls\qopensslbackend.dll'),
    'late unprovenanced Qt OpenSSL backend'
  )
  $rejected = $false
  try {
    Assert-WindowsRuntimeProvenance `
      -Destination $payloadRoot `
      -PythonVersion $pythonVersion `
      -PythonArchiveUri $pythonArchiveUri `
      -PythonArchiveSha256 $pythonArchiveSha256 `
      -ExpectedOpenSslVersion $pythonOpenSslVersion
  } catch {
    $rejected = $true
  }
  if (-not $rejected) {
    throw 'Final Windows staging accepted a late Qt OpenSSL backend'
  }

  [IO.File]::WriteAllText((Join-Path $payloadRoot 'launcher.exe'), $payloadRoot)
  $rejected = $false
  try {
    Assert-NoFileContainsText -Root $payloadRoot -Text $payloadRoot
  } catch {
    $rejected = $true
  }
  if (-not $rejected) {
    throw 'Windows Python payload accepted a staging-path launcher'
  }
  [IO.File]::WriteAllText((Join-Path $payloadRoot 'launcher.exe'), 'relocatable')
  Assert-NoFileContainsText -Root $payloadRoot -Text $payloadRoot

  $installerFixture = Join-Path $temporaryRoot 'GoldenCheetah-installer.exe'
  [IO.File]::WriteAllText($installerFixture, 'NSIS fixture')
  $script:installerSmokeCalls = @()
  function Invoke-NativeCommand {
    param($FilePath, $ArgumentList)
    $script:installerSmokeCalls += [ordered]@{
      path = $FilePath
      arguments = @($ArgumentList)
    }
    if ($FilePath -ceq $installerFixture) {
      if ($ArgumentList[0] -cne '/S' -or
          $ArgumentList[-1] -notmatch '^/D=') {
        throw 'Installer fixture was not invoked silently with /D last'
      }
      $root = $ArgumentList[-1].Substring(3)
      New-Item -ItemType Directory -Path $root | Out-Null
      foreach ($name in @(
        'GoldenCheetah.exe', 'python.exe',
        'GoldenCheetah.windows-provenance.json'
      )) {
        [IO.File]::WriteAllText((Join-Path $root $name), 'installed fixture')
      }
      return
    }
    if ((Split-Path -Leaf $FilePath) -ceq 'GoldenCheetah.exe') {
      return @(
        'goldencheetah_build_provenance=1',
        'application=GoldenCheetah',
        ('source_revision=' + ('1' * 40))
      )
    }
    if ((Split-Path -Leaf $FilePath) -ceq 'python.exe') {
      return 'installed Python smoke passed'
    }
    throw "Unexpected installer smoke command: $FilePath"
  }
  $script:provenanceSmokeRoot = $null
  function Assert-WindowsRuntimeProvenance {
    param(
      $Destination,
      $PythonVersion,
      $PythonArchiveUri,
      $PythonArchiveSha256,
      $ExpectedOpenSslVersion
    )
    $script:provenanceSmokeRoot = $Destination
  }
  Test-WindowsInstallerPayload `
    -Installer $installerFixture `
    -PythonVersion $pythonVersion `
    -PythonArchiveUri $pythonArchiveUri `
    -PythonArchiveSha256 ('a' * 64) `
    -ExpectedOpenSslVersion $pythonOpenSslVersion
  $installedApplicationCalls = @(
    $script:installerSmokeCalls | Where-Object {
      (Split-Path -Leaf $_.path) -ceq 'GoldenCheetah.exe'
    }
  )
  $installedPythonCalls = @(
    $script:installerSmokeCalls | Where-Object {
      (Split-Path -Leaf $_.path) -ceq 'python.exe'
    }
  )
  if ($installedApplicationCalls.Count -ne 2 -or
      $installedPythonCalls.Count -ne 1 -or
      $null -eq $script:provenanceSmokeRoot) {
    throw 'Installed NSIS payload did not run app, Python, and provenance smoke'
  }

  Write-Output 'PASS: Windows dependencies and Python staging fail closed'
} finally {
  Remove-Item Env:GC_BUILD_ACQUISITION_HELPERS_ONLY -ErrorAction SilentlyContinue
  Remove-Item Env:GITHUB_ACTIONS -ErrorAction SilentlyContinue
  Remove-Item Env:RUNNER_TOOL_CACHE -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath (Join-Path ([IO.Path]::GetTempPath()) "gc-cache-fixture-$PID.zip") -Force -ErrorAction SilentlyContinue
}
