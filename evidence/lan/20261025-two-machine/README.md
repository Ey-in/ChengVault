# 真实双机局域网验收（2026-10-25）

| 项目 | 结果 |
|---|---|
| 结果 | PASS_TWO_MACHINE_LAN |
| realLanVerified | True |
| independentWindowsVerified | True |
| differentMachines | True |
| 服务器地址 | 192.168.0.100:48484 |
| 客户端 machineIdSha256 | c796493330fc72d174e71dfda6df6ab2ebbbe14414be739b913e3323aaa2f130 |
| 服务器 machineIdSha256 | 35c8fd52abfdc1022052970cf225561f0acafe2f0694380427818553169faa2e |
| 用例 | 15 全部通过 / 0 失败 |
| 客户端源码标识 | local-build |
| 服务器源码标识 | local-build |

## 证据文件

- `acceptance-results.json`：完整结果，hostName 已替换为 `<redacted-client-host>` / `<redacted-server-host>`。
- `screens/`：客户端 8 个页面截图，证明 GUI 在第二台机器正常启动。
- `console-output.txt`：15 个用例的 PASS 摘要。

## 复现命令

服务器：

```powershell
pwsh -File .\verification\external-acceptance.ps1 -Mode Server -EvidenceDirectory D:\acceptance-server-2 -ServerAddress 192.168.0.100 -Port 48484
```

客户端：

```powershell
pwsh -File .\verification\external-acceptance.ps1 -Mode Client -EvidenceDirectory D:\acceptance-client-2 -ConnectionFile D:\private\connection-private.json
```

## 隐私说明

- `connection-private.json`、`server-session.json`、账户密码和设备密钥均未收录。
- 原始运行目录（约 51 MB）保留在仓库外，不提交 `repository/`、`download/`、`source/`、`local-restored/`、`remote-restored/` 等测试数据。
- 提交到公开仓库前，hostName 已脱敏；machineIdSha256 是 MachineGuid 的 SHA-256，不是原始标识。
