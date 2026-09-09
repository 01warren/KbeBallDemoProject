# Cluster 自动化测试工程

对 `kbe/src/server/cluster` 组件（多副本选主 + 日志复制 + 注册表状态机 + 本机代理起停）的系统化功能测试。

- 入口脚本：`run_tests.ps1`
- 工具：`test_client.exe` / `snapshot_gen.exe` / `dummy_proc.exe`（由 `_build_tests.bat` 编译）
- 被测对象：`bin/server/cluster.exe`（构建方式见仓库既有工程）

## 1. 目录与工具

| 文件 | 作用 |
| --- | --- |
| `assets/single/res/server/kbengine.xml` | 单副本资产（TTL 6s，用于单副本链路 / ctl） |
| `assets/single_long/res/server/kbengine.xml` | 单副本资产（长租约，避免快照用例误触发 TTL） |
| `assets/{a,b,c}/res/server/kbengine.xml` | 三副本资产（127.0.0.1/2/3，端口 20193/20194，lease 60s） |
| `test_client.cpp` | 协议客户端：leader/reg/renew/unreg/find/queryall/ctl，机读输出 + `--timeout=` |
| `snapshot_gen.cpp` | 制造确定性状态文件 `cluster_state_*.bin`（回归快照加载偏移问题） |
| `dummy_proc.cpp` | 伪进程：打印 PID、开隐藏窗口、默认不退（用于验证 ctl kill/stop 真实进程语义） |
| `run_tests.ps1` | 主套件；`-Fast` 跳过 TTL，`-Full` 追加多数派丢失组 |
| `run/` | 运行期日志（每个副本 cwd 独立，状态文件也落在各自子目录） |

端口约定：service 20193，peer 20194（与冒烟套件的 20093/20094 隔离，可并行执行）。

## 2. 用例总览

| 组 | 用例数 | 覆盖点 |
| --- | --- | --- |
| G1 | 17 | 单副本注册表链路、单成员写应答回归、TTL 租约、ctl 起停/杀 |
| G2 | 10 | 快照落盘/加载偏移回归、损坏快照容忍、快照后继续写 |
| G3 | 13 | 三副本选举唯一性、follower 重定向、2/3 多数派、故障切换、旧 leader 重入、选举抖动活锁回归 |
| G4 | 6 | 多数派丢失时写阻塞（`-Full` 专属）、恢复副本后服务自愈 |

- 默认（无参数）：40 项（G1 全量含 TTL）
- `-Fast`：36 项（跳过 G1-8a/8/9/10 的 TTL 部分）
- `-Full`：46 项（追加 G4）

## 3. G1 单副本链路（asset: single）

| ID | 断言要点 | 备注 |
| --- | --- | --- |
| G1-0 | 单副本 15s 内自举为 leader | 注册表可自服务的最小部署 |
| G1-1 | `reg` 立即回 `MSG_RESP_OK`，耗时 < 2500ms | **回归：registerWaiter**——单成员下提交先于 waiter 登记，原实现漏回包导致 4s 超时 |
| G1-2 | `renew` OK | 续租链路 |
| G1-3 | `find (1,6,40001)` 精确命中 `MSG_RESP_ENTRY` 且 CID 一致 | 组件引导精确查找语义 |
| G1-4 | `find (1,6,0)` 按类型列出含 40001 | 按类型发现语义 |
| G1-5 | `queryall` count>=1 | 整表查询 |
| G1-6 | 重复 (uid,type,cid) 被拒 `MSG_RESP_IDENTITY_CONFLICT` | 去中心化 ID 的冲突检测 |
| G1-7 | 不同 uid 可复用同 type+cid | key=(uid,type,cid) 隔离验证 |
| G1-8a/8 | 双组件注册 + 持续 renew 租约保持 | 默认模式；lease=6s 下以 1.1s 间隔续租 7 轮 |
| G1-9/10 | 停止续租后条目按租约过期移除 | TTL 下线语义；默认模式 |
| G1-11 | `ctl start` type=15(CLUSTER) 拒绝 code=-1 | 禁止对集群类型自身起停 |
| G1-12 | `ctl start` 缺失可执行文件 → code=-1 | 本机代理失败路径 |
| G1-13 | `ctl stop` 无运行实例 → code=0 且消息含 no running | no-op 语义 |
| G1-14 | `ctl kill` 终止真实进程（dummy_proc，code=0 且进程退出） | 本机代理执行 taskkill /f |
| G1-15 | `ctl stop` 优雅路径二义性收敛：交互式窗口站 code=0 且进程退出；非交互窗口站则明确失败 code=-1（消息含原因），runner 兜底强杀 | taskkill 不带 /f 依赖 WM_CLOSE 投递能力 |

## 4. G2 快照加载回归（asset: single_long）

| ID | 断言要点 | 备注 |
| --- | --- | --- |
| G2-0 | `snapshot_gen` 产出状态文件 | 预置 2 条注册记录 |
| G2-1 | 文件头 magic=`0x4B42E533` | 校验字节序与布局 |
| G2-2 | 带状态文件启动，15s 内成 leader | 加载后仍能参与集群 |
| G2-3/4 | `find` 两条预置记录均恢复 | **回归：loadSnapshotFromDisk 4 字节偏移**——原实现未跳过 magic 头，导致快照解码错位、记录不可见 |
| G2-5 | `queryall` count=2 | 数量精确恢复 |
| G2-6 | 快照基础上追加注册 OK | 快照 + 新日志并存 |
| G2-7/8/9 | 破坏 magic 头后：忽略损坏文件正常启动、注册表为空、服务可继续写 | 损坏容忍，不崩溃、无半加载状态 |

