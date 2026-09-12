# Router 测试体系（全引擎）

本文是"Router + Actor/MailBox 迁移"测试体系的**总纲**：分层架构、框架设计、
用例清单（含已实现与骨架）、执行方式与已知限制。

- 用例 ID 规范与编写模板：`skeleton/TESTID.md`
- 未接入组件的用例骨架：`skeleton/loginapp_router_cases.md`、`skeleton/dbmgr_router_cases.md`、
  `skeleton/managers_router_cases.md`

---

## 1. 分层架构

| 层 | 目标 | 形态 | 依赖 | 本机状态 |
|---|---|---|---|---|
| **L1 纯逻辑单元** | 路由表 / Mail 契约 / 协议序列 | 独立 exe（自研极简框架） | 无 KBE 依赖 | ✅ 29 例全通过 |
| **L2 静态收敛检查** | 守护"接线"不被回退 | PowerShell 扫描 | 无工具链 | ✅ 23 项全通过 |
| **L3 进程内协议序列** | RouterCore 分派顺序 + Registry 的组合语义 | 独立 exe（driver 复刻分派） | 无 KBE 依赖 | ✅ 已实现（并入 L1 可执行） |
| **L3′ 真实 router 进程** | 协议级端到端（Hello/注册/SEND/失败回报/迁移） | 零依赖 proto client + 脚本 | 需 router.exe | ⚠️ 已实现未运行（本机构建不出 router.exe，见 §4/§6） |
| **L4 集群 E2E** | ghost 同步 / 迁移 / 客户端可见性 / 开关等价 | 脚本 + assets | 需完整二进制 | ⛔ 待环境 |

设计取舍：仓库无 gtest，引入需改全平台构建；因此**纯逻辑用自研极简框架**
（`test_common.h`，几十行、零依赖、机读输出 + 退出码），
**协议/集群层沿用 `kbe/src/server/cluster/test` 的"零依赖 exe + 脚本驱动"范式**，
与既有工程习惯保持一致。

## 2. 框架设计

```
test_common.h        TestRunner：用例计数 + 断言 + 机读输出 + 退出码
   ├─ BEGIN_CASE(id, desc) / END_CASE()
   ├─ CHECK(cond) / CHECK_EQ_INT(a,b) / CHECK_EQ_STR(a,b)
   └─ summary(suite) -> 0 或 1
```

- 输出：`[ RUN ] [ OK ] [ FAILED ]`、`==== <suite>: N case(s), M failed ====`
- 退出码：`0` 全通过；`1` 失败；`2`（编排器）构建失败
- 新增用例只需写 `static void case_Xnn()` 并在 `main()` 注册，无需注册宏或改构建

**为什么不用 gtest**：需要新增第三方依赖目录 + 改造 `kbengine.sln`、`common.mak`
与所有平台构建，成本远高于收益；且 L1 用例全部是零依赖纯逻辑，自研框架足够。

## 3. 已实现用例

### 3.1 `router_registry_test`（R01–R12）

被测：`server/router/actor_registry.{h,cpp}`

| ID | 场景 |
|---|---|
| R01 | 绑定 / 查找 / 注销、换宿主、重复注销 |
| R02 | ActorKey 携带 componentType：base 与 cell 互不覆盖 |
| R03 | SUSPEND → 缓冲 → RESUME 按到达顺序冲刷到新宿主 |
| R04 | 挂起缓冲溢出丢弃最旧 |
| R05 | 挂起超时回滚到原宿主 |
| R06 | PENDING 入队（目标未注册），TTL 从首次入队算起 |
| R07 | PENDING 注册即冲刷，顺序一致 |
| R08 | PENDING 溢出：丢弃最旧并交给调用方回报 |
| R09 | PENDING TTL 到期逐条回报（src/dst/msgId/srcConnId 可还原） |
| R10 | **回归防护**：`bindActor()` 在 `takePending()` 之前会静默丢消息 |
| R11 | `dropPending` / `dropSuspended` |
| R12 | `removeConn` 清理 Actor/Service，保留 pending |

