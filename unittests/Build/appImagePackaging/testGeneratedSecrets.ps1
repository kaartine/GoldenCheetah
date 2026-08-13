param(
  [Parameter(Mandatory = $true)][string]$RepositoryRoot
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$generator = Join-Path $RepositoryRoot 'util/add_secrets.ps1'
$defaultsHeader = Join-Path $RepositoryRoot 'src/Core/Secrets.h'
$temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) "gc-secret-test-$PID"
$header = Join-Path $temporaryRoot 'GeneratedSecrets.h'
$emptyHeader = Join-Path $temporaryRoot 'EmptyGeneratedSecrets.h'
$privateTemporary = Join-Path $temporaryRoot 'PrivateTemporary.h'
$secret = "visible-secret-$PID-`$-quote`"-utf8-" + [char]0x00e4

function Decode-CEncodedUtf8 {
  param([Parameter(Mandatory = $true)][string]$Encoded)

  $matches = [regex]::Matches($Encoded, '"\\([0-7]{3})"')
  $bytes = New-Object byte[] $matches.Count
  for ($index = 0; $index -lt $matches.Count; $index++) {
    $bytes[$index] = [Convert]::ToByte($matches[$index].Groups[1].Value, 8)
  }
  return [Text.Encoding]::UTF8.GetString($bytes)
}

try {
  New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
  $env:GC_SECRET_GENERATOR_HELPERS_ONLY = '1'
  . $generator
  Remove-Item Env:GC_SECRET_GENERATOR_HELPERS_ONLY
  $privateStream = New-PrivateSecretTemporaryFile -Path $privateTemporary
  try {
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
      $acl = Get-Acl -LiteralPath $privateTemporary
      if (-not $acl.AreAccessRulesProtected) {
        throw 'Secret temporary file inherits Windows ACL entries'
      }
      $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
      $allowed = @($acl.Access | Where-Object {
        $_.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow
      })
      if ($allowed.Count -ne 1 -or
          $allowed[0].IdentityReference.Translate(
            [Security.Principal.SecurityIdentifier]
          ).Value -ne $currentUser) {
        throw 'Secret temporary file is accessible by another Windows identity'
      }
    } else {
      $runningOnMacOS = [Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [Runtime.InteropServices.OSPlatform]::OSX
      )
      $temporaryMode = if ($runningOnMacOS) {
        (& stat -f '%Lp' $privateTemporary).Trim()
      } else {
        (& stat -c '%a' $privateTemporary).Trim()
      }
      if ($LASTEXITCODE -ne 0 -or $temporaryMode -ne '600') {
        throw 'Secret temporary file is not private before secret data is written'
      }
    }
  } finally {
    $privateStream.Dispose()
    Remove-Item -LiteralPath $privateTemporary -Force -ErrorAction SilentlyContinue
  }

  $staleTemporary = "$header.tmp-stale"
  [IO.File]::WriteAllText($staleTemporary, 'stale-secret')
  $env:GC_STRAVA_CLIENT_SECRET = $secret
  $env:GC_CLOUD_OPENDATA_SECRET = 'opendata-enabled'
  & $generator $header

  if (Test-Path -LiteralPath $staleTemporary) {
    throw 'Stale generated-secret temporary file was not cleaned'
  }
  if (@(Get-ChildItem -LiteralPath $temporaryRoot -Filter 'GeneratedSecrets.h.tmp-*').Count -ne 0) {
    throw 'Generated-secret temporary files remain after a successful write'
  }

  $content = [IO.File]::ReadAllText($header)
  if ($content.Contains($secret) -or $content.Contains('opendata-enabled')) {
    throw 'Generated header contains a raw secret value'
  }
  if ($content.Contains('DEFINES +=')) {
    throw 'Generated header contains a qmake compiler definition'
  }
  $stravaLines = @($content -split "`n" |
    Where-Object { $_ -match '^#define GC_STRAVA_CLIENT_SECRET ' })
  if ($stravaLines.Count -ne 1) {
    throw 'Generated Strava secret definition is missing or duplicated'
  }
  $stravaLine = $stravaLines[0]
  $encoded = $stravaLine.Substring($stravaLine.IndexOf('"'))
  if ((Decode-CEncodedUtf8 -Encoded $encoded) -cne $secret) {
    throw 'Generated Strava secret does not round-trip as UTF-8'
  }
  if ($content -notmatch '(?m)^#define GC_CLOUD_OPENDATA_SECRET ') {
    throw 'OpenData credential definition is missing'
  }

  Remove-Item Env:GC_STRAVA_CLIENT_SECRET -ErrorAction SilentlyContinue
  $env:GC_CLOUD_OPENDATA_SECRET = ''
  & $generator $emptyHeader
  $emptyContent = [IO.File]::ReadAllText($emptyHeader)
  if ($emptyContent -match '(?m)^#define GC_CLOUD_OPENDATA_SECRET ' -or
      $emptyContent -match '(?m)^#define GC_STRAVA_CLIENT_SECRET ') {
    throw 'Generated header defines a credential with an empty environment value'
  }
  $defaults = [IO.File]::ReadAllText($defaultsHeader)
  if ($defaults -notmatch '(?s)#ifndef GC_CLOUD_OPENDATA_SECRET\s+#define OPENDATA_DISABLE\s+#define GC_CLOUD_OPENDATA_SECRET') {
    throw 'Secrets.h no longer disables OpenData when its credential is absent'
  }
  if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    $finalAcl = Get-Acl -LiteralPath $header
    $finalAllowed = @($finalAcl.Access | Where-Object {
      $_.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow
    })
    $currentUser = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
    if (-not $finalAcl.AreAccessRulesProtected -or
        $finalAllowed.Count -ne 1 -or
        $finalAllowed[0].IdentityReference.Translate(
          [Security.Principal.SecurityIdentifier]
        ).Value -ne $currentUser) {
      throw 'Generated secret header has a non-private Windows ACL'
    }
  } else {
    $runningOnMacOS = [Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
      [Runtime.InteropServices.OSPlatform]::OSX
    )
    $mode = if ($runningOnMacOS) {
      (& stat -f '%Lp' $header).Trim()
    } else {
      (& stat -c '%a' $header).Trim()
    }
    if ($LASTEXITCODE -ne 0 -or $mode -ne '600') {
      throw 'Generated secret header permissions are not private'
    }
  }
  Write-Output 'PASS: generated secrets are encoded outside compiler arguments'
} finally {
  Remove-Item Env:GC_STRAVA_CLIENT_SECRET -ErrorAction SilentlyContinue
  Remove-Item Env:GC_CLOUD_OPENDATA_SECRET -ErrorAction SilentlyContinue
  Remove-Item Env:GC_SECRET_GENERATOR_HELPERS_ONLY -ErrorAction SilentlyContinue
  Remove-Item -LiteralPath $temporaryRoot -Recurse -Force -ErrorAction SilentlyContinue
}
