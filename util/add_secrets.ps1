# Generate an ignored CI-only header without exposing secrets on command lines.
param(
  [Parameter(Position=0)] [string]$f = "src/Core/GeneratedSecrets.h"
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

function ConvertTo-CEncodedUtf8 {
  param([AllowEmptyString()][string]$Value)

  $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
  if ($bytes.Length -eq 0) {
    return '""'
  }
  return (($bytes | ForEach-Object {
    '"\{0}"' -f ([Convert]::ToString($_, 8).PadLeft(3, '0'))
  }) -join '')
}

function Assert-SafeSecretPath {
  param(
    [Parameter(Mandatory = $true)][string]$Path,
    [switch]$AllowMissing
  )

  if (-not (Test-Path -LiteralPath $Path)) {
    if ($AllowMissing) {
      return
    }
    throw "Missing generated-secret path: $Path"
  }
  $item = Get-Item -LiteralPath $Path -Force
  if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
    throw "Refusing linked generated-secret path: $Path"
  }
}

function Set-PrivateSecretFilePermissions {
  param([Parameter(Mandatory = $true)][string]$Path)

  if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    $security = New-Object Security.AccessControl.FileSecurity
    $security.SetOwner($identity)
    $security.SetAccessRuleProtection($true, $false)
    $rule = [Security.AccessControl.FileSystemAccessRule]::new(
      $identity,
      [Security.AccessControl.FileSystemRights]::FullControl,
      [Security.AccessControl.InheritanceFlags]::None,
      [Security.AccessControl.PropagationFlags]::None,
      [Security.AccessControl.AccessControlType]::Allow
    )
    [void]$security.AddAccessRule($rule)
    Set-Acl -LiteralPath $Path -AclObject $security
  } else {
    & chmod 600 $Path
    if ($LASTEXITCODE -ne 0) {
      throw "Cannot restrict generated secret file permissions: $Path"
    }
  }
}

function New-PrivateSecretTemporaryFile {
  param([Parameter(Mandatory = $true)][string]$Path)

  $stream = [IO.File]::Open(
    $Path,
    [IO.FileMode]::CreateNew,
    [IO.FileAccess]::ReadWrite,
    [IO.FileShare]::None
  )
  try {
    # The new file is still empty here. Restrict it before writing any secret.
    Set-PrivateSecretFilePermissions -Path $Path
    return $stream
  } catch {
    $stream.Dispose()
    Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    throw
  }
}

function Remove-StaleSecretTemporaryFiles {
  param([Parameter(Mandatory = $true)][string]$TargetPath)

  $directory = Split-Path -Parent $TargetPath
  $name = Split-Path -Leaf $TargetPath
  Get-ChildItem -LiteralPath $directory -Filter "$name.tmp-*" -Force |
    ForEach-Object {
      if ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) {
        Remove-Item -LiteralPath $_.FullName -Force
      } elseif (-not $_.PSIsContainer) {
        Remove-Item -LiteralPath $_.FullName -Force
      } else {
        throw "Refusing unexpected generated-secret temporary directory: $($_.FullName)"
      }
    }
}

if ($env:GC_SECRET_GENERATOR_HELPERS_ONLY -eq '1') {
  return
}

$definitions = [ordered]@{
  GC_CLOUD_OPENDATA_SECRET = $env:GC_CLOUD_OPENDATA_SECRET
  GC_NOKIA_CLIENT_SECRET = $env:GC_NOKIA_CLIENT_SECRET
  GC_DROPBOX_CLIENT_SECRET = $env:GC_DROPBOX_CLIENT_SECRET
  GC_STRAVA_CLIENT_SECRET = $env:GC_STRAVA_CLIENT_SECRET
  GC_CYCLINGANALYTICS_CLIENT_SECRET = $env:GC_CYCLINGANALYTICS_CLIENT_SECRET
  GC_CLOUD_DB_BASIC_AUTH = $env:GC_CLOUD_DB_BASIC_AUTH
  GC_CLOUD_DB_APP_NAME = $env:GC_CLOUD_DB_APP_NAME
  GC_POLARFLOW_CLIENT_SECRET = $env:GC_POLARFLOW_CLIENT_SECRET
  GC_SPORTTRACKS_CLIENT_SECRET = $env:GC_SPORTTRACKS_CLIENT_SECRET
  GC_RWGPS_API_KEY = $env:GC_RWGPS_API_KEY
  GC_NOLIO_CLIENT_ID = $env:GC_NOLIO_CLIENT_ID
  GC_NOLIO_SECRET = $env:GC_NOLIO_SECRET
  GC_XERT_CLIENT_SECRET = $env:GC_XERT_CLIENT_SECRET
  GC_AZUM_CLIENT_SECRET = $env:GC_AZUM_CLIENT_SECRET
  GC_TRAINERDAY_API_KEY = $env:GC_TRAINERDAY_API_KEY
}

$targetPath = [IO.Path]::GetFullPath($f)
$targetDirectory = Split-Path -Parent $targetPath
if (-not (Test-Path -LiteralPath $targetDirectory)) {
  New-Item -ItemType Directory -Path $targetDirectory | Out-Null
}
Assert-SafeSecretPath -Path $targetDirectory
Assert-SafeSecretPath -Path $targetPath -AllowMissing
Remove-StaleSecretTemporaryFiles -TargetPath $targetPath

$temporary = "$targetPath.tmp-$PID-$([guid]::NewGuid().ToString('N'))"
$stream = $null
$writer = $null
try {
  $lines = @(
    '#ifndef GC_GENERATED_SECRETS_H'
    '#define GC_GENERATED_SECRETS_H 1'
    ''
  )
  foreach ($definition in $definitions.GetEnumerator()) {
    $value = [string]$definition.Value
    if ([string]::IsNullOrEmpty($value)) {
      continue
    }
    $encoded = ConvertTo-CEncodedUtf8 -Value $value
    $lines += '#define {0} {1}' -f $definition.Key, $encoded
  }
  $lines += ''
  $lines += '#endif'

  $stream = New-PrivateSecretTemporaryFile -Path $temporary
  $utf8WithoutBom = [Text.UTF8Encoding]::new($false)
  $writer = [IO.StreamWriter]::new($stream, $utf8WithoutBom, 4096, $true)
  foreach ($line in $lines) {
    $writer.WriteLine($line)
  }
  $writer.Flush()
  $writer.Dispose()
  $writer = $null
  $stream.Flush($true)
  $stream.Dispose()
  $stream = $null

  Move-Item -LiteralPath $temporary -Destination $targetPath -Force
  Set-PrivateSecretFilePermissions -Path $targetPath
} finally {
  if ($null -ne $writer) {
    $writer.Dispose()
  }
  if ($null -ne $stream) {
    $stream.Dispose()
  }
  Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
}