### 3.2 `router_protocol_test`（P01–P02、I01–I06）

被测：`lib/server/router_interface.h` 线格式 + ActorRegistry + RouterCore 分派顺序

| ID | 场景 |
|---|---|
| P01 | Mailbox 线格式 13 字节编解码往返；截断输入必须失败 |
| P02 | MSG_SEND 载荷 `dst+src+msgId+body` 编解码 |
| I01 | 目标未注册 → PENDING → 注册后按序冲刷 → 之后直达 |
| I02 | 待定缓冲溢出丢弃最旧 |
| I03 | 待定缓冲 TTL 到期逐条回报，到期后再注册不重复投递 |
| I04 | 组件 Actor 寻址；组件地址与实体地址不冲突（kind 不同） |
| I05 | 迁移 SUSPEND → 缓冲 → RESUME 冲刷到新宿主 |
| I06 | 迁移期间 REGISTER 等价于 RESUME（不丢缓冲） |

> driver（`RouterCoreDriver`）复刻 `router_core.cpp` 的 `onRegisterActor`/`MSG_SEND`/
> `SUSPEND`/`RESUME` 分派顺序；顺序本身的回归由 R10 + I06 + 静态检查共同守护。

### 3.3 `router_mail_test`（M01–M09）

被测：`lib/server/router_mail.h`

| ID | 场景 |
|---|---|
| M01 | 7 个 BodyTag 的 buildBody/parseBody 往返 |
| M02 | 空 body / tag=0 拒绝；只有 tag 合法 |
| M03 | `parseBodyPayload` 取跳过 tag 的载荷副本 |
| M04 | 地址推导（含 componentType 不参与 `operator==` 的语义固化） |
| M05 | 剥 baseapp 中继外壳（正常长度） |
| M06 | 剥中继外壳（扩展长度分支） |
| M07 | 截断/越界输入一律拒绝 |
| M08 | 只有外壳无客户端消息 → 失败 |
| M09 | 聚合多条客户端消息后仍可逐条解析 |

### 3.4 `converge_static_check.ps1`（S01–S23）

守护接线：RouterMail 契约、cellapp 派发与剥壳、GhostManager 组件 Actor 投递、
Witness 各发送点、baseapp `sendToCellapp` 与迁移缓冲旁路、旧转发缓冲感知、
client proxy Actor 绑定、组件 Actor 重建、迁移 SUSPEND、`ServerApp` 投递助手、
`entitycallabstract` 安全阀；S21–S23 额外比对 `router_proto_client.cpp` 中
重复定义的 `COMPONENT_TYPE` 常量与 `lib/common/common.h` 是否一致（防漂移）。

## 4. L3′ 真实 router 进程（I11–I19，已实现 / 本机未验证）

- 形态：零依赖 proto client `router_proto_client.cpp`（只
  `#include "lib/server/router_interface.h"` + 系统 socket，照 `cluster/test/test_client.cpp`
  范式），脚本驱动，机读输出与退出码同 L1。
- 前置：`router.exe`（`_link_router.bat` 产出，需 v142 工具集）；测试用配置
  `test/assets/res/server/kbengine.xml` 覆盖 `<router>` 段，经 `KBE_RES_PATH` 注入，
  不污染产品配置。

| ID | 场景 |
|---|---|
| I11 | HELLO 握手 + Actor 注册/注销（返回非 0 connId；REGISTER/UNREGISTER 回 OK） |
| I12 | 组件 Actor 直达投递（目标按序收到原样 dst/src/msgId/body） |
| I13 | 未注册 → 不立即 TARGET_NOT_FOUND（进 PENDING）→ 注册即按序冲刷 → 之后直达 |
| I14 | 待定缓冲溢出：发 `pendingBufferMax+1` 条 → 最旧一条收 `DROPPED_PENDING` |
| I15 | 待定缓冲 TTL 到期：每条各回一次 `DROPPED_PENDING`；到期后再注册不补投 |
| I16 | 迁移 SUSPEND → 缓冲（旧宿主静默）→ 新宿主注册(RESUME) 按序冲刷 |
| I17 | 实体/组件寻址隔离：同 actorId、不同 kind 不互串（kind 为判别位） |
| I18 | PING→PONG；过短 MSG_SEND → `BAD_REQUEST` 且连接仍可用（不断线） |
| I19 | CLIENT_TYPE 未注册：快速失败回 `TARGET_NOT_FOUND`（不进待定缓冲） |

