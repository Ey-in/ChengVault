# 澄匣备份 · ChengVault

Windows 10/11 x64、NTFS、C++20 与 Qt 6 Widgets 的课程数据备份项目。提供普通备份与恢复、两种打包解包、两种压缩解压、两种密码加密解密、十类筛选、版本仓库、自动备份、独立网络服务器及 RS 冗余纠错。

实现与测试结果以代码和 `evidence` 中的实际执行记录为准。

## 直接运行

解压 `dist/ChengVault-1.0-windows-x64.zip`，双击 `ChengVault.exe`。完整保留同目录 DLL、`plugins` 与 `qt.conf`，无需安装 Qt 开发环境。

| 程序 | 用途 |
|---|---|
| `ChengVault.exe` | 中文图形客户端 |
| `ChengVaultCli.exe` | 可脚本化备份、恢复、校验、仓库和网络操作 |
| `ChengVaultAgent.exe` | 用户会话内的定时及实时后台进程 |
| `ChengVaultServer.exe` | 本机或局域网独立密文存储服务器 |

首次使用见 [用户手册](docs/user-manual.md)。网络账户与设备配置见 [网络说明](docs/network.md)。特殊文件权限条件见 [NTFS 支持说明](docs/ntfs-support.md)。

客户端为八页 Qt Widgets 界面；界面回归与高 DPI 截图见 `evidence/ui-refresh`，复现入口为 `scripts/validate-ui.ps1`。

## 功能与独立实现

| 项目 | 实现 |
|---|---|
| 备份 / 恢复 | 空目录、空文件、多源、目录树、选择恢复、校验、取消与安全覆盖 |
| 特殊文件 | 符号链接、硬链接、Junction、稀疏文件、文件及目录 ADS |
| 元数据 | 属主/组 SID、三类时间、属性、DACL 与继承状态 |
| 六类必需筛选 | 路径、对象类型/扩展名、名字、时间、尺寸、文件属主 |
| 四类额外筛选 | 属性、深度、UTF-8 文本包含、相对清单的元数据变化 |
| 两种打包 | 自主索引归档、PAX/TAR；均生成完整单文件归档 |
| 两种压缩 | 自主 Huffman、LZ77，以及原样块回退 |
| 两种加密 | 自主 AES-256-CTR、ChaCha20，配合 HMAC-SHA256；自主 SHA-256、PBKDF2、HKDF |
| 增量仓库 | 1 MiB 原始块的仓库专属 HMAC 标识、完整快照清单、共享块保留、密码重包 |
| 自动备份 | 间隔/每天/每周/实时、任务持久化、后台执行、版本淘汰 |
| 网络 | 账户、设备授权、配额、元数据目录、认证加密传输、块增量与断点续传 |
| 新增扩展 | 自主 GF(256) RS(12,10)，每组修复最多两个已定位损坏分片 |

Qt/Win32 提供界面、JSON、文件访问、线程、套接字、文件事件、随机源和本机凭据存储。核心算法不调用第三方归档、压缩、密码或纠错库。Qt 自身依赖的 zlib、zstd 等 DLL 属于界面/网络基础设施依赖，运行时不调用来完成备份压缩。部署包附实际 DLL 导入关系与原始许可证，详见 [依赖声明](docs/third-party-notices.md)。

## 从源码构建

使用同一套 MSYS2 UCRT64 的 GCC、Qt、CMake 与 Ninja，避免把其他发行版 Qt 与编译器混用。首次安装环境：

```powershell
.\scripts\bootstrap.ps1 -MsysRoot D:\msys2
.\scripts\build.ps1 -Configuration Release -Toolchain D:\msys2\ucrt64
.\scripts\test.ps1 -Configuration Release -Toolchain D:\msys2\ucrt64
```

本工作区已经准备的隔离工具链位于 `.toolchain/root/ucrt64`，构建脚本默认检测该位置。它不属于源码交付包；新机器按上述脚本安装依赖即可重建。

```powershell
.\scripts\build.ps1
.\scripts\test.ps1
.\scripts\network-smoke.ps1
.\scripts\benchmark.ps1
.\scripts\package.ps1 -SourceBundle
.\scripts\deployment-smoke.ps1
```

`benchmark.ps1` 默认创建真实 1 GiB 输入、5000 个小文件和逻辑长度超过 4 GiB 的 NTFS 稀疏文件，保留数据与日志供复验。`network-smoke.ps1` 启动独立服务器进程并验证两个账户，结束后清理本次生成的临时明文凭据。

`package.ps1` 收集四个程序、Qt 插件、PE 递归 DLL 依赖、文档和许可证。`-SourceBundle` 额外从当前 Git 提交生成源码 ZIP 与历史 bundle；先提交最终代码，避免源码包与运行包版本不同。

运行 `scripts/verify-package.ps1 -SourceBundle` 可核对运行目录与 ZIP 的全部摘要、四个程序与部署实测证据、源码 ZIP 与当前 Git 提交的每个文件字节，以及历史 bundle。它将结果写入 `dist/package-verification.json`，遇到版本或内容不一致会失败。运行包内置的 `verification/external-acceptance.ps1` 用于独立 Windows 和真实局域网复验，操作见 [外部验收说明](docs/external-acceptance.md)。

`deployment-smoke.ps1` 将子进程 PATH 限定为 Windows 系统目录，验证实际发布 GUI、CLI、Server，以及使用相同运行依赖的 GUI 自动化和独立 Agent。当前本机验证全部通过；独立 Windows 机器、符号链接和双机局域网复验均已完成。

## 实测结果

2026-10-25 最终自动测试为核心 591、GUI 20、后台 5，合计 616 通过、0 失败、0 跳过；行覆盖 3694/3786=97.6%，原始 GCC 分支 8184/14446=56.7%，辅助 86.5%，判定 75.6%。两台真实 Windows 主机的局域网验收为 PASS_TWO_MACHINE_LAN；独立 Windows 机器和符号链接验收均完成。

性能测试覆盖 1 GiB 数据、5000 个小文件和逻辑长度超过 4 GiB 的稀疏文件。1 GiB 数据集包含重复块，存储比例同时反映去重和压缩，不代表普通文件的通用压缩率。测试记录见 `evidence/test-results.json`、`evidence/coverage/summary.json`；性能记录见 `evidence/benchmark-results.json`、`evidence/network-smoke-results.json`；局域网记录见 `evidence/lan/20261025-two-machine/`。

## 目录结构

```text
include/chengvault/     公共接口
src/core/              自主算法、仓库、网络、调度
src/platform/          NTFS 捕获与恢复
src/gui/               Qt Widgets 界面
src/apps/              四个程序入口
tests/                 原创测试及测试框架
scripts/               环境、构建、测试、测量与打包
docs/                  格式、设计、手册、UML 与小组任务材料
evidence/              实际执行结果
dist/                  便携运行包、源码包及 Git 历史
```

仅支持 Windows 10/11 x64 与 NTFS；Linux FIFO 和设备节点不映射，符号链接需要开发者模式或权限，恢复是文件级安全替换而不是全目录事务。详见 [NTFS 支持说明](docs/ntfs-support.md)。
