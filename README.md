# MoLink

A lightweight ADB-based TCP tunneling toolkit for Windows and Android, communicating entirely over USB without external dependencies.

## Overview

MoLink provides a transparent TCP bridge between a Windows host and an Android device via USB cable. It implements the ADB protocol from scratch using libusb and BCrypt, requiring no adb installation, no root access, and no third-party runtime dependencies.

The typical topology is a SOCKS5 proxy on the Android side, reachable from the Windows host through a local port forward—usable by any application that speaks SOCKS5.

## Architecture

```
Windows (molink.exe) ──USB── Android (molink-worker.apk)
       │                          │
  localhost:1080              SOCKS5 :1081
  (port forward)              (proxy service)
```

## Quick Start

### Android Side

Install `molink-worker.apk`. Launch the app and toggle the switch to start the proxy service on port 1081.

### Windows Side

```powershell
# First run: accept the RSA key prompt on the device
.\molink.exe auth

# Start background daemon and port forwarding
.\molink.exe start
.\molink.exe forward -p 1080 -r 1081

# Configure any SOCKS5-capable client to 127.0.0.1:1080 (no auth)

# Check status / stop
.\molink.exe status
.\molink.exe stop
```

### File Transfer

```powershell
.\molink.exe push .\file.txt /sdcard/
.\molink.exe pull /sdcard/file.txt .\
.\molink.exe ls /sdcard/
```

### APK Install

```powershell
.\molink.exe install -r .\app-release.apk
```

## Build

```powershell
.\build.ps1
```

Output: `build/molink.exe`, `build/molink-worker.apk`, `build/version.txt`.

Prerequisites: Windows 10+, MinGW-w64 + CMake 3.14+ (PC side), Android Studio + Gradle (Android side).

## Verified Devices

| Device | Model | OS | Status |
|--------|-------|----|:---:|
| Samsung | SM-S9110 | Android 14 | OK |
| Meizu | M1852 | Flyme OS | OK |

Android 8.1+ should work in general.

## Technical Notes

- **Self-contained ADB stack**: libusb + BCrypt RSA, no adb dependency
- **Static linking**: single exe, zero DLLs, copy-and-run
- **Concurrent forwarding**: up to 16 relay channels, non-blocking bidirectional drain
- **Auto-reconnect**: device re-attach without re-authorization
- **Unified versioning**: single build script produces matched exe + apk version pair

## FAQ

**Device not detected?**

The ADB interface requires a WinUSB driver. Use [Zadig](https://zadig.akeo.ie/) to replace the default driver.

**Re-authorization on every reconnect?**

This should not happen in normal use. If it does, upgrade to the latest build and run `molink auth` once.

**Multiple PCs sharing one device?**

USB is exclusive. Multiple apps on the same PC are supported (up to 16 concurrent connections).

## License

MIT
