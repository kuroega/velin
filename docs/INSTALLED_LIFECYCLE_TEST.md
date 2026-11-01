# Installed lifecycle regression

Run `scripts/Test-InstalledLifecycle.ps1` only inside a disposable Windows VM,
Windows Sandbox, or disposable Windows account. The installer changes HKCU
registrations and Start menu shortcuts, so an alternate installation directory
alone is not isolation. Take a snapshot or discard the entire test environment
afterward. The script refuses to run without `-DisposableEnvironment` and checks
for existing Velin state, but those checks cannot prove an environment is disposable.

Copy the installer and this script into the isolated environment, then run:

```powershell
./Test-InstalledLifecycle.ps1 -Installer 'C:\fixtures\Velin-current-setup.exe' `
    -PreviousInstaller 'C:\fixtures\Velin-previous-setup.exe' -DisposableEnvironment
```

The previous installer is optional. Without it the script tests reinstalling
the same package, not upgrading across versions. The script retains the fixture
directories, user data marker and any shortcuts preserved by ownership rules for
inspection. Its output includes the fixture path and both CLI version strings.
Record the Windows version, package hashes, output and whether the two packages
are different before using the result as release evidence.

The script exercises silent core installation into a Unicode path, CLI startup,
same-directory upgrade or reinstall, registration ownership across two install
directories, uninstall, and preservation of user data and unrelated files. It
does not select optional installer components or test GUI first launch, Open with
and protocol registration, a running player, update rollback, WebView2 runtime
installation, interactive cancellation, or a clean-machine package launch. Those
remain separate acceptance tests. A script parse or refusal check on the host is
not evidence that the installed lifecycle passed.
