# Windows bootstrap 套件

`install-device-agent.cmd` 是现场双击入口，负责 UAC 提权并调用 `install-device-agent.ps1`。安装脚本支持本地 EXE/ZIP、在线下载以及 bootstrap turnkey 解压后的完整套件。

## 生成完整套件

在 Windows 构建机执行：

```powershell
.\make-kit.ps1 `
  -AgentDir C:\build\device-agent\Release `
  -AgentVersion 1.2.3 `
  -VcRedistPath C:\deps\vc_redist.x64.exe `
  -OutZip C:\out\device-agent-kit.zip
```

`-AgentDir` 必须同时包含 `device-agent.exe` 和其依赖 DLL。提供 `-VcRedistPath` 后，套件包含：

- 根目录的 Agent、DLL、CMD、PowerShell 安装脚本；
- `prerequisites/vc_redist.x64.exe`；
- `SHA256SUMS.txt`，记录打包时所有 payload 的 SHA-256。
- `DEVICE_AGENT_MANIFEST.txt`，记录与二进制、EnrollRequest、周期 inventory 一致的 `agent_version`。

`-AgentVersion` 必填，且脚本会执行 `device-agent.exe --version` 做逐字一致性校验；因此正式测试/发布套件缺版本或版本漂移会在打包阶段失败。开发构建仅允许 CMake 默认值 `0.0.0-dev`，正式构建需同时设置：

```powershell
cmake -S . -B build -DDEVICE_AGENT_BUILD_WINDOWS_AGENT=ON `
  -DDEVICE_AGENT_RELEASE_BUILD=ON -DDEVICE_AGENT_VERSION=1.2.3
```

安装脚本的 VC++ Runtime 选择顺序：

1. `vc_redist_url` + `vc_redist_sha256`；
2. 套件内 `prerequisites/vc_redist.x64.exe` + `SHA256SUMS.txt` 对应 hash；
3. 两者都没有时，兼容既有离线包，记录警告并假定目标机已安装 Runtime。

发现 bundled redist 但缺 hash、hash 不匹配或安装失败时，脚本以 prerequisite exit code `8` 退出，不继续安装 Agent。成功码接受 `0`、已存在更新版本 `1638` 和需要重启 `3010`；`3010` 会记录警告，随后继续尝试安装和启动服务。

## 安全边界

- 在线 URL 不允许只填 URL 不填 SHA-256。
- `SHA256SUMS.txt` 保护 redist payload 的传输/打包完整性；套件本身仍必须从受信 HTTPS bootstrap 链路获取。
- 不要把 installer key、设备 token 或生产地址写进仓库内 answer 示例。
