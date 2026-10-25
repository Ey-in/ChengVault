# 网络备份协议与运行说明

网络模块的实现位于 `src/core/network.cpp`；只使用 Qt TCP 套接字承载字节。帧协议、认证握手、密码派生、认证加密、续传、对象查重、配额和用户管理由本项目实现，没有调用 TLS、OpenSSL、现成网盘或云存储 SDK。

## 启动一个本机服务器

以下命令在发布目录运行。先把账户密码写入本机 UTF-8 文件 `alice-password.txt`。文件只供初始化读取，配置后删除明文密码文件。不要把真实凭据提交到 Git。

```powershell
.\ChengVaultServer.exe user-add --root .\server-data --user alice --password-file .\alice-password.txt --administrator
.\ChengVaultServer.exe device-add --root .\server-data --user alice --device desktop --key-file .\desktop-key.hex
.\ChengVaultServer.exe serve --root .\server-data --listen 127.0.0.1 --port 48484
```

`device-add` 在密钥文件不存在时生成独立的随机 32 字节配对密钥；已经存在时读取 64 位十六进制密钥。服务器数据库中保存经当前 Windows 用户 DPAPI 保护的设备密钥。客户端配对需要安全取得该密钥文件；服务端不会通过未认证的 TCP 请求分发密钥。

同一客户端必须同时提供账户名、账户登录密码、设备标识和设备配对密钥。登录密码与备份仓库密码相互独立。服务器不能使用登录密码解密备份。

局域网演示显式改为 `--listen 0.0.0.0`，客户端填写服务器局域网 IP。防火墙是否放行由实际演示环境配置。默认仅监听本机。启动成功输出 `READY <实际端口> <监听地址>`；`--port 0` 可为自动化测试选择可用端口。

## 客户端接口

```cpp
cv::network::RemoteOptions options;
options.host = "127.0.0.1";
options.port = 48484;
options.user = "alice";
options.password = loginPassword;
options.deviceId = "desktop";
options.deviceKey = QByteArray::fromHex(cv::readFile(keyFile).trimmed());
cv::network::Client client(options);
auto uploadResult = client.upload(localRepositoryPath);
auto remoteRepositories = client.list();
auto downloadResult = client.download(repositoryId, emptyDestination);
```

调用是同步的，GUI 在工作线程中调用。每个接口建立并关闭一个完整认证会话。`Context` 提供进度和取消。下载完成后用 `Repository(downloadResult.output, backupPassword)` 打开，随后选择快照恢复。

上传返回 `TaskResult.details` 中的 `uploadedObjects`、`reusedObjects`、`payloadBytes`、`sentBytes`、`receivedBytes`、`resumedBytes`。`payloadBytes` 是本次真正发送的密文对象字节；`sentBytes` 包含握手、JSON、Base64、认证标签和分帧开销，因此即使重复上传的数据量为零，控制报文仍产生流量。

远端列表只显示仓库 ID、更新时间、加密存储字节数、快照数量和对象数量。文件名、目录树、属主、权限与文件级元数据仍保存在加密清单里，客户端解密后才能浏览。

## 认证与传输流程

1. 客户端发送协议版本、账户、设备 ID 和 32 字节随机数。
2. 服务端确认账户与设备的绑定关系，生成独立随机数，用设备 PSK 对握手摘要与 `server` 方向标识作 HMAC。
3. 客户端验证服务端证明，再返回 `client` 方向的 HMAC 证明。
4. 双方以设备 PSK 和整个握手摘要通过 HKDF 派生两个不同方向的会话密钥。
5. 加密后的第一条业务消息是账户密码登录。服务端用独立随机盐、120000 次 PBKDF2-HMAC-SHA256 验证密码。
6. 后续所有业务消息均封装为“32 字节会话标识 + 8 字节大端递增序号 + JSON”，使用自主 ChaCha20/HMAC 密文信封传输。

