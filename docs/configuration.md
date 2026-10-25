# 备份计划与任务配置

本文对应 `BackupPlan::fromJson`、文件系统筛选器和后台任务的实际字段。目录树及清单格式见 [manifest.md](manifest.md)，完整操作流程见 [用户手册](user-manual.md)。配置文件使用 UTF-8 JSON，字段名区分大小写。

## 可用的 BackupPlan 示例

将以下内容保存为 `plan.json`，把来源与输出路径改为本机实际位置。输出不能在来源目录内；多个来源的根名称不能重复。

```json
{
  "sources": ["D:/demo/source"],
  "destination": "D:/demo/backups/demo.cva",
  "format": "indexed",
  "compression": "lz77",
  "cipher": "chacha20",
  "strictMetadata": true,
  "filters": {
    "pathExclude": ["source/build", "source/.git"],
    "nameExclude": ["*.tmp"]
  }
}
```

上述示例备份其余对象，保留空文件、空目录及所需元数据。`source` 是所选目录的末级名称，筛选路径使用清单内的相对路径；例如磁盘上的 `D:/demo/source/src/main.cpp` 对应 `source/src/main.cpp`。

| 计划字段 | 类型、默认值及作用 |
|---|---|
| `sources` | 字符串数组；必需的来源文件或目录。 |
| `destination` | 字符串；独立归档输出文件，或仓库模式中的目录。 |
| `format` | 默认 `indexed`；还支持 `pax` / `tar`，两者表示同一 PAX/TAR 实现。仓库模式不使用内层归档格式。 |
| `compression` | 默认 `lz77`；可选 `huffman`、`none`。块压缩无收益时自动回退原始块。 |
| `cipher` | 默认 `none`；推荐使用规范名称 `aes256` 或 `chacha20`。只有独立归档支持不加密；新仓库遇到 `none` 会选择 ChaCha20，已有仓库沿用其持久化算法。 |
| `password` | 可读取的字符串字段，但不建议写进计划。CLI 可通过下述环境变量或密码文件提供。默认序列化与后台任务保存均省略它。 |
| `strictMetadata` | 默认 `true`。权限、属主或时间等元数据操作失败时报错；`false` 保留元数据警告，仍不忽略数据读取或完整性失败。 |
| `filters` | 可选对象，默认空对象表示不添加筛选条件。 |

CLI 中显式指定的 `--source`、`--output`、`--format`、`--compression`、`--cipher` 覆盖计划中的对应字段。仓库命令必须提供 `--repo`，并以它作为仓库位置。恢复的覆盖与严格元数据选项由 `--overwrite`、`--lenient-metadata` 控制，不能用备份计划中的字段替代恢复参数。

## 筛选字段

路径、名称、类型、扩展名与属主规则可写字符串数组，也接受单个字符串。一个字段内多条规则采用“或”，不同字段采用“且”；排除优先。通配符匹配不区分大小写，支持 `*`、`?`；当前通配符中的 `*` 可以跨越 `/`。路径规则中的反斜杠会转为 `/`，账户名中的反斜杠不会转换。

| `filters` 字段 | 语义及示例 |
|---|---|
| `pathInclude` / `pathExclude` | 相对路径包含/排除，例如 `["source/src/*"]`。不带通配符的 `source/build` 同时匹配该目录及其后代。 |
| `nameInclude` / `nameExclude` | 仅匹配末级名称，例如 `["*.cpp", "*.hpp"]`，不匹配整个相对路径。 |
| `types` | `file`、`directory`、`symlink`、`junction`、`hardlink`。硬链接筛选也识别尚未转为硬链接条目的多链接普通文件。 |
| `extensions` | 末级扩展名，例如 `["cpp", ".txt"]`；允许有或没有前导点，不是 MIME 类型。 |
| `owners` | Windows 文件属主 SID 或账户名，可用通配符；JSON 示例 `["DOMAIN\\student"]`。与网盘登录用户名无关。 |
| `timeField` | `created`、`modified`（默认）或 `accessed`。 |
| `timeFrom` / `timeTo` | ISO 8601 时间字符串，例如 `"2026-01-01T00:00:00+08:00"`；下界、上界均包含边界。非法时间会报错。 |
| `minSize` / `maxSize` | JSON 整数，逻辑字节数的包含式上下界，不是磁盘实际分配量。 |
| `maxDepth` | JSON 整数；所选根深度为 0，根下一级为 1。 |
| `attributesAll` | 必须同时具备的 Win32 属性位掩码，例如 `3` 要求只读 `1` 和隐藏 `2`。 |
| `attributesNone` | 任何一个指定属性位出现即排除，例如 `2` 排除隐藏对象。 |
| `contentContains` | 非空字符串按 UTF-8 字节精确匹配普通文件主数据流；区分大小写，可跨读取块，查询串最多 1 MiB。不猜测其他文本编码，不搜索 ADS。 |
| `changedSinceManifest` | 之前导出的**完整清单对象**，不是文件名字符串。按相对路径、类型、大小、修改时间及属性判断变化；未见过的路径入选。 |

