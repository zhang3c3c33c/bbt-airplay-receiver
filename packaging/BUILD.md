# Building BigBirdTouch AirPlay Receiver

The receiver is a separate GPL process. It must not be linked into, packed
with, or integrity-gated by `sender.exe`.

## Prerequisites

- Windows 10/11 x64
- MSYS2 UCRT64 packages: GCC, Ninja, OpenSSL, GLib2, libplist, gettext-runtime,
  libiconv and PCRE2
- CMake 3.13 or newer
- PowerShell 5.1 or newer

## Rebuild and stage

From the corresponding-source directory:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_airplay_receiver.ps1 `
  -Configuration Release `
  -OutDir .\dist\airplay_receiver `
  -ForkSourceURL https://github.com/zhang3c3c33c/bbt-airplay-receiver/tree/v0.1.0
```

The command validates the pinned UxPlay revision, rebuilds from the current
source tree, checks `--version-json` and `--self-test-json`, rejects any
GStreamer runtime, and creates the executable, runtime DLLs, license texts,
source archive, build instructions and source manifest.

## Replacement

Stop BigBirdTouch Sender, replace `bbt_airplay_receiver.exe` and the complete
`airplay_runtime` directory with a protocol-major-1 compatible rebuild, then
restart Sender. Sender does not enforce a binary hash whitelist for this GPL
component.