双方随机数绑定在会话密钥中；方向密钥不同；密文信封认证成功后才读取序号。序号必须严格等于下一期望值，重复、乱序和跨会话数据均被拒绝。采用预共享设备密钥的此协议不提供前向保密，报告不作 TLS 等价性声明。

外层帧是 4 字节大端无符号长度加密文，最大 8 MiB。单次数据传输段最大 1 MiB，单个仓库对象最大 64 MiB，单个 inventory 最多 50000 项且仍受帧长限制。服务端同时最多 32 个会话；有界读取超时防止半包永久占用线程。

每次业务请求重新检查账户禁用状态、密码版本和设备授权。禁用账户、重置密码、撤销设备或轮换设备密钥会使原会话的后续业务请求失效。

## 增量上传、原子提交与恢复

客户端先锁定本地仓库，枚举 `repository.json`、加密快照和加密块，计算每个密文文件的大小与 SHA-256。稳定的原始块 HMAC ID 保证仓库不会为未变化的块重复生成随机密文，因此网络层可以真正跳过旧块。

服务端按以下路径隔离账户和仓库：

```text
server-data/
  accounts.json
  users/<user>/repositories/<repository-id>/
    index.json
    upload.lock
    blobs/<ciphertext-sha256>
    blobs/<ciphertext-sha256>.part
```

`begin` 验证清单、相对路径、重复项、大小与账户配额，返回缺失对象及暂存偏移。`put` 只接受本次清单内对象，严格核对连续偏移，完整接收后核验 SHA-256 再发布不可变 blob。`commit` 确认对象齐备，原子替换 index，随后清理旧 index 不再引用的密文 blob。失败时旧 index 继续有效。

中断上传保留 `.part`，下次同一对象从已确认的偏移继续。如果末尾校验失败，下次从零重新传输。上传期间同一远端仓库只能有一个写入任务；不同用户不会共享 blob。

下载目标必须为空。程序使用同目录附近的专属暂存目录下载，校验每个对象后才发布完整仓库。取消后再次选择同一空目标可继续。若服务器在下载期间提交了更新，被移除的旧对象会返回明确错误；重新下载取得新 index，不将两个版本静默混合。实际恢复到已有文件由本地恢复模块负责，默认不覆盖。

配额按当前账户所有已提交仓库 inventory 总大小计算，提交时在互斥区重新检查以避免两个并发提交同时通过剩余额度检查。当前实现保留中断上传暂存以支持续传，因此长期无人继续的暂存文件需要管理员根据磁盘情况清理；不宣称具有自动暂存过期策略。

## 管理操作

管理员通过 `Client::admin(action, args)` 在认证加密通道内操作。普通账户调用管理接口被拒绝。

| action | args | 行为 |
|---|---|---|
| `list-users` | `{}` | 返回账户、管理员状态、禁用状态、配额和设备 ID，不返回密码散列或配对密钥 |
| `create-user` | `user,password,administrator,quota` | 创建账户；quota 为字节数字字符串 |
| `disable-user` | `user,disabled` | 禁用或启用账户；不能禁用当前管理员自己 |
| `reset-password` | `user,password` | 重置登录密码，不改变仓库密码 |
| `set-quota` | `user,quota` | 调整账户配额，必须大于零 |
| `register-device` | `user,device,key` | 注册或轮换 64 位十六进制设备密钥 |
| `revoke-device` | `user,device` | 撤销设备 |

## 可执行测试证据

`tests/network_repository_tests.cpp` 使用真实本机 TCP、两个独立账户及第三个动态创建账户。覆盖完整上传下载恢复、重复上传零数据负载、变化块上传、实际取消后的偏移续传、用户隔离、错误密码、错误设备密钥、普通用户管理拒绝、密码重置、禁用、配额、设备撤销、重放、密文篡改和超长帧拒绝。测试通过与否以最终执行产生的 JSON 记录为准。