## 5. G3 三副本一致性与故障切换（assets a/b/c）

| ID | 断言要点 | 备注 |
| --- | --- | --- |
| G3-1 | 3 副本 25s 内选出 leader | 启动冒烟 |
| G3-2 | 恰好一个副本自报 leader | 无双主 |
| G3-3 | follower 上 `find` 返回 `NOT_LEADER` + 真实 leader 地址 | 客户端重定向语义 |
| G3-4 | leader 上注册 OK | |
| G3-5/6 | 杀 1 个 follower 后仍以 2/3 多数派提交 | 降级容忍 |
| G3-7 | follower 重入追赶日志后不干扰现任 leader | 日志追赶/复制收敛 |
| G3-8/9/10 | 杀 leader 后剩余两副本在 20s 内选出新 leader 且不同于旧 | **回归：选举抖动活锁**——原实现用常量 RNG 种子，各副本选举 deadline 完全同步、互不投票；修复为按进程播种（见 §7 Bug-3） |
| G3-11 | 切换后旧注册条目(51001/51002)保留 | 状态机跨 term 持久 |
| G3-12 | 旧 leader 重入后仍恰好一个 leader | 重入不自立 |
| G3-13 | 重入后注册 51003 且 `queryall` count>=3 | 全员一致 |

## 6. G4 多数派丢失（assets a/b/c，-Full 专属）

| ID | 断言要点 | 备注 |
| --- | --- | --- |
| G4-1 | 3 副本选出初始 leader | |
| G4-2 | 两个 follower 先后下线（端口探测确认） | |
| G4-3 | leader 仅剩 1/3 时 `reg` 无提交应答（阻塞） | 防脑裂只读语义：多数派丢失即拒绝写 |
| G4-4/5 | 恢复 1 个副本后重新选主、写路径自愈 | 多数派恢复即服务恢复 |

## 7. 回归缺陷与修复记录

| # | 缺陷 | 症状 | 修复 |
| --- | --- | --- | --- |
| 1 | `registerWaiter` 错过已提交条目 | 单成员/多数派已提交场景客户端 4s 超时无应答 | waiter 登记时若 `isLeader && index<=commitIndex_` 立即补发 `MSG_RESP_OK` |
| 2 | `loadSnapshotFromDisk` 4 字节错位 | 快照恢复后注册记录全部不可见/错乱 | 校验 magic 后从 `bytes.substr(4)` 解码 |
| 3 | 选举抖动使用常量 RNG 种子 | 多 follower 以完全相同的 deadline 进入选举，同 term 互不投票 → 活锁，leader 故障后无新主（G3-9 场景） | 实例构造时以 `nowWallMS ^ (&g_randState<<32) ^ (selfIdx<<48)` 播种 |
| 4 | 跨用例组残留状态文件 | G3 触发快照落盘后，G4 复用同一 run 目录加载旧注册表，多数派用例基线被污染（G4-3 假失败） | `run_tests.ps1` 新增 `Clean-TagDir`，每组启动前清空其运行目录 |
| 5 | `run_smoke.ps1` 函数返回值污染 | `Invoke-Smoke` 把 client stdout 与退出码一起返回（object[]），`-eq 0` 判定恒假，冒烟 S1 假失败 | 函数内 `... | Out-Null`，仅返回 `$LASTEXITCODE` |

## 8. 运行方式

```powershell
# 构建测试工具（需 VS 开发者命令行环境）
test\_build_tests.bat

# 默认全量（含 TTL 慢速用例，约 45s）
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1

# 快速模式：跳过 TTL 用例（约 25s）
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1 -Fast

# 多数派丢失组（含 TTL，约 60s）
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1 -Full

# 保留副本进程以便人工核对
powershell -ExecutionPolicy Bypass -File test\run_tests.ps1 -KeepRunning
```

前置条件：

- `bin/server/cluster.exe` 已构建（Release x64）。
- 三个工具已编译于 `test\` 目录。
- 无其它 cluster/dummy_proc 进程占用端口 20193/20194（脚本尾部会自动兜底清理）。
- Windows 环境：ctl 用例依赖 Windows 进程管理（CreateProcess/taskkill 语义）。

日志位置：`test/run/*.stdout.log`（每个副本一份）、`dummy*.out`（伪进程输出）。脚本退出码：0=全绿。

## 9. 验证结果（最近一次）

| 运行方式 | 结果 |
| --- | --- |
| `run_tests.ps1`（默认） | 40/40 PASS，~44s |
| `run_tests.ps1 -Fast` | 36/36 PASS |
| `run_tests.ps1 -Full` | 46/46 PASS（含 G4 多数派丢失） |
| `smoke/run_smoke.ps1` | 14/14 PASS（S1 启动选主、S2 注册续租查询、S3 TTL、S4 leader 故障切换、S5 旧 leader 重入） |

## 10. 已知环境限制

- G1-15 优雅停止在无交互窗口站的会话（如 CI/远程服务会话）中，`taskkill`（不带 /f）无法投递 `WM_CLOSE`，断言按“成功优雅退出 或 明确失败码 -1 + 原因”双轨收敛，二者都判定为语义正确。
- 三副本资产绑定 127.0.0.1/2/3 单机回环，验证的是协议/一致性语义；真实跨机部署时成员表应填各机地址（见 `kbengine_defaults.xml` `<cluster><addresses>`）。
