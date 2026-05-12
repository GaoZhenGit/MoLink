# MoLink Access — 智能 push/pull & start/forward 分离 设计文档

> 日期：2026-05-12 | 分支：dev | 基于：2026-05-11-hotplug-and-file-transfer-design.md

## 一、背景

当前 molink-access-cpp 已具备单文件 push/pull/ls 和自动端口转发。本次新增两个能力：

1. **apush/apull**：智能 push/pull，参考 `D:\project\my-script\molink.py`，目录自动压缩/解压、base64 文件名编码、.gitignore 支持
2. **start/forward 分离**：`molink start` 仅启动 daemon，`molink forward` 独立控制端口转发

## 二、架构原则

- **daemon 零改动**：所有智能逻辑在 CLI 层，daemon 只做 ADB 传输
- **库依赖**：miniz（单文件公域 zip 库），放置方式与 libusb 一致
- **静态编译**：所有依赖编译进 molink.exe，零外部 DLL

## 三、新增文件

```
third_party/miniz/
├── miniz.h
├── miniz.c                    # 直接编译进 molink.exe
└── README.md                  # 源码地址 + 编译说明

src/utils/
├── base64.h                   # base64 编解码（自实现，~30 行）
└── gitignore.h/cpp            # .gitignore 匹配器（自实现，~150 行）
```

### miniz 说明

- 来源：https://github.com/richgel999/miniz
- 版本：3.0.2（单文件公域授权）
- API：`mz_zip_writer` 创建 zip，`mz_zip_reader` 解压 zip

## 四、功能一：apush（智能 push）

### 命令

```
molink apush <path> [--git] [--no-git] [--rdir <remote_dir>]
```

- `--rdir`：远程目录，默认 `/sdcard/tmp`
- `.gitignore`：自动检测（路径或上级目录有 `.gitignore` 则启用）
- `--git`：强制启用（没找到 .gitignore 时警告但仍上传）
- `--no-git`：强制禁用

### 流程

```
apush("myproject/")
  │
  ├─ 1. filesystem::is_directory(path)
  │
  ├─ 2. 若是目录:
  │     ├─ 检测 .gitignore → git_spec
  │     ├─ compressFolder(path, git_spec) → %TEMP%/xxx.molink.zip
  │     │     └─ miniz: 遍历目录，跳过 gitignore 匹配项，ZIP_DEFLATED
  │     ├─ remoteName = "b64_" + base64("xxx.molink.zip")
  │     └─ sendPipeCmd("push <temp.zip> <rdir>/<remoteName>")
  │         └─ 清理 temp zip
  │
  ├─ 3. 若是文件:
  │     ├─ remoteName = "b64_" + base64(原文件名)
  │     └─ sendPipeCmd("push <file> <rdir>/<remoteName>")
  │
  └─ 4. 打印: "已上传: xxx -> <rdir>/<remoteName>"
```

## 五、功能二：apull（智能 pull）

### 命令

```
molink apull [--rdir <remote_dir>]
```

无文件参数，纯交互式。

### 流程

```
apull()
  │
  ├─ 1. sendPipeCmd("ls <rdir>/") → 远程目录列表
  │
  ├─ 2. 解析 ls -la 输出:
  │     ├─ 按空白切分，取最后一列 → 文件名
  │     ├─ 过滤: 文件名以 "b64_" 开头
  │     ├─ base64 解码 → 显示名
  │     └─ 标记: 是否以 .molink.zip 结尾
  │
  ├─ 3. 交互菜单:
  │     === 选择要下载的文件 ===
  │     [0] myproject.molink.zip
  │     [1] config.txt
  │     输入序号:
  │
  ├─ 4. 选择后:
  │     ├─ sendPipeCmd("pull <rdir>/<rawName> <tempPath>")
  │     ├─ 若 .molink.zip:
  │     │     extractZip(tempPath, ".")  → 解压到当前目录
  │     │     删除 tempPath
  │     └─ 若普通文件:
  │           MoveFile(tempPath, ".\<解码名>")
  │
  └─ 5. 打印结果: "已下载: xxx（共 N 个文件）"
```

## 六、功能三：start/forward 分离

### 命令变化

