# PS5 Direct Package Installer (singleDPI)

**中文** | [English](README_EN.md)

一个独立、轻量、单一用途的 PS5 Direct Package Installer payload。它通过 TCP 9090
接收远程 PKG 安装请求，不加载 etaHEN，也不包含 kstuff。

已在 PS5 5.50 上配合兼容的 kstuff/kstuff-lite 完成 PS4 FPKG 安装和启动验证。

当前版本：`0.1.0`

## 项目来源（请先阅读）

本项目基于 [etaHEN](https://github.com/etaHEN/etaHEN) 的 Direct Package Installer
实现及 AppInst 结构定义进行独立化和修改。原始代码版权声明为：

```text
Copyright (C) 2025 etaHEN / LightningMods
```

2026 年由 MaxMilu 完成独立 payload、TCP 协议扩展、运行能力检查、状态接口和文档等修改。
本项目是非官方派生项目，不是 etaHEN 官方版本；本项目自身问题不应归因于 etaHEN。

## 许可证

整个派生项目依照 **GNU GPL v3 或任何后续版本**发布。根目录 [LICENSE](LICENSE)
包含与 etaHEN 相同版本的完整 GPL v3 文本，[NOTICE](NOTICE) 记录来源、修改和版权归属。
发布 ELF 等二进制时，必须同时按照 GPL 要求提供对应源代码和许可证信息。

`third_party/tiny-json` 是独立的 MIT 许可组件，其原始版权和许可保留在
[`third_party/tiny-json/LICENSE`](third_party/tiny-json/LICENSE)。

## AI 辅助开发说明

本项目的部分代码分析、重构、问题排查和文档编写使用了 AI 辅助编程工具，包括
OpenAI Codex。AI 工具仅作为开发辅助，不是本项目的作者或版权主体。所有合入的修改均由
项目维护者审查，并结合编译或真机测试进行验证；项目维护者对最终代码、发布内容和维护
责任负责。

## 客户端与使用说明

### 作者的话
- 其他内容都是ai自动整理的，这是我自己写的部分，可能忘记了一部分，仅供参考
- 根据 etaHEN 分离的 DPI 安装api系列功能
- 使用 readme 中的 改版 [ps4-remote-pkg-sender](https://github.com/MaxMilu/ps4-remote-pkg-sender) 选择 singleDPI 即可使用
- 载入顺序需要在 kstuff / kstuff lite 之后
- 只在实体机 5.5 系统上测试通过，更高版本的请使用对应系统版本的 kstuff 配合 singleDPI 自行测试
- 由于整体代码是参考etaHEN的安装部分解析出来的，理论上使用etaHEN的pkg安装功能可以安装的系统版本都应该支持
- 使用了极简的本地http服务器来缓存安装列表的icon资源，防止重启机器或者不使用payload的时候icon丢失的问题（没有源码参考只是通过现象参考推测）
- icon缓存可以在 /data/singleDPI 中使用ftp或者任意文件管理器进行管理清理，目前没有自动清理功能。

### 准备内容

- 与 PS5 系统版本匹配的 exploit 和 ELF Loader。
- 与系统版本匹配的 kstuff、kstuff-lite 或兼容实现。
- 本项目编译生成的 `bin/singleDPI.elf`。
- 支持 singleDPI 的
  [PS4 Remote PKG Sender](https://github.com/MaxMilu/ps4-remote-pkg-sender/tree/codex/singledpi-support)。
- PS5 和电脑位于同一可信局域网，并且 PS5 可以访问电脑提供的 PKG URL。

### PS5 加载顺序

严格按照以下顺序加载：

```text
Exploit / ELF Loader
  -> kstuff 或 kstuff-lite
  -> singleDPI.elf
  -> 电脑端发送安装请求
```

成功后会读取 PS5 系统语言并显示对应通知。简体中文示例：

```text
singleDPI 0.1.0 - 已就绪
远程 PKG 安装服务
TCP 端口：9090
```

如果显示 `Not ready`，请根据通知检查 kstuff、Debug AuthID 或 AppInst。服务仍会保留
9090 端口，客户端可以通过 `ping` 查看详细状态。

### 客户端配置与安装

1. 打开 PS4 Remote PKG Sender 的配置页面。
2. 目标程序选择 `PS5 singleDPI`。
3. 填写 PS5 IP 地址，端口使用 `9090`。
4. 选择电脑的网络接口和 PKG 目录，然后启动内置 HTTP 服务。
5. 将 PKG 加入队列并启动自动安装，或直接发送单个条目。
6. 队列可以选择上一项完成后立即继续，或等待自定义秒数后安装下一项。
7. 在客户端查看下载、复制和 Promote 进度；PS5 通知会显示标题和 Content ID。

游戏和更新包仍须满足当前 PS5 中 PS4 兼容环境的版本要求。较新的 PS4 FPKG 可能需要
正确匹配 Base、更新版本和 Title ID 的 Backport/降级补丁，否则可能安装成功但无法启动。

### 不使用客户端时手动调用

检查服务：

```json
{"action":"ping"}
```

开始安装：

```json
{
  "action": "install",
  "url": "http://192.168.1.10/game.pkg",
  "content_name": "游戏标题",
  "content_id": "UP0000-CUSA00000_00-XXXXXXXXXXXXXXXX",
  "icon_url": "http://192.168.1.10/icon0.png"
}
```

查询进度：

```json
{"action":"status","content_id":"UP0000-CUSA00000_00-XXXXXXXXXXXXXXXX"}
```

## 编译

推荐在 Windows 11 上使用 WSL2 Ubuntu，并将 PS5 Payload SDK 安装到
`/opt/ps5-payload-sdk`：

```bash
rm -rf "$HOME/ps5-direct-package-installer"
cp -a /mnt/d/Dev/git/singleDPI "$HOME/ps5-direct-package-installer"
cd "$HOME/ps5-direct-package-installer"
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make clean
make -j"$(nproc)"
```

输出文件：

```text
bin/singleDPI.elf
```

复制回 Windows：

```bash
cp bin/singleDPI.elf /mnt/d/Dev/git/singleDPI/bin/
```

Makefile 使用 SDK 自带的 `SceNet`、`SceSystemService`、`SceAppInstUtil` 和
`kernel_sys` stub 库，不需要 etaHEN 源码。

## 功能

- TCP 9090 JSON API。
- 兼容 etaHEN 的简单 `{ "url": "..." }` 请求。
- 支持标题、Content ID、图标、PlayGo 和扩展 URI 等安装元数据。
- 查询下载、复制、Promote 进度和 AppInst 错误。
- 检测 kstuff 提供的必要 RWX 能力。
- 初始化 AppInst 前切换并验证 Debug AuthID。
- PS5 通知显示服务状态、安装标题、Content ID 和错误码。
- PS5 通知自动跟随系统语言，支持简体中文、繁体中文和英文回退。
- 不依赖 etaHEN 的源码、运行进程或 stub 库。

## API 说明

每个请求使用一个 TCP 连接：连接 PS5 的 9090 端口，发送一个完整 JSON 文档，读取一个
JSON 响应，然后由服务端关闭连接。单个请求不能超过 16 KiB。

### `ping`

```json
{
  "res": 0,
  "service": "singleDPI",
  "version": "0.1.0",
  "notification_language": "zh-Hans",
  "ready": true,
  "message": "ready to install packages",
  "kstuff_available": true,
  "authid_available": true,
  "original_authid": "0x4800000000010003",
  "current_authid": "0x4800000000000006",
  "appinst_available": true
}
```

开始安装前应确认 `ready` 以及三个 `*_available` 字段均为 `true`。

### `install`

只有 `url` 为必填字段：

- `url`：PS5 可访问的 PKG HTTP URL。
- `content_name`：下载和安装界面显示的标题。
- `content_id`：显示和识别任务使用，应与 PKG 内容一致。
- `icon_url`：PS5 可访问的 PNG 图标 URL，安装期间需要保持可用。
- `playgo_scenario_id`、`ex_uri`：可选 AppInst 参数。

成功响应：

```json
{
  "res": 0,
  "content_id": "UP0000-CUSA00000_00-XXXXXXXXXXXXXXXX",
  "title": "游戏标题",
  "message": "installation task started"
}
```

不提供 `action` 时按 `install` 处理，以兼容 etaHEN DPI：

```json
{"url":"http://192.168.1.10/game.pkg"}
```

### `status`

不提供 `content_id` 时查询最近一次任务：

```json
{"action":"status"}
```

响应包含 `status`、`downloaded_size`、`total_size`、`progress`、
`promote_progress`、`remain_time`、`local_copy_percent` 和 AppInst 错误字段。
系统 AppInst 服务独立执行安装，singleDPI 只保存最近一次任务的 Content ID。

### 错误码

| `res` | 含义 |
| ---: | --- |
| `-1` | 未检测到 kstuff 所需能力 |
| `-2` | 无法切换或验证 AppInst Debug AuthID |
| `-3` | AppInst 初始化不可用 |
| `-4` | 缺少安装 URL |
| `-5` | 查询状态时缺少 Content ID |
| `-10` | JSON 无效 |
| `-11` | 未知 action |

AppInst 系统调用自身返回的非零错误码会直接放入 `res`。

## PS5 通知

singleDPI 只在关键节点发送通知：

- 启动成功：版本、Ready 状态和 TCP 端口。
- 服务未就绪：缺少的 kstuff 能力、Debug AuthID 或 AppInst 状态。
- 安装开始：客户端提供的标题和 AppInst 返回的 Content ID。
- 安装调用失败：标题和十六进制 AppInst 错误码。

通知语言通过 `sceSystemServiceParamGetInt` 读取：简体中文系统使用 `zh-Hans`，繁体中文
系统使用 `zh-Hant`，其他语言或读取失败时使用英文 `en`。当前选择也会由 `ping` 的
`notification_language` 字段返回。API 字段名和错误字符串保持英文，以保证客户端兼容。

`content_name` 和 `icon_url` 由系统下载/安装界面使用。singleDPI 自身通知只显示文字，
不会将远程 `icon_url` 设置成通知图标。

## 限制与安全

- 目前只有 9090 TCP API，不包含 DPI v2 WebUI 或 PKG 文件上传。
- 服务没有身份认证、访问控制或加密，并监听所有网络接口；只应在可信局域网使用。
- 元数据由客户端提供，singleDPI 不解析远程 PKG 来校验标题、Content ID 或图标。
- 服务一次处理一个连接，只记录最近一次安装任务。
- `playable` 等 AppInst 状态不代表游戏无需 Backport 或一定能够启动。
- 能力检测不能替代固件匹配检查，错误版本的 kstuff 仍可能导致安装或启动失败。

## 项目名称

`singleDPI` 保留为 ELF 文件名和 API `service` 标识，以兼容现有客户端。仓库和对外名称使用
**PS5 Direct Package Installer**，完整描述为 **PS5 Direct Package Installer (singleDPI)**。
