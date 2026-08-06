Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$repositoryRoot = [IO.Path]::GetFullPath(
  (Join-Path $PSScriptRoot '../..')
)
$testRoot = Join-Path $repositoryRoot 'unittests/Build/appImagePackaging'

& (Join-Path $testRoot 'testGeneratedSecrets.ps1') `
  -RepositoryRoot $repositoryRoot
& (Join-Path $testRoot 'testWindowsPackaging.ps1') `
  -RepositoryRoot $repositoryRoot

Write-Output 'PASS: native Windows BUILD-001 regressions'
