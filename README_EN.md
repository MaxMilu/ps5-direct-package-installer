# PS5 Direct Package Installer (singleDPI)

[中文](README.md) | **English**

A lightweight, standalone, single-purpose PS5 Direct Package Installer payload. It accepts DPI v1
JSON remote PKG installation requests over TCP port 9090 and also listens on the experimental port
12800 DPI v2 URL entrypoint. It does not load etaHEN or include kstuff.

Tested on PS5 firmware 5.50 with compatible kstuff/kstuff-lite for installing and launching
PS4 FPKGs.

Current version: `0.1.0`

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

The payload reads the PS5 system language before displaying notifications. English example:

```text
singleDPI 0.1.0 - Ready
Direct Package Installer
TCP port: 9090
```

If it displays `Not ready`, check the reported kstuff, Debug AuthID, or AppInst problem. Port
9090 remains available so the client can request detailed status with `ping`; when the 12800
listener is created successfully, it also accepts experimental DPI v2 URL requests.

### Client configuration and installation

1. Open the configuration page in PS4 Remote PKG Sender.
2. Select `PS5 singleDPI` as the target application.
3. Enter the PS5 IP address and choose `Auto`, `DPI v1`, or `DPI v2 URL` in `singleDPI Mode`.
   `Auto`/`DPI v1` shows port 9090, while `DPI v2 URL` shows port 12800. Internally, the client
   still uses the 9090 control API for `ping`, status, and installed checks.
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
- HTTP PNG icon caching on the PS5, served locally to AppInst on port 9091.
- Download, copy, Promote progress, and AppInst error queries.
- Installed-record detection for Base, Patch, and DLC packages.
- Icon cache inspection and manual cache clearing.
- Detection of the required RWX capability provided by kstuff.
- Debug AuthID transition and verification before AppInst initialization.
- PS5 notifications for service state, title, Content ID, and error code.
- Notifications follow the PS5 system language, with Simplified Chinese, Traditional Chinese,
  and an English fallback.
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
  "version": "0.1.0",
  "notification_language": "en",
  "ready": true,
  "message": "ready to install packages",
  "kstuff_available": true,
  "authid_available": true,
  "original_authid": "0x4800000000010003",
  "current_authid": "0x4800000000000006",
  "appinst_available": true,
  "icon_cache_size_bytes": 245760
}
```

Before installing, confirm that `ready` and all three `*_available` fields are `true`.

### `install`

Only `url` is required:

- `url`: PKG HTTP URL reachable from the PS5.
- `content_name`: title shown by the download and installation UI.
- `content_id`: used to display and identify the task; it should match the PKG.
- `icon_url`: PNG URL reachable from the PS5. HTTP PNGs are cached under
  `/data/singleDPI/icons` and served to AppInst from `http://127.0.0.1:9091`; the original URL is
  retained when caching fails.
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

### `is_installed`

Check PS5 installation records using the SFO `CATEGORY`, `TITLE_ID`, and `CONTENT_ID`:

```json
{
  "action": "is_installed",
  "title_id": "CUSA32184",
  "content_id": "EP3643-CUSA32184_00-CULTISTPAC000000",
  "category": "ac"
}
```

Path rules:

- `gd`, `gda`, `la`: `/user/app/<TITLE_ID>/app.json`
- `gp`: `/user/patch/<TITLE_ID>/patch.json`
- `ac`: `/user/addcont/<TITLE_ID>/<last 16 characters of CONTENT_ID>/ac.json`

The `exists` response field reports whether the target JSON exists. Diagnostic fields include
`user_root_visible`, `category_root_visible`, `title_root_visible`, `parent_visible`, `errno`, and
`error_description`. This API checks record paths only; it does not compare installed versions or
PKG digests.

### `cache_info` and `cache_clear`

```json
{"action":"cache_info"}
```

Returns the cache directory, file list, file count, and total size. Clear the cache with:

```json
{"action":"cache_clear"}
```

`cache_clear` returns the number of bytes freed and only removes files matching singleDPI's icon
cache naming convention.

### Error codes

| `res` | Meaning |
| ---: | --- |
| `-1` | Required kstuff capability was not detected |
| `-2` | AppInst Debug AuthID could not be changed or verified |
| `-3` | AppInst initialization is unavailable |
| `-4` | Installation URL is missing |
| `-5` | Content ID is missing from a status query |
| `-6` | Title ID is missing from an installation check |
| `-7` | CATEGORY is missing from an installation check |
| `-8` | Invalid installation-check parameters or invalid DLC Content ID |
| `-9` | Unsupported CATEGORY |
| `-10` | Invalid JSON |
| `-11` | Unknown action |

Non-zero errors returned by AppInst itself are passed through in `res`.

## PS5 notifications

singleDPI only sends notifications at important transitions:

- Successful startup: version, Ready state, and TCP port.
- Service not ready: missing kstuff capability, Debug AuthID, or AppInst state.
- Installation started: client-provided title and the Content ID returned by AppInst.
- Installation call failed: title and hexadecimal AppInst error code.