命中对象所需的祖先目录会被保留，即使这些祖先未满足扩展名或尺寸等包含条件。明确被路径或名字排除的目录会剪去整个子树。预览返回 `selected` 和 `reason`，先预览再执行可避免误解筛选范围。

变化筛选不是内容哈希比较：如果修改内容但刻意保留原大小、时间与属性，它可能不入选。仓库自身的内容块去重独立于该筛选；未启用变化筛选时，扫描到的内容仍逐块计算标识。

## CLI 密码与运行

在发布包目录运行下面的 PowerShell 示例。输入不会回显，示例将密码短暂交给当前进程环境；任务结束后清理该变量。

```powershell
$secret = Read-Host '备份密码' -AsSecureString
$env:CHENGVAULT_PASSWORD = [System.Net.NetworkCredential]::new('', $secret).Password
try {
    .\ChengVaultCli.exe preview --plan .\plan.json
    .\ChengVaultCli.exe backup --plan .\plan.json
    .\ChengVaultCli.exe verify --input D:\demo\backups\demo.cva
} finally {
    Remove-Item Env:\CHENGVAULT_PASSWORD -ErrorAction SilentlyContinue
    $secret.Dispose()
}
```

如使用 `--password-file`，CLI 读取最多 65536 字节的 UTF-8 文本，并去除首尾空白；它优先于 `CHENGVAULT_PASSWORD`。CLI 获得的非空密码会覆盖计划密码。环境变量或密码文件不是加密存储，勿将真实凭据提交到 Git。恢复、检查及仓库命令应同样通过环境变量/密码文件提供密码，不依赖备份计划中的密码字段。

`CHENGVAULT_LOGIN_PASSWORD` 用于网络账户登录；`CHENGVAULT_NEW_PASSWORD` 用于 `repo-change-password` 的新仓库密码。它们与备份密码用途不同。网络设备配对密钥由独立连接配置提供，详见用户手册。

## 自动任务与 DPAPI

通过 GUI“自动任务”创建任务，不手写或复制伪造的 `key` 字段。任务使用版本仓库；保存时调用 `plan.toJson(false)` 去掉明文密码，并把仓库主密钥以当前 Windows 用户 DPAPI 保护后写入 Base64 `key`。Agent 解封此凭据后复用仓库核心，不用 DPAPI 代替 AES/ChaCha20 的备份内容加密。

| 任务配置 | 含义 |
|---|---|
| `name`、`mode` | 显示名；模式为 `interval`、`daily`、`weekly`、`realtime`。 |
| `intervalSeconds` | 间隔秒数，默认 3600，实际最小 1 秒。 |
| `time`、`weekday` | 每日/每周本地时间 `HH:mm`，默认 `02:00`；星期 1–7，1 为周一。 |
| `keepCount`、`keepDays` | 保留版本数默认 10，至少 1；保留天数默认 0，表示不按年龄淘汰。 |
| `id`、`plan`、`key` | 程序生成的任务编号、无密码计划及 DPAPI 凭据。 |
| `enabled`、`state`、`nextRun` | 启停和执行状态；由界面及 Agent 更新，勿把修改显示状态当作任务成功。 |

新任务立即运行一次；以后按计划触发。过期计划启动后补跑一次，失败有上限重试间隔。实时模式合并事件并等待两秒静默后重新扫描，文件占用或持续变化会失败重试。淘汰只在成功备份后执行，至少保留一个版本。

任务位于当前用户应用数据目录的 `jobs` 子目录；`CHENGVAULT_DATA_HOME` 可指定隔离配置目录。DPAPI 凭据与当前用户环境有关，迁移后应重新输入仓库密码创建任务。Agent 是独立用户进程，关闭 GUI 后可继续工作；本项目不承诺未登录状态下的系统服务运行。
