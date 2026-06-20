# PS5 Direct Package Installer (singleDPI)

[中文](README.md) | **English**

A lightweight, standalone, single-purpose PS5 Direct Package Installer payload. It accepts
remote PKG installation requests over TCP port 9090 and does not load etaHEN or include kstuff.

Tested on PS5 firmware 5.50 with compatible kstuff/kstuff-lite for installing and launching
PS4 FPKGs.

## Project origin (read first)

This project is an independently packaged and modified derivative of the Direct Package Installer
implementation and AppInst structure definitions from
[etaHEN](https://github.com/etaHEN/etaHEN). The original code carries this notice:

```text
Copyright (C) 2025 etaHEN / LightningMods
```

The standalone payload, TCP protocol extensions, runtime capability checks, status API, and
documentation were modified by MaxMilu in 2026. This is an unofficial derivative project, not an
official etaHEN release. Problems specific to this project should not be attributed to etaHEN.

## License

The derivative work is released under the **GNU GPL version 3 or any later version**. The root
[LICENSE](LICENSE) contains the complete GPL v3 text matching etaHEN's license version, while
[NOTICE](NOTICE) records the origin, modifications, and copyright attribution. Distribution of an
ELF or other binary must include access to corresponding source and license information as required
by the GPL.

`third_party/tiny-json` is a separate MIT-licensed component. Its original copyright and license
are preserved in [`third_party/tiny-json/LICENSE`](third_party/tiny-json/LICENSE).

## AI-assisted development disclosure

AI-assisted coding tools, including OpenAI Codex, were used for parts of the code analysis,
refactoring, debugging, and documentation. These tools served only as development assistance and
are not authors or copyright holders of this project. Every merged change is reviewed by the
project maintainer and verified through compilation or console testing where applicable. The
project maintainer remains responsible for the final code, releases, and maintenance.

## Client and usage

### Requirements

- An exploit and ELF Loader compatible with the PS5 firmware.
- kstuff, kstuff-lite, or a compatible implementation matching the firmware.
- `bin/singleDPI.elf` built from this project.
- The singleDPI-compatible
  [PS4 Remote PKG Sender](https://github.com/MaxMilu/ps4-remote-pkg-sender/tree/codex/singledpi-support).
- The PS5 and computer on the same trusted LAN, with PKG URLs reachable from the PS5.

### PS5 load order

Load components in this exact order:

```text
Exploit / ELF Loader
  -> kstuff or kstuff-lite
  -> singleDPI.elf
  -> send the installation request from the computer
```

The PS5 displays the following notification when ready:

```text
singleDPI v2 - Ready
Direct Package Installer
TCP port: 9090
```

If it displays `Not ready`, check the reported kstuff, Debug AuthID, or AppInst problem. Port
9090 remains available so the client can request detailed status with `ping`.

### Client configuration and installation

1. Open the configuration page in PS4 Remote PKG Sender.
2. Select `PS5 singleDPI` as the target application.
3. Enter the PS5 IP address and use port `9090`.
4. Select the computer network interface and PKG directory, then start the built-in HTTP server.
5. Add PKGs to the queue and start automatic installation, or send a single item directly.
6. Configure the queue to continue immediately or wait a custom number of seconds between items.
7. Monitor download, copy, and Promote progress in the client. PS5 notifications show the title
   and Content ID.

Games and updates must still satisfy the version requirements of the PS4 compatibility environment
on the PS5. Newer PS4 FPKGs may require a Backport patch matching the Base, update version, and
Title ID. Otherwise, installation may succeed while the game still fails to launch.

### Manual calls without the client

Check the service:

```json
{"action":"ping"}
```

Start an installation:

```json
{
  "action": "install",
  "url": "http://192.168.1.10/game.pkg",
  "content_name": "Game title",
  "content_id": "UP0000-CUSA00000_00-XXXXXXXXXXXXXXXX",
  "icon_url": "http://192.168.1.10/icon0.png"
}
```

Query progress:

```json
{"action":"status","content_id":"UP0000-CUSA00000_00-XXXXXXXXXXXXXXXX"}
```

## Build

On Windows 11, use WSL2 Ubuntu with the PS5 Payload SDK installed at
`/opt/ps5-payload-sdk`:

```bash
rm -rf "$HOME/ps5-direct-package-installer"
cp -a /mnt/d/Dev/git/singleDPI "$HOME/ps5-direct-package-installer"
cd "$HOME/ps5-direct-package-installer"
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make clean
make -j"$(nproc)"
```

Output:

```text
bin/singleDPI.elf
```

Copy it back to Windows:

```bash
cp bin/singleDPI.elf /mnt/d/Dev/git/singleDPI/bin/
```

The Makefile uses the SDK-provided `SceNet`, `SceSystemService`, `SceAppInstUtil`, and
`kernel_sys` stub libraries. The etaHEN source tree is not required.

## Features

- TCP port 9090 JSON API.
- Compatibility with etaHEN's simple `{ "url": "..." }` request.
- Installation metadata including title, Content ID, icon, PlayGo, and extended URI.
- Download, copy, Promote progress, and AppInst error queries.
- Detection of the required RWX capability provided by kstuff.
- Debug AuthID transition and verification before AppInst initialization.
- PS5 notifications for service state, title, Content ID, and error code.
- No runtime or build dependency on etaHEN source, processes, or stub libraries.

## API reference

Each request uses one TCP connection. Connect to port 9090 on the PS5, send one complete JSON
document, read one JSON response, and let the server close the connection. A request cannot exceed
16 KiB.

### `ping`

```json
{
  "res": 0,
  "service": "singleDPI",
  "version": 2,
  "ready": true,
  "message": "ready to install packages",
  "kstuff_available": true,
  "authid_available": true,
  "original_authid": "0x4800000000010003",
  "current_authid": "0x4800000000000006",
  "appinst_available": true
}
```

Before installing, confirm that `ready` and all three `*_available` fields are `true`.

### `install`

Only `url` is required:

- `url`: PKG HTTP URL reachable from the PS5.
- `content_name`: title shown by the download and installation UI.
- `content_id`: used to display and identify the task; it should match the PKG.
- `icon_url`: PNG URL reachable from the PS5 and available throughout installation.
- `playgo_scenario_id`, `ex_uri`: optional AppInst parameters.

Successful response:

```json
{
  "res": 0,
  "content_id": "UP0000-CUSA00000_00-XXXXXXXXXXXXXXXX",
  "title": "Game title",
  "message": "installation task started"
}
```

When `action` is omitted, the request is treated as `install` for etaHEN DPI compatibility:

```json
{"url":"http://192.168.1.10/game.pkg"}
```

### `status`

Omit `content_id` to query the most recent task:

```json
{"action":"status"}
```

The response includes `status`, `downloaded_size`, `total_size`, `progress`,
`promote_progress`, `remain_time`, `local_copy_percent`, and AppInst error fields. The system
AppInst service performs the task independently; singleDPI only remembers the latest Content ID.

### Error codes

| `res` | Meaning |
| ---: | --- |
| `-1` | Required kstuff capability was not detected |
| `-2` | AppInst Debug AuthID could not be changed or verified |
| `-3` | AppInst initialization is unavailable |
| `-4` | Installation URL is missing |
| `-5` | Content ID is missing from a status query |
| `-10` | Invalid JSON |
| `-11` | Unknown action |

Non-zero errors returned by AppInst itself are passed through in `res`.

## PS5 notifications

singleDPI only sends notifications at important transitions:

- Successful startup: version, Ready state, and TCP port.
- Service not ready: missing kstuff capability, Debug AuthID, or AppInst state.
- Installation started: client-provided title and the Content ID returned by AppInst.
- Installation call failed: title and hexadecimal AppInst error code.

`content_name` and `icon_url` are used by the system download and installation UI. singleDPI's
own notifications are text-only and do not use the remote `icon_url` as their notification icon.

## Limitations and security

- Only the port 9090 TCP API is implemented; there is no DPI v2 WebUI or PKG upload endpoint.
- The service has no authentication, access control, or encryption and listens on all interfaces.
  Use it only on a trusted LAN.
- Metadata is supplied by the client. singleDPI does not parse the remote PKG to validate its title,
  Content ID, or icon.
- The service handles one connection at a time and only remembers the latest installation task.
- AppInst states such as `playable` do not mean a game needs no Backport or is guaranteed to launch.
- Capability detection does not replace firmware matching. Incorrect kstuff versions can still
  cause installation or launch failures.

## Project name

`singleDPI` remains the ELF filename and API `service` identifier for compatibility with existing
clients. The repository and public project name are **PS5 Direct Package Installer**, with the full
description **PS5 Direct Package Installer (singleDPI)**.