- 执行：`run_tests.ps1 -WithRouter` / `sh run_tests.sh --with-router`。
  编排器用测试配置拉起 router 子进程 → 跑客户端 → 收尾；客户端连不上 router 时
  退出码 2，编排器输出 `[ SKIP ]` 且**不判失败**（不污染退出码）。
- 本机状态：`router.exe` 需 v143 链接 v142/GL 预编译库，必报 `C1905`，故**未做端到端
  运行**；本程序仅保证"可编译 + 用例逻辑自洽"。在有 v142 工具集（或 Linux 构建）的
  机器上一条命令即可验证，见 §7。
- 待定缓冲语义依赖 `pendingBufferMax>0`：若被配成 0，I14 自动 SKIP；I15 在
  `pendingTimeout>10000ms` 时自动 SKIP（避免用默认 30s 拖慢）。

## 5. L4 集群 E2E（待环境执行）

| 段 | 用例要点 |
|---|---|
| C01–C06（cellapp） | ghost 高频同步持续可见；无 "not found cellapp"；包量级不退化；客户端方法调用只执行一次；`controlledBy` 投递到控制者客户端；cell 未就绪时由 PENDING 补投 |
| B01–B05（baseapp） | `sendToCellapp` 经 Router；迁移期间不建旧缓冲；迁移不丢属性同步；`reqTeleportToCellAppOver` 走组件 Actor；失败可见 |
| E01–E06（全局） | 开关关闭逐字节等价（安全阀）；Router 重启后 Actor 重建；失败日志可见；多 cellapp ghost 压测；客户端可见性顺序；`pending` 上限/超时告警 |

## 6. 已知限制

1. **本机无法构建可执行组件**：`pythoncore` 链接失败 + v143/v142 LTCG 不兼容（C1905）。
   因此 L3′/L4 未在本机执行；L1/L2 不依赖引擎本体，已全部通过。L3′ 的
   `router_proto_client` 已可编译、用例逻辑自洽，但端到端仍需 v142/Linux 环境。
2. **`MailboxAddress::operator==` 不比较 `componentType`**（`router_interface.h:111`），
   base/cell 地址在该运算下相等；路由不受影响（`ActorKey` 含 componentType，见 R02），
   M04 已固化该语义。
3. **PENDING 与 SUSPEND 顺序敏感**：`bindActor()` 会清缓冲，必须先 `takePending()`（R10）。
4. Linux 侧脚本未在本机执行（无 WSL）；`common.mak` 有 `-Werror -std=c++11`，
   测试代码已按零警告编写。

## 7. 运行

```powershell
# Windows：L1 + L2（L3 自动 SKIP）
powershell -ExecutionPolicy Bypass -File run_tests.ps1
# Windows：静态检查单独跑
powershell -ExecutionPolicy Bypass -File converge_static_check.ps1
# Windows：L3 端到端（需 v142 工具集，先 _link_router.bat 产出 router.exe）
powershell -ExecutionPolicy Bypass -File run_tests.ps1 -WithRouter
```

```sh
# Linux / macOS
sh run_tests.sh
# Linux / macOS：L3 端到端（需先构建 ./router）
sh run_tests.sh --with-router
```

L3 的退出码约定：客户端 `0`=通过、`1`=用例失败、`2`=router 不可达（环境不可用，
编排器记 `SKIP` 且不影响整体退出码）。router 子进程日志落在 `test/run/router.*.log`。
