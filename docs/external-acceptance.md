# 第二台 Windows 与真实局域网验收

`scripts/external-acceptance.ps1` 使用发布包的真实 GUI、CLI、Server。每个子进程的 PATH 仅保留 Windows 系统目录，并清除 Qt/QML 插件环境变量。脚本不需要本机 Qt、编译器或测试框架，只要求 PowerShell 7.2 以上、Windows 和 NTFS 工作目录。

运行包内置 `verification/external-acceptance.ps1` 和 `docs/validation-origin.json`。以下命令从完整解压后的运行包根目录执行，自动定位四个程序；若在源码目录执行，可改用 `scripts/external-acceptance.ps1`，或显式指定 `-RuntimeDirectory`。

外机验收以实际结果 JSON 为准。`PASS_TWO_MACHINE_LAN` 要求客户端与服务器的机器标识不同、使用非回环地址，且所有数据/隔离/增量检查实际通过。只有客户机还不同于 `validation-origin.json` 记录的开发机器，才把 `independentWindowsVerified` 记为 true。机器标识使用 Windows MachineGuid 的 SHA-256，不保存原始 MachineGuid。

## 机器 A：启动服务

将发布包完整解压，在服务器机器选择真实 LAN 地址，例如 `192.168.1.20`：

```powershell
pwsh -File .\verification\external-acceptance.ps1 -Mode Server -EvidenceDirectory D:\acceptance-server -ServerAddress 192.168.1.20 -Port 48484
```

脚本创建独立测试账户、独立设备密钥和独立存储目录，然后启动隐藏的 Server 进程。它不会修改已有服务器、注册登录启动或自动修改 Windows 防火墙。防火墙若提示，应按当前网络的实际管理策略允许所选端口。

保存屏幕输出中的 `server-session.json` 路径。把同目录的 `connection-private.json` 通过你选择的私密方式复制到机器 B；该文件含本次测试账户与配对凭据，不能作为公开测试报告上传。

## 机器 B：执行实际验收

同样完整解压发布包。原始开发机器标识文件随包提供，也可用 `-OriginFile` 指定：

```powershell
pwsh -File .\verification\external-acceptance.ps1 -Mode Client -EvidenceDirectory D:\acceptance-client -ConnectionFile D:\private\connection-private.json
```

客户机依次验证八页 GUI 启动、中文路径与空目录、PAX/Huffman/AES 加密归档和严格元数据恢复、8 MiB 随机数据的初次与重复上传、单字节修改仅上传一个块和一个清单、两账户隔离、下载与解密恢复、最终 SHA-256 一致。真实对象负载和带协议封装的发送量分别记录，重复上传的控制报文单独统计。

输出 `acceptance-results.json` 保存双方主机信息、发布源码版本、四程序 SHA-256、实际端点、退出码、预期错误码、每步结果与耗时。提交这份公开 JSON 及所需截图；原始配对文件和本机运行目录内的私密配置不进入源码或公开报告。客户机临时配置在测试结束时自动清除。

## 停止本次服务

回到机器 A，使用先前保存的会话记录：

```powershell
pwsh -File .\verification\external-acceptance.ps1 -Mode Stop -ServerSession D:\acceptance-server\server-日期-标识\server-session.json
```

停止操作比较机器标识、程序路径与进程启动时间，只结束本工具记录的那个进程。测试资料和密文存储保留供复核；需要删除私密配对文件时，由持有该文件的小组成员自行处理。

## 同机脚本自检

本机开发时可给 Server 和 Client 都显式添加 `-AllowSameMachineSelfTest`，服务地址使用 `127.0.0.1`。这类结果固定分类为 `PASS_SELFTEST_ONLY`，`realLanVerified=false`，不能作为第二台 Windows 或真实跨机 LAN 的通过证据。

## 文件与目录符号链接专项复验

在完整解压的运行包根目录执行：

```powershell
pwsh -File .\verification\symlink-acceptance.ps1 -EvidenceDirectory D:\acceptance-symlink
```

脚本在独立 NTFS 临时目录分别调用 `CreateSymbolicLinkW`，使用允许非提升创建的标志；不会修改开发者模式、安全策略或请求提升权限。若返回权限错误 1314 或 5，该项明确记为 `SKIP_PERMISSION`，不会计入 PASS；其他创建错误属于失败。能创建时，会使用发布 CLI 严格备份、恢复，并分别比较恢复对象的 SymbolicLink 类型、重解析属性、文件或目录属性以及原始目标字符串，只比较实际对象属性，普通内容副本不算通过。

输出 `symlink-results.json` 记录实际 Win32 错误码、当前账户 SID、管理员令牌状态、程序 SHA-256 与每项真实结果。进程正常退出也可能是权限跳过，必须查看 `result` 与 `passed/failed/skipped`，只有两项都成功才是 `PASS`。此专项不需要 Qt 开发环境或 C++ 编译工具。
