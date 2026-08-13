#!/usr/bin/env python3

import json
from pathlib import Path
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
PACKAGER = REPOSITORY_ROOT / "appveyor/windows/after_build.ps1"
INSTALL_SCRIPT = REPOSITORY_ROOT / "appveyor/windows/install.ps1"
BEFORE_BUILD = REPOSITORY_ROOT / "appveyor/windows/before_build.ps1"
LOCK = REPOSITORY_ROOT / "appveyor/windows/openssl-runtime.lock.json"
INSTALLER = REPOSITORY_ROOT / "src/Resources/win32/GC3.8-Master-W64-QT6.nsi"
VCPKG_MANIFEST = REPOSITORY_ROOT / "appveyor/windows/vcpkg.json"
APPVEYOR = REPOSITORY_ROOT / "appveyor.yml"
CI_WORKFLOW = REPOSITORY_ROOT / ".github" / "workflows" / "ci.yml"
PYTHON_VERSION = "3.13.14"
PYTHON_ABI = "313"
PYTHON_ARCHIVE_SHA256 = (
    "90b4e5b9898b72d744650524bff92377c367f44bd5fbd09e3148656c080ad907"
)
PYTHON_INSTALLER_SHA256 = (
    "c54d9b9bbb8a36e6489363ddd01139707fd781d72f1f9e90c7ec65d0061368e0"
)
OPENSSL_VERSION = "3.0.21"


class WindowsOpenSslTests(unittest.TestCase):
    def test_schannel_uses_the_pinned_python_openssl_runtime(self):
        self.assertFalse(LOCK.exists())
        packager = PACKAGER.read_text(encoding="utf-8")
        self.assertNotIn(r"C:\OpenSSL-Win64", packager)
        self.assertNotIn("Install-VerifiedOpenSslRuntime", packager)
        self.assertNotIn("openssl-runtime.lock.json", packager)
        self.assertIn("Use-WindowsSchannelTlsBackend", packager)
        self.assertIn("Write-WindowsRuntimeProvenance", packager)
        self.assertIn("Assert-WindowsRuntimeProvenance", packager)
        self.assertIn("import ssl", packager)
        for required in ("_ssl.pyd", "libcrypto-3.dll", "libssl-3.dll"):
            self.assertIn(f"'{required}'", packager)
        self.assertIn(f"$pythonVersion = '{PYTHON_VERSION}'", packager)
        self.assertIn(f"$pythonOpenSslVersion = '{OPENSSL_VERSION}'", packager)
        self.assertIn(PYTHON_ARCHIVE_SHA256, packager)
        self.assertNotIn("startswith('OpenSSL 3.')", packager)
        self.assertIn("-ExpectedOpenSslVersion $pythonOpenSslVersion", packager)

        vcpkg = json.loads(VCPKG_MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(vcpkg["dependencies"], ["gsl"])

        appveyor = APPVEYOR.read_text(encoding="utf-8")
        self.assertIn(r"C:\Python313-x64", appveyor)
        self.assertNotIn(r"C:\Python311-x64", appveyor)
        regression = appveyor.index("appveyor/windows/run-build-regressions.ps1")
        packaging = appveyor.index("./appveyor/windows/after_build.ps1")
        self.assertLess(regression, packaging)

    def test_windows_build_uses_the_same_hash_pinned_python_abi(self):
        install_script = INSTALL_SCRIPT.read_text(encoding="utf-8")
        before_build = BEFORE_BUILD.read_text(encoding="utf-8")
        workflow = CI_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn(f"$pythonBuildVersion = '{PYTHON_VERSION}'", install_script)
        self.assertIn(PYTHON_INSTALLER_SHA256, install_script)
        self.assertIn("Install-VerifiedPythonBuild", install_script)
        self.assertIn("python313.lib", install_script)
        self.assertIn("sys.version.split()[0]", install_script)
        self.assertIn("Resolve-GitHubRunnerPythonBuildRoot", install_script)
        self.assertIn("$env:RUNNER_TOOL_CACHE", install_script)
        self.assertIn("$env:GITHUB_ACTIONS", install_script)
        self.assertNotIn(
            '$env:PATH = "C:\\Python313-x64\\Scripts;C:\\Python313-x64;',
            workflow,
        )
        self.assertNotIn("-lpython311", before_build)
        self.assertIn("-lpython$pythonAbi", before_build)

    def test_windows_installer_carries_the_verified_runtime_provenance(self):
        installer = INSTALLER.read_text(encoding="utf-8")
        self.assertIn('File "GoldenCheetah.windows-provenance.json"', installer)
        self.assertIn('File "libcrypto-3.dll"', installer)
        self.assertIn('File "libssl-3.dll"', installer)
        self.assertIn('File "tls\\qschannelbackend.dll"', installer)
        self.assertNotIn("qopensslbackend.dll", installer)
        for current in (
            f"python{PYTHON_ABI}.dll",
            f"python{PYTHON_ABI}._pth",
            f"python{PYTHON_ABI}.zip",
            "_uuid.pyd",
            "_wmi.pyd",
            "_zoneinfo.pyd",
            "python.cat",
            "vcruntime140.dll",
            "vcruntime140_1.dll",
        ):
            self.assertIn(f'File "{current}"', installer)
        for legacy in (
            "libcrypto-1_1-x64.dll",
            "libssl-1_1-x64.dll",
            "OpenSSL License.txt",
            "python311.dll",
            "python311._pth",
            "python311.zip",
            "_msi.pyd",
        ):
            self.assertNotIn(f'File "{legacy}"', installer)
            self.assertGreaterEqual(
                installer.count(f'Delete "$INSTDIR\\{legacy}"'),
                2,
                f"upgrade and uninstall must both remove {legacy}",
            )
        self.assertIn(
            'Delete "$INSTDIR\\GoldenCheetah.windows-provenance.json"',
            installer,
        )

    def test_built_nsis_artifact_is_silently_installed_and_smoke_tested(self):
        packager = PACKAGER.read_text(encoding="utf-8")
        self.assertIn("function Test-WindowsInstallerPayload", packager)
        self.assertIn("[IO.Path]::GetTempPath()", packager)
        self.assertIn("'/S'", packager)
        self.assertIn('"/D=$installRoot"', packager)
        self.assertIn(
            "Join-Path $installRoot 'GoldenCheetah.exe'", packager
        )
        self.assertIn("'--version'", packager)
        self.assertIn("'--goldencheetah-build-provenance'", packager)
        self.assertIn("Join-Path $installRoot 'python.exe'", packager)
        self.assertIn("import ssl, sys", packager)
        self.assertIn(
            "Assert-WindowsRuntimeProvenance `\n"
            "      -Destination $installRoot",
            packager,
        )
        installer_build = packager.index("makensis.exe")
        installer_smoke = packager.index(
            "Test-WindowsInstallerPayload", installer_build
        )
        self.assertLess(installer_build, installer_smoke)


if __name__ == "__main__":
    unittest.main()