The notification language is read through `sceSystemServiceParamGetInt`. Simplified Chinese uses
`zh-Hans`, Traditional Chinese uses `zh-Hant`, and every other language or a read failure falls
back to English (`en`). The selected value is returned as `notification_language` by `ping`.
API field names and error strings remain English for client compatibility.

`content_name` and `icon_url` are used by the system download and installation UI. singleDPI's
own notifications are text-only and do not use the remote `icon_url` as their notification icon.

## Higher-firmware compatibility status and planned path

The current release has only been tested on a physical PS5 running firmware 5.50. A user reported
that on 11.60, `ping` reports Ready and the client marks the PS5 as reachable, but installation
returns `-2135813777`. This converts to `0x80B2116F`,
`SCE_PLAYGO_ERROR_CORE_INVALID_SLOT`. The same error has also been reported with etaHEN DPI v1 on
other firmware versions, so it cannot currently be attributed uniquely to singleDPI.

Ready/the green client indicator only confirms the port 9090 service, the RWX capability probe,
Debug AuthID, and AppInst initialization. It does not prove that the complete AppInst/PlayGo
installation path is compatible with the running firmware. The maintainer only has firmware 5.50
and cannot reproduce the 11.60 failure locally, so the project does not currently claim 11.60
support.

singleDPI now includes an experimental etaHEN DPI v2 URL-compatible entrypoint:
HTTP `POST http://PS5_IP:12800/` with the `url` form field and optional `content_name`,
`content_id`, `playgo_scenario_id`, `ex_uri`, and `icon_url` fields. This mode still calls
`sceAppInstUtilInstallByPackage` and still uses a remote URL, so it must not be assumed to fix the
11.60 `INVALID_SLOT` failure by itself. The more relevant difference remains file-upload mode: it
first stores the complete PKG in a temporary file on the PS5 and then starts installation from that
local path. The planned investigation and implementation path is:

The companion PS4 Remote PKG Sender provides a `singleDPI Mode` selector when `PS5 singleDPI` is
selected: `Auto`/`DPI v1` shows port 9090, and `DPI v2 URL` shows port 12800. Before installing,
it always calls 9090 `ping` to read `firmware_version` and `dpi_v2_url_available`. Higher firmware
versions in `Auto` prefer the 12800 DPI v2 URL path, while lower firmware versions prefer the 9090
DPI v1 path. Manually selecting v2 uses the 12800 URL entrypoint first and then falls back to v1 on
failure; the sender only reports an error to the user after both available modes fail.

1. On firmware 11.60, test etaHEN DPI v2 URL installation and file-upload installation separately
   to determine whether only the local-file path avoids `INVALID_SLOT`.
2. If upload mode works, add a minimal PKG upload, local staging, installation, and temporary-file
   cleanup flow to singleDPI. Reproducing the complete DPI v2 WebUI is not required.
3. Add upload-mode support to the companion client. Existing port 9090 clients only send a PKG URL
   and will not automatically use the new path.
4. Keep the existing DPI v1 URL API for verified firmware and for users who do not want to stage a
   complete PKG on the PS5.
5. Improve diagnostics with the installation stage, decimal and hexadecimal error codes, firmware,
   and request mode, distinguishing connectivity, immediate AppInst-call, and asynchronous
   installation failures.

Local uploads require temporary storage; large packages may consume close to one additional full
PKG during installation. The upload endpoint also needs file-size, free-space, failure-cleanup, and
concurrency limits and must remain restricted to a trusted LAN. If etaHEN DPI v2 upload mode returns
the same error on 11.60, the likely cause moves back to AppInst, the PKG, or firmware-specific
kstuff, and adding DPI v2/upload support alone will not solve it.

## Limitations and security

- The port 9090 TCP API and experimental port 12800 DPI v2 URL entrypoint are implemented; there is
  no DPI v2 WebUI or PKG upload endpoint.
- The service has no authentication, access control, or encryption and listens on all interfaces.
  Use it only on a trusted LAN.
- Metadata is supplied by the client. singleDPI does not parse the remote PKG to validate its title,
  Content ID, or icon.
- Icon caching actively downloads HTTP PNGs only, with an 8 MiB per-file limit. HTTPS URLs and
  validation failures fall back to the original URL.
- There is currently no automatic total cache limit or eviction policy. Use `cache_info` and
  `cache_clear` for manual management.
- The local icon server listens on `127.0.0.1:9091` only while singleDPI is running.
- `is_installed` depends on the currently verified `/user/app`, `/user/patch`, and `/user/addcont`
  record layout.
- The service handles one connection at a time and only remembers the latest installation task.
- AppInst states such as `playable` do not mean a game needs no Backport or is guaranteed to launch.
- Capability detection does not replace firmware matching. Incorrect kstuff versions can still
  cause installation or launch failures.

## Project name

`singleDPI` remains the ELF filename and API `service` identifier for compatibility with existing
clients. The repository and public project name are **PS5 Direct Package Installer**, with the full
description **PS5 Direct Package Installer (singleDPI)**.
