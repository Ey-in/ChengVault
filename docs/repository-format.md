# 增量仓库格式 v1

实现：`include/chengvault/repository.hpp`、`src/core/repository.cpp`。文件系统快照结构见 `manifest.md`；压缩与密文信封由自主算法模块负责。普通归档与多版本仓库共用文件系统捕获和恢复接口。

## 目录布局

```text
repository/
  repository.json
  repository.lock                # 任务运行期间存在
  objects/<前两位>/<64位HMAC>.cvo
  snapshots/<UUID>.cvs
```

`repository.json` 包含 `version`、随机 `id`、`cipher`、`createdUtc`、32 字节随机 `salt` 的十六进制形式、`iterations`、Base64 `wrappedKey` 和十六进制 `proof`。它不包含明文密码、明文主密钥或文件名。

初始化生成独立的 32 字节随机主密钥。用户密码的 UTF-8 字节经 PBKDF2-HMAC-SHA256、随机盐、120000 次迭代得到封装密钥，再以 ChaCha20/HMAC 信封加密主密钥。`proof` 使用主密钥对域分隔标识、仓库 ID 和内容加密算法作 HMAC，将公开配置与解锁后的密钥绑定。

仓库内容加密可选择 `chacha20` 或 `aes256`（AES-256-CTR/HMAC）；接受 `aes256-ctr` 别名。仓库始终加密。未指定内容算法的备份计划使用 ChaCha20；不加密选项用于独立归档。

## 数据块与真正增量

文件系统捕获模块对每个数据流的已分配数据按最多 1 MiB 输出原始块。普通文件数据和 ADS 使用相同接口；稀疏文件未分配区间不产生数据块。

```text
blockId = hex(HMAC-SHA256(masterKey, "chunk\0" || rawBytes))
storedObject = seal(compression.encode(rawBytes), masterKey, selectedCipher)
```

域分隔字符串是 6 个字节，包括末尾 NUL。块 ID 在压缩、随机盐及随机 nonce 之前计算。相同仓库中的相同明文块得到相同 ID，不同随机主密钥得到不同 ID。已有块先读取、解密、解压和校验，再允许新快照复用，避免悄悄引用已损坏的旧对象。

新增块压缩后以独立密文信封保存。每次读块都验证信封认证和内容 HMAC，解压输出限制为 1 MiB。对象不能仅凭文件名或存在性被视为可信。

## 快照与提交

每个 `.cvs` 是完整 JSON 清单的加密信封，包含所有选中文件的目录结构、类型、元数据和块引用，不依赖前一个清单才能恢复。清单公共字段定义于 `manifest.md`，仓库额外加入：

```json
{
  "snapshot": {
    "id": "UUID",
    "createdUtc": "ISO-8601 UTC with milliseconds",
    "compression": "lz77",
    "newChunks": 1,
    "reusedChunks": 20,
    "newBytes": 1024
  }
}
```

示例中的数字仅解释字段，不代表测试结果。`newBytes` 统计本次新写入密文块字节，不包含清单和仓库配置。

写入顺序为：获取仓库锁 → 扫描并写入新块 → 完成文件稳定性检查 → 验证完整清单 → 检查取消标记 → 原子发布加密清单。加密清单是成功提交标记；失败可能留下无引用块，但不会出现半完成的成功快照。

`Repository::list()` 解密清单取得摘要并按时间倒序排列，不维护可能泄露文件名的明文目录索引。

## 恢复、验证与保留策略

`restore(id, destination, options, context)` 持有仓库锁，先验证解密清单，再以按需块读取函数调用文件系统恢复模块。块必须通过认证、解压界限和 HMAC 检查才能提供给恢复模块。特殊文件、路径安全、临时文件安全替换和元数据恢复规则由该模块统一执行。

`verify()` 遍历全部快照引用，对每个不同的块执行真实读取、解密、解压和校验。重复引用的块只验证一次。

`prune(keepCount, keepDays)` 的 `keepCount` 至少为 1。按倒序最多保留指定数量，并可额外设置天数限制，始终保留最新一个成功版本。先完整解析所有保留清单并验证其块，然后删除过期清单，最后仅删除未被保留清单引用的块。ADS 与普通数据流都会计入引用集合。中途中断可能留下冗余对象，但不会删除仍被保留清单需要的块。

## 密码更换与后台任务

`changePassword(newPassword)` 只生成新盐并重新封装相同随机主密钥。已有 `.cvo`、`.cvs` 字节及块 ID 均不变化，不触发全量重新加密或上传。

后台任务使用 `crypto::protectLocalKey(repository.masterKey())` 缓存当前 Windows 用户保护的主密钥。运行时调用 `Repository::withMasterKey(path, unprotectLocalKey(...))`；仓库 proof 检测缓存是否属于当前仓库。后台任务不需要保存明文密码。

## API 与结果

```cpp
cv::Repository repository(repositoryPath, backupPassword);
// 对尚未存在的新仓库，backup 会自动初始化；也可显式 init("aes256")。
auto backup = repository.backup(plan, context);
QString id = backup.details["snapshotId"].toString();
auto snapshots = repository.list();
auto manifest = repository.manifest(id);
auto restored = repository.restore(id, destination, options, context);
auto checked = repository.verify(context);
auto cleanup = repository.prune(10, 30, context);
repository.changePassword(replacementPassword);
```

新仓库目录必须为空。构造已有仓库会立即验证密码和配置。后台、GUI、CLI、上传和清理共享 `repository.lock`；并发冲突返回明确的 busy 错误，不无限等待。

备份结果包括逻辑文件数、逻辑字节数、新写入密文块字节数、快照 ID、新增块数、复用块数及警告。`strictMetadata=false` 时只允许元数据权限相关的显式警告，不把数据读取失败当作成功。

可执行测试覆盖稳定块复用、单块修改、旧版本独立恢复、清单加密、错误密码、DPAPI 缓存、密码重包、损坏检测、保留共享块、并发锁、取消和路径穿越拒绝。结论以实际测试 JSON 为准。