```
当前:
  molink start -p 1080 -r 1081   →  daemon + 自动转发

改后:
  molink start                   →  仅启动 daemon
  molink forward [-p 1080] [-r 1081]  →  设置端口转发（需 daemon 运行，默认 -p 1080 -r 1081）
  molink run -p 1080 -r 1081     →  前台模式保持不变（自动转发）
```

### daemonMain 改动

```cpp
// 原: Forwarder 在启动时创建
Forwarder forwarder(client, localPort, remotePort);
forwarder.start();

// 改: 按需创建
std::unique_ptr<Forwarder> forwarder;  // 初始为空
```

**Pipe handler 新增：**
```
"forward <localPort> <remotePort>"
  → 若已有 forwarder 则 stop() + reset()
  → make_unique<Forwarder>(client, lp, rp) + start()
  → 返回 "ok: forwarding 127.0.0.1:<lp> → device tcp:<rp>"
```

**status 命令适配：**
```
forwarder 存在时: "connected  serial=...  forwarding=<lp>→<rp>  connections=N"
forwarder 为空时: "connected  serial=...  forwarding=off"
```

### 影响范围

| 文件 | 改动 |
|------|------|
| `src/main.cpp` daemonMain | Forwarder → unique_ptr，pipe 新增 forward，status 适配 |
| `src/main.cpp` cmdStart | 移除 -r 等待逻辑，仅检查 daemon 存活 |
| `src/main.cpp` 新增 cmdForward | 解析 -p/-r 参数，发送 pipe 命令 |
| `src/main.cpp` printUsage | 更新帮助文本 |
| `src/main.cpp` transitionToDisconnected | forwarder 操作加判空 |
| `src/forward/forwarder.h` | 新增 `getRemotePort()` |

## 七、.gitignore 匹配规范

实现 gitwildmatch 子集（覆盖常用模式）：

| 模式 | 含义 | 示例 |
|------|------|------|
| `*.ext` | 匹配任意目录下的 .ext 文件 | `*.o`, `*.exe` |
| `dir/` | 匹配名为 dir 的目录 | `build/`, `node_modules/` |
| `**/dir` | 匹配任意深度的 dir | `**/__pycache__` |
| `/path` | 仅匹配根下的 path | `/CMakeLists.txt` |
| `!pattern` | 取反（白名单） | `!important.o` |
| `#` 开头 | 注释 | `# 这是注释` |

不支持：`[abc]` 字符类、`?` 单字符通配（后续按需添加）。

## 八、错误处理

| 场景 | 行为 |
|------|------|
| daemon 未运行 | "Daemon is not running. Use 'molink start' first." |
| 本地路径不存在 (apush) | "文件/目录不存在: <path>" |
| 远程目录不存在 (apull) | "目录为空" 或显示空列表 |
| 远程 ls 失败 | "fail: No output from shell ls command" |
| 压缩失败 | "压缩失败: <reason>" |
| 解压失败 | "解压失败: <reason>"，保留原始 zip |
| daemon disconnected | "Device is disconnected, waiting for reconnect..." |
| forward 时 daemon 未运行 | "Daemon is not running. Use 'molink start' first." |

## 九、测试

### apush

1. `molink apush testdir` → 目录压缩上传，显示 `已上传文件夹: testdir -> /sdcard/tmp/b64_xxx`
2. `molink apush test.txt` → 单文件上传，显示 `已上传: test.txt -> /sdcard/tmp/b64_xxx`
3. `molink apush testdir --no-git` → 忽略 .gitignore，上传所有文件
4. `molink apush /nonexistent` → 报错文件/目录不存在
5. 不启动 daemon 执行 apush → 报错 daemon 未运行

### apull

1. `molink apull` → 列出远程文件，选序号下载
2. 选择 `.molink.zip` 文件 → 自动解压到当前目录
3. 选择普通文件 → 下载到当前目录（解码名称）
4. 输入无效序号 → 报错
5. Ctrl+C → 取消退出

### start/forward 分离

1. `molink start` → status 显示 `forwarding=off`
2. `molink forward -p 1080 -r 1081` → status 显示 `forwarding=1080→1081`
3. `molink forward -p 2080 -r 1081` → 旧转发停止，新端口启动
4. `molink forward` 在 daemon 未启动时 → 报错
