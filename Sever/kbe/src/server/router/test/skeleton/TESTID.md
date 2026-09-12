# 测试用例 ID 规范（全引擎 Router 迁移）

## 1. 段位分配

| 段 | 前缀 | 层 | 被测对象 | 状态 |
|---|---|---|---|---|
| Router 路由表 | `R01–R99` | L1 单元 | `router/actor_registry.*` | 已实现 R01–R12 |
| Mail 契约 | `M01–M99` | L1 单元 | `lib/server/router_mail.*` | 已实现 M01–M09 |
| 协议序列 | `P01–P99` | L1 单元（进程内） | `router_interface.h` 线格式 | 已实现 P01–P02 |
| 协议行为 | `I01–I99` | L3（进程内/真实进程） | RouterCore 分派 + Registry | I01–I06（进程内，已通过）/ I11–I19（真实进程，未验证） |
| 静态收敛 | `S01–S99` | L2 静态 | 全仓收敛点接线 | 已实现 S01–S23 |
| cellapp E2E | `C01–C99` | L4 集群 | ghost 同步 / client 直投 | 待环境 |
| baseapp E2E | `B01–B99` | L4 集群 | 迁移缓冲 / sendToCellapp | 待环境 |
| loginapp | `L01–L99` | L4 / 预留 | 登录改走 Router | SKELETON |
| dbmgr | `D01–D99` | L4 / 预留 | dbmgr 接入 Router | SKELETON |
| baseappmgr | `BM01–BM99` | L4 / 预留 | mgr 接入 Router | SKELETON |
| cellappmgr | `CM01–CM99` | L4 / 预留 | mgr 接入 Router | SKELETON |
| 全局端到端 | `E01–E99` | L4 集群 | 开关关闭等价 / 重启恢复等 | 待环境 |

编号在段内递增，**只增不改**：某条用例被废弃时保留 ID 并标注 `DEPRECATED`，
避免历史报告中的 ID 含义漂移。

## 2. 用例文档格式

```markdown
### <ID>  <一句话场景>

- 层：L1 / L2 / L3 / L4
- 前置：<环境、开关、数据>
- 步骤：
  1. ...
- 判定标准：<可机读的断言；脚本可解析的输出>
- 状态：IMPLEMENTED / IMPLEMENTED-UNVERIFIED（已实现，但受环境限制未做端到端运行验证） / SKELETON / DEPRECATED
- 实现位置：<文件:函数>
```

## 3. 断言与输出约定

- 断言宏见 `test_common.h`：`CHECK` / `CHECK_EQ_INT` / `CHECK_EQ_STR`。
- 输出行（脚本可解析）：
  - `[ RUN      ] <ID>  <desc>`
  - `[       OK ] <ID>` / `[   FAILED ] <ID>`
  - 汇总 `==== <suite>: N case(s), M failed ====`
- 退出码：`0` 全通过；`1` 有用例失败；`2` 构建失败（仅 Windows 编排器）。
- **不要**在断言里打印业务敏感内容（账号、IP）；日志遵循既有 INFO/WARNING/ERROR 分级。

## 4. L3′ 真实 router 进程用例（I11–I19）

被测：真实 `router.exe` 进程（`server/router/router_core.cpp` 的网络分派路径）。
实现：`server/router/test/router_proto_client.cpp`（零 KBE 依赖协议客户端）+ `run_tests.*`。
状态：**IMPLEMENTED-UNVERIFIED**——本机只有 v143，无法链接 v142/GL 预编译库（C1905），
故未做端到端运行；需在有 v142 工具集（或 Linux 构建）的机器上执行验证。

| ID | 场景 | 关键判定 |
|---|---|---|
| I11 | HELLO 握手 + Actor 注册/注销 | 两条连接各得非 0 `connId`；REGISTER/UNREGISTER 回 `MSG_RESP_OK` |
| I12 | 组件 Actor 直达投递 | 目标连接按序收到原样 `dst/src/msgId/body` |
| I13 | 未注册 → PENDING → 注册即冲刷 | 注册前**不**回 `TARGET_NOT_FOUND`；注册后按到达顺序冲刷，之后直达 |
| I14 | 待定缓冲溢出 | 发送方收到 `DROPPED_PENDING`，且被丢的是最旧一条(msgId 可对) |
| I15 | 待定缓冲 TTL 到期 | 每条缓冲 mail 各回一次 `DROPPED_PENDING`；到期后再注册不补投 |
| I16 | 迁移 SUSPEND → 缓冲 → RESUME | 挂起期间旧宿主静默；新宿主注册后按序冲刷 |
| I17 | 实体/组件寻址隔离 | 同 `actorId`、不同 `kind` 不互串（判别位） |
| I18 | Ping/Pong 与畸形报文 | PING→PONG；过短 SEND→`BAD_REQUEST` 且连接仍可用 |
| I19 | CLIENT_TYPE 未注册快速失败 | 直接回 `TARGET_NOT_FOUND`（不进待定缓冲） |

执行：`run_tests.ps1 -WithRouter` / `sh run_tests.sh --with-router`。
前置：`router.exe` 可由 `_link_router.bat` 产出（需 v142 工具集）；测试用配置
`test/assets/res/server/kbengine.xml` 覆盖 `<router>` 段（端口 20295/20296、
`pendingTimeout=2000`、`pendingBufferMax=8` 等）。I15 在客户端未拿到测试配置
（`--pending-timeout-ms>10000`）时自动 SKIP。

## 5. 新增用例的落地顺序

1. 先在本文档登记 ID（避免撞号）。
2. 纯逻辑优先落 L1（零依赖、可立即跑、可回归）。
3. 需要接线保障的补 L2 静态检查（`converge_static_check.ps1`）。
4. 需要进程/集群的落 L3/L4，并在 `TESTCASES.md` 标注执行前置与 SKIP 条件。
5. 更新 `run_tests.ps1` / `run_tests.sh` 的分层编排。
