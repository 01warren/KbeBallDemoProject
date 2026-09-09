# KBEngine cluster 集群技术手册（实现级）

> 本手册与 `kbe/src/server/cluster/` 源码逐行对齐，是**实现细节的唯一权威描述**。
> 设计意图、迁移历史与已知取舍见 `docs/kbe_cluster_design.md`，两文档配套阅读。
>
> 适用范围：本仓库 `kbe/` 引擎源码。标注的版本/常量均以 2026-09 源码为准。
>
> 若源码与本文矛盾，**以源码为准**，并请同步修正本文。

---

## 目录

1. [组件定位与对外接口](#1-组件定位与对外接口)
2. [目录与构建工程](#2-目录与构建工程)
3. [进程骨架（Cluster）](#3-进程骨架cluster)
4. [配置项与端口](#4-配置项与端口)
5. [ClusterCore 一致性核心](#5-clustercore-一致性核心)
   - 5.1 线程模型与事件队列
   - 5.2 Raft 状态与角色机
   - 5.3 成员与链路管理
   - 5.4 选举
   - 5.5 心跳与日志复制
   - 5.6 提交与 apply
   - 5.7 TTL 扫描
   - 5.8 快照与日志归档
6. [服务面协议 cluster_interface](#6-服务面协议-cluster_interface)
   - 6.1 帧格式与连接模型
   - 6.2 消息总表
   - 6.3 Wire 序列化
   - 6.4 ComponentData 字段序
   - 6.5 ServerCommandData 字段序
   - 6.6 各消息载荷定义
   - 6.7 服务请求处理流程
7. [副本间协议 PeerProto](#7-副本间协议-peerproto)
   - 7.1 帧格式
   - 7.2 消息布局
   - 7.3 快照同步
8. [注册表状态机](#8-注册表状态机)
   - 8.1 数据模型与 key
   - 8.2 applyEntry 语义
   - 8.3 注册冲突检测
   - 8.4 TTL 租约
9. [远程起停指令处理 handleCtlCommand](#9-远程起停指令处理-handlectlcommand)
   - 9.1 消息路由与本地代理（MSG_PROXY_*）
   - 9.2 START 语义
   - 9.3 STOP / KILL 语义
10. [本机代理执行器 ClusterProcess](#10-本机代理执行器-clusterprocess)
11. [组件侧客户端 lib/server/components.cpp](#11-组件侧客户端-libservercomponentscpp)
    - 11.1 clusterRequest 重试与重定向
    - 11.2 注册状态机
    - 11.3 续租与发现
    - 11.4 优雅注销
12. [组件 ID 生成（kbemain.h）](#12-组件-id-生成kbemainh)
13. [快照文件格式](#13-快照文件格式)
14. [单成员模式](#14-单成员模式)
15. [关键数值速查](#15-关键数值速查)
16. [已知限制与约定](#16-已知限制与约定)

---

## 1. 组件定位与对外接口

`cluster` 是引擎内自研的高可用集群组件，替代原 `machine` 的职责：

| 替代职责 | 对外面 | 协议 |
|---|---|---|
| 组件注册（原 UDP onBroadcastInterface） | 服务面 `servicePort` TCP | `cluster_interface.h` |
| 组件发现 / 启动引导 find | 服务面 `servicePort` TCP | `cluster_interface.h` |
| 组件存活判定（原"本机进程表"） | —— | TTL 租约 + Renew/Expire 日志 |
| guiconsole 远程起停 start/stop/kill | 服务面 `servicePort` TCP | `cluster_interface.h` + 副本间转发 |
| 副本间一致性 | 内部 `peerPort` TCP | `cluster_core.cpp` 内 `PeerProto` |

进程内两个监听端口各自独立：

- **servicePort（默认 20093）**：组件/工具统一入口。任意副本皆可连，非 leader 副本对“需要 leader 的消息”回 `MSG_RESP_NOT_LEADER`（含 leader 地址）。
- **peerPort（默认 20094）**：副本间选举心跳 / 日志复制 / 快照同步。只接受**成员地址表内**的对端连接。

> 与 machine 的最大差异：所有交互从 UDP 广播改为 **TCP 直连 + 长度前缀帧**；写操作全部经 leader 日志线性一致；读操作（FIND/QUERY_ALL）也由 leader 提供（未实现 lease-read）。

---

## 2. 目录与构建工程

```
kbe/src/server/cluster/
├── cluster.h / cluster.cpp          # ServerApp 子类进程骨架
├── cluster_interface.h              # 服务面协议（纯头文件，自包含）
├── cluster_core.h / cluster_core.cpp# 一致性核心 + 双端口监听 + 注册表状态机 + TTL/快照
├── cluster_process.h / .cpp         # 本机代理执行器（进程起停）
├── main.cpp                         # KBENGINE_MAIN 入口
├── imp_stubs.cpp                    # 引擎消息存根
├── Makefile                         # Linux
├── cluster.vcxproj / .filters       # Windows
├── smoke/                           # 早期冒烟资产与脚本（测试工程演进为 test/，见下）
└── test/                            # 集成测试工程（用例文档 TESTCASES.md 见工程内）
```

构建挂载点（已实现）：

- Linux：`kbe/src/server/Makefile` 中 `cluster` 子目录行。
- Windows：`kbe/src/kbengine.sln` 中 `cluster.vcxproj`；工程继承 `lib/server`、`lib/network`、`lib/python` 等既有依赖。
- 组件类型：`CLUSTER_TYPE = 15`，`COMPONENT_END_TYPE = 16`（`lib/common/common.h`，编号在旧类型之后追加，不改变既有枚举值）。

---

## 3. 进程骨架（Cluster）

`class Cluster : public ServerApp, public Singleton<Cluster>`。

### 3.1 生命周期

| 阶段 | 行为 |
|---|---|
| `initializeBegin()` | 打印 `servicePort/peerPort/members` 数量；调用基类初始化 |
| `inInitialize()` | 基座初始化（引擎引导） |
| `initializeEnd()` | **先** `startServiceListeners()`（失败则返回 false，进程退出）；随后注册 100ms 游戏帧定时器 `TIMEOUT_GAME_TICK` |
| 主循环 | 每帧（100ms）`pCore_->tick()` 推进一致性核心 |
| `finalise()` | 取消定时器 → `pCore_->stop()`（关监听、join 网络线程）→ 基类释放 |

### 3.2 startServiceListeners

1. 取 `<cluster>` 配置：`internalInterface`（绑定网卡，空字符串自动选择）、`clusterServicePort`、`clusterPeerPort`、三个租约/心跳参数。
2. 计算本机 IP（`intTcpAddr().ip` → 点分十进制 `selfIP`）。
3. 成员表逻辑：
   - `<addresses>` 为空 → 单成员模式，`members = [selfIP]`（开发/演示）。
   - 非空但**不含本机 IP** → ERROR 并返回 false（启动失败）。
   - 否则找到本机在表中的下标 `selfIdx`（成员序号按表顺序 0..N-1）。
4. 构造 `ClusterCore(selfIP, selfIdx, members, servicePort, peerPort, electionTimeoutMS, heartbeatIntervalMS, componentHeartbeatInterval(s), componentLeaseSeconds(s))` 并 `start()`。

### 3.3 main.cpp

`kbeMainT<Cluster>(argc, argv, CLUSTER_TYPE, -1,-1(ext tcp min/max), -1,-1(ext udp), "", 0,0(int listening), info.internalInterface)`。

说明：

- cluster **不绑定引擎外部端口**（ext 端口范围全 -1），引擎内部网（intTcpEndpoint）仅用于少量引擎级消息，服务面走自建 TCP。
- `main.cpp` 通过 `#undef/#define DEFINE_IN_INTERFACE` 方式把 baseappmgr/cellappmgr/cellapp/baseapp/dbmgr/loginapp/logger/interfaces/bots 的 interface 注册进引擎消息 digest（保证引擎网络层可用），cluster 自身业务不使用 lib/network 的 MessageHandler 消息集。

---

## 4. 配置项与端口

配置段 `<cluster>`，解析实现见 `lib/server/serverconfig.cpp`（`readClusterConfig`）与默认值 `kbengine_defaults.xml`：

| XML 项 | 字段（ENGINE_COMPONENT_INFO） | 默认 | 说明 |
|---|---|---|---|
| `internalInterface` | `internalInterface` | 空 | 绑定网卡/IP，空自动选择 |
| `servicePort` | `clusterServicePort` | 20093 | 服务面 TCP |
| `peerPort` | `clusterPeerPort` | 20094 | 副本间 TCP |
| `addresses/item...` | `cluster_addresses`（vector<string>） | 空 | 成员 IP 表，顺序=成员序号；空=单成员自举 |
| `electionTimeout` | `clusterElectionTimeoutMS` | 3000 | 选举超时 ms（实际加抖动，见 §5.4） |
| `heartbeatInterval` | `clusterHeartbeatIntervalMS` | 1000 | leader 心跳间隔 ms |
| `componentHeartbeatInterval` | `clusterComponentHeartbeatInterval` | 5 | 组件续租间隔 **秒** |
| `componentLeaseSeconds` | `clusterComponentLeaseSeconds` | 15 | 组件租约时长 **秒** |

组件侧读配置统一走 `ServerConfig::getKCluster()`（`serverconfig.inl`）。

> 迁移语义：所有副本的 servicePort/peerPort/addresses 必须一致（同一集群一套配置）；每台机器只跑一个副本，占用同一组端口。
>
> 冒烟/测试用一套加速参数（`smoke/assets_*/res/server/kbengine.xml`）：`electionTimeout=1200`、`heartbeatInterval=400`、`componentHeartbeatInterval=2`、`componentLeaseSeconds=6`，三副本绑定 `127.0.0.1/2/3`，成员表 `[127.0.0.1,127.0.0.2,127.0.0.3]`。

---

## 5. ClusterCore 一致性核心

`class ClusterCore`（`cluster_core.h/.cpp`）实现：

- 自研极简 Raft 子集（选举/心跳/日志复制/majority 提交/快照追平）；
- 注册表纯内存状态机；
- servicePort/peerPort 两个 TCP 监听 + 网络线程；
- 本机代理执行指令的路由与聚合（调 `ClusterProcess`）。

### 5.1 线程模型与事件队列

**网络线程（每副本固定 3 个 + 每连接 1 个）：**

| 线程 | 职责 |
|---|---|
| `serviceAcceptorLoop` | accept servicePort 连接；为每连接 spawn `serviceConnThread` |
| `peerAcceptorLoop` | accept peerPort 连接；按源 IP 匹配成员序号（不在成员表则丢弃）；重复连接关闭 |
| `peerConnectorLoop` | 主动连接"成员序号大于自己"的对端（主动方=小序号端），~1s 重试 |
| `serviceConnThread(ep, connId)` | 读一条帧 → 入队 `EVENT_SERVICE_REQ` → 等主循环应答（限时 4000ms）→ 回帧 → 关连接。**短连接模型：一次请求一条连接** |
| `peerConnThread(ep, peerIdx)` | 持续读对端帧 → 入队 `EVENT_PEER_MSG`；断开时清 `connected` 并置 `needSnapshot` |

**状态机单线程语义：** 网络线程只做收发与投递；`ClusterCore` 的角色机、日志、注册表、计时器只在**主循环 `tick()`**（引擎主线程，100ms 帧）推进。队列与对端状态用互斥锁保护，其余状态机数据无锁。

**事件：**

```cpp
enum EventType { EVENT_SERVICE_REQ=1, EVENT_PEER_MSG=2 };
struct Event {
  uint8_t type;
  uint64 connId;   // EVENT_SERVICE_REQ 连接ID
  int     peerIdx; // EVENT_PEER_MSG 来源成员
  std::string data;
};
```

`tick()` 顺序（每 100ms 帧）：

1. `drainEvents()`：交换队列并逐个处理。
2. 若 `role_ != LEADER`：检查选举超时（→ candidate，§5.4）。
3. 若 `role_ == LEADER`：心跳间隔到 → 向所有成员 `sendReplicateToPeer()`。
4. 若 leader 且距上次 TTL 扫描 ≥ 1000ms → `ttlScan()`。
5. 定期快照/日志归档（≥ 60s 且日志 > 64 条且 commit 前进，§5.8）。

### 5.2 Raft 状态与角色机

```cpp
enum Role { ROLE_FOLLOWER=0, ROLE_CANDIDATE, ROLE_LEADER };

// 持久/易失状态（主循环独占）
Role   role_;          // 初值 FOLLOWER
uint64 currentTerm_;   // 初值 0
int    votedFor_;      // -1 未投
int    leaderIdx_;     // -1 未知
uint64 commitIndex_;   // 初值 0
uint64 lastApplied_;   // 初值 0
uint64 snapshotIndex_; // 初值 0（快照覆盖到的日志位置）
uint64 snapshotTerm_;  // 初值 0
std::vector<RaftLogEntry> log_;  // 覆盖 (snapshotIndex_, lastIndex()]
```

日志项：

```cpp
struct RaftLogEntry {
  uint64 index;    // 从 1 开始（全局序号 = snapshotIndex_ + log_ 内偏移）
  uint64 term;
  uint8  type;     // ClusterCore::CmdType
  std::string data;// Wire 编码的命令载荷（命令自带时间戳）
};
enum CmdType { CMD_REGISTER=1, CMD_RENEW=2, CMD_UNREGISTER=3, CMD_EXPIRE=4 };
```

计时器均为墙上时钟（`nowWallMS()`）。关键：

- `electionDeadlineMS_`：follower/candidate 的下一次超时；
- `lastHeartbeatMS_`：leader 上次发心跳时刻；
- `lastTTLScanMS_`、`lastSnapshotMS_`。

### 5.3 成员与链路管理

- `members_`（vector<string>）与 `selfIdx_` 由 `ClusterCore` 构造传入。
- `peers_[i]`（`PeerCtx`）：
  ```cpp
  struct PeerCtx {
    bool connected;
    bool outbound;          // 本方是否为主动连接方（成员序号小者）
    Network::EndPoint* ep;
    uint64 lastAcked;       // 该对端已确认的连续日志 index（leader 视角）
    bool needSnapshot;      // 需快照追平
  };
  ```
- 主动连接规则：**成员序号小者主动连接序号大者**，避免对连。序号相同不会发生（表内 IP 唯一、自检在 §3.2 完成）。
- 出站连接**显式 bind 到 selfIP_**（否则多回环/多网卡环境下源地址可能错误，导致对端按源 IP 识别成员错误→"重复连接"→链路抖动→选主乒乓）。bind 失败（地址不属于本机）时告警并退回 OS 选路。
- peer 连接按**对端源 IP** 与成员表匹配确定 `peerIdx`；不匹配的接入连接直接关闭。
- `sendFrameToPeer(idx,msg)` 加锁取 ep 后 `sendFrame`（u32 长度前缀）。发送失败交由读线程检测断开。

### 5.4 选举

触发：`role_ != LEADER` 且 `now >= electionDeadlineMS_`。

- 启动时（多成员）：`electionDeadlineMS_ = now + electionTimeoutMS_ + 500`，给已有 leader 留出至少 500ms 心跳窗口。
- 超时后：
  1. `role_ = CANDIDATE; currentTerm_ += 1; votedFor_ = selfIdx_; leaderIdx_ = -1;`
  2. `grantVotes_` 清空并计入自己；
  3. 重置超时：`jitter = electionTimeoutMS_*2/3 + 1; electionDeadlineMS_ = now + electionTimeoutMS_ + fastRand() % jitter;`（实际随机区间 `[T, T+2T/3)`）；
  4. 计算自己的"最新日志"：`myLastIdx = lastIndex()`；`myLastTerm`：若无日志项则 `snapshotTerm_`，否则 `log_.back().term`；
  5. 向所有其它成员广播 `PM_VOTE_REQ(term, selfIdx, myLastIdx, myLastTerm)`。

投票（`onVoteRequest`）：

- 若 `term > currentTerm_`：升 term、退回 follower、`votedFor_=-1`、清 `grantVotes_`。
- 授予条件：`term >= currentTerm_` 且（`votedFor_==-1` 或 `==candidateIdx`），且候选日志不旧于本机（`candLastTerm>myLastTerm || (== 且 candLastIdx>=myLastIdx)`）。
- 授予后 `votedFor_=candidateIdx` 并把 `electionDeadlineMS_` 顺延一个完整超时。
- 响应 `PM_VOTE_RESP(term, granted:0/1)`。

计票（`onVoteResponse`）：

- 响应 term 更高 → 退位 follower 并返回。
- 仅在 `role_==CANDIDATE && term==currentTerm_` 时统计；得票（含自己）`>= majority = members_/2 + 1` 即当选：
  - `role_=LEADER; leaderIdx_=selfIdx_; lastHeartbeatMS_=0; lastTTLScanMS_=now;`
  - 立即向所有成员 `sendReplicateToPeer()`（确立权威 + 立即复制）。

### 5.5 心跳与日志复制

Leader 每 `heartbeatIntervalMS_` 向每个成员 `sendReplicateToPeer(i)`。该函数决定"增量日志"或"快照"：

**快照条件**：`peers_[i].needSnapshot` 为真，或对端 `lastAcked < snapshotIndex_`，或需追平的位置早于已归档区。

- 发送 `PM_APPEND(kind=1)`，内含 `registrySnapshotBytes()`（无 magic，§13）与 leaderCommit。
- 对端 `onAppend`：`decodeSnapshot(snap)` → `saveSnapshot(snap)`（落盘加 magic）→ 回 `PM_APPEND_RESP(ack=1, snapshotIndex)`。

**增量日志**（含 count=0 的纯心跳）：

```
[type=PM_APPEND][term:u64][leader:selfIdx:u32][leaderCommit:u64][kind:u8=0]
[prevIndex:u64][prevTerm:u64][count:u16]
  count 条: [index:u64][term:u64][cmdType:u8][dataLen:u32][data]
```

- `fromIndex = lastAcked+1`；若已追平（fromIndex>last）则 `prevIndex=last`、count=0 纯心跳。
- `prevTerm`：`prevIndex > snapshotIndex_` 时取 `log_[prevIndex-snapshotIndex_-1].term`，否则取 `snapshotTerm_`。

对端 `onAppend(kind=0)` 校验：

1. `term < currentTerm_` → 拒（ack=0 + 自己的 lastIndex）。
2. `term > currentTerm_ || role != FOLLOWER` → 接受并退位 follower；更新 `leaderIdx_`；`electionDeadlineMS_` 顺延。
3. `prevIndex != lastIndex()`：
   - 若 `prevIndex` 在本机日志内且该项 term == prevTerm → 认为匹配（随后按 count 逐条 append）；
   - 否则回 ack=0（leader 侧把 `needSnapshot=true` → 下轮走快照）。
4. 逐条追加：空洞保护（`idx > lastIndex()+1` 则 ack=0）；重复项跳过；**同 index 不同 term 则截断本机日志尾部再追加**（日志冲突以 leader 为准）。
5. `leaderCommit > commitIndex_` → `commitIndex_ = min(leaderCommit, lastIndex())` → `applyCommitted()`。
6. 回 `PM_APPEND_RESP(term, ack=1, lastIndex())`。

Leader 收 `PM_APPEND_RESP`（`onAppendResponse`）：

- 更高 term → 退位 follower。
- ack=1 → `peers_[from].lastAcked=index; needSnapshot=false`；
- ack=0 → `needSnapshot=true`（下轮走快照重追）；
- 然后 `advanceCommit()`。

### 5.6 提交与 apply

`advanceCommit()`（仅 leader）：

- `majority = members_/2 + 1`；
- 从 `commitIndex_+1` 起，只要"leader 自己 + 已 connected 且 lastAcked>=idx 的对端" ≥ majority 就推进 `commitIndex_ = idx`，并**完成等待该日志的客户端连接**：`cmdWaiter_[idx]` → 应答 `MSG_RESP_OK`。
- 每轮循环结束后 `applyCommitted()` 并 `notify_all`。

`appendLog(type,payload)`（仅 leader 调用）：

1. 构造 `index = lastIndex()+1; term = currentTerm_` 并 push；
2. 立即向所有成员复制；
3. 立即 `advanceCommit()`（单成员即刻提交）。

`registerWaiter(idx, connId)`：把 `(idx → connId)` 记入 `cmdWaiter_`，`(connId → idx)` 记入 `revWaiter_`；提交时删除并写应答。

`applyCommitted()`：把 `(lastApplied_, commitIndex_]` 的日志逐条 `applyEntry` 到状态机（跳过已归档部分）。

### 5.7 TTL 扫描

仅 leader，每秒执行 `ttlScan()`：

- 遍历注册表，收集 `lastRenewal > 0 && now - lastRenewal >= componentLeaseMS_` 的条目；
- 对每个过期条目把 `(uid,type,cid)` 编码为 `CMD_EXPIRE` 日志项 append（**到期下线也走日志复制**，保证各副本一致）。

### 5.8 快照与日志归档

每 `tick()` 检查（leader/follower 都做本机压缩）：

```
now - lastSnapshotMS_ >= 60000ms
&& log_.size() > 64
&& commitIndex_ > snapshotIndex_
```

- `newSnap = commitIndex_`；`drop = min(newSnap - snapshotIndex_, log_.size())`；
- `snapshotTerm_ = log_[drop-1].term`（有可归档项时）；
- `snapshotIndex_ = newSnap`；`saveSnapshot()` 落盘；
- 落盘后移除日志前 drop 条（`log_.clear()` 或 `erase`）。

落盘文件名：`cluster_state_<selfIdx_>.bin`（写入**副本工作目录**）。启动时 `loadSnapshotFromDisk()` 恢复。

---

## 6. 服务面协议 cluster_interface

`kbe/src/server/cluster/cluster_interface.h`：纯头文件、自包含（不依赖 lib/network），服务端与组件侧（`components.cpp`）、guiconsole（`tools/guiconsole/ClusterClient`）共用。

### 6.1 帧格式与连接模型

```
请求/响应帧（服务面）:
┌───────────────┬──────────┬──────────┐
│ u32 length(BE)│ u8 type  │ payload  │
└───────────────┴──────────┴──────────┘
```

- **一次交互一条短连接**：客户端连接副本 servicePort → 发一条帧 → 收一条应答帧 → 关闭。
- 字节序统一网络序（大端）；字符串 = `u32 长度前缀 + UTF-8 原始字节`。
- 请求超时：服务连接线程等待应答上限 **4000ms**；组件侧每次 `clusterRoundTrip` 超时 600/400ms 由调用方指定。
- 协议版本：`PROTOCOL_VERSION = 1`（`encodeComponentData`/`encodeServerCommand` 头字节，解码时校验）。

### 6.2 消息总表

| 类型 | 值 | 方向 | 说明 |
|---|---|---|---|
| `MSG_QUERY_LEADER` | 0x01 | 客户端→任意副本 | 载荷无；应答 LEADER 或 NOT_LEADER（follower 也答，无需 leader） |
| `MSG_REGISTER` | 0x02 | →leader | 载荷 = encodeComponentData；应答 OK/冲突/重定向 |
| `MSG_RENEW` | 0x03 | →leader | uid,type,cid,pid；OK / NOT_FOUND / 重定向 |
| `MSG_UNREGISTER` | 0x04 | →leader | uid,type,cid；OK / 重定向 |
| `MSG_FIND` | 0x05 | →leader | uid,findType,findCid(0=按类型全找)；ENTRY / ENTRY_LIST / NOT_FOUND |
| `MSG_QUERY_ALL` | 0x06 | →leader | uid；ENTRY_LIST |
| `MSG_START_SERVER` | 0x07 | →leader | encodeServerCommand；CMD_RESULT |
| `MSG_STOP_SERVER` | 0x08 | →leader | encodeServerCommand；CMD_RESULT |
| `MSG_KILL_SERVER` | 0x09 | →leader | encodeServerCommand；CMD_RESULT |
| `MSG_PROXY_START_SERVER` | 0x0A | leader→目标副本 | 本机代理指令（localOnly），副本**不校验角色、不重定向** |
| `MSG_PROXY_STOP_SERVER` | 0x0B | leader→目标副本 | 同上 |
| `MSG_PROXY_KILL_SERVER` | 0x0C | leader→目标副本 | 同上 |
| `MSG_RESP_LEADER` | 0x81 | 副本→客户端 | `u32 leaderIP, u16 leaderServicePort` |
| `MSG_RESP_NOT_LEADER` | 0x82 | 副本→客户端 | `u32 leaderIP, u16 leaderServicePort`（未知时 0） |
| `MSG_RESP_OK` | 0x83 | | 载荷无 |
| `MSG_RESP_NOT_FOUND` | 0x84 | | 载荷无 |
| `MSG_RESP_IDENTITY_CONFLICT` | 0x85 | | 载荷 = encodeComponentData（已存在进程完整信息） |
| `MSG_RESP_ENTRY` | 0x86 | | 载荷 = encodeComponentData |
| `MSG_RESP_ENTRY_LIST` | 0x87 | | `u16 count` + count×encodeComponentData |
| `MSG_RESP_CMD_RESULT` | 0x88 | | `i32 code`(0 成功/<0 失败) + `string message` |
| `MSG_RESP_ERROR` | 0x89 | | `string message` |

> 载荷不带版本字节；`encodeComponentData/encodeServerCommand` 的**内容首字节是 PROTOCOL_VERSION**（仅作为这两种载荷的版本头），解码时校验。

### 6.3 Wire 序列化

`ClusterInterface::Wire`：`put/get U8,I8,U16,U32,I32,U64,Float,String`。所有多字节为网络序。解析函数在越界/版本不符时返回 false（调用方静默丢弃该请求）。

### 6.4 ComponentData 字段序

与 machine `onBroadcastInterface` 25 字段一一对应（encode/decode 对称）：

```
u8  ver(==1)
u32 uid
str username
u32 componentType
u64 componentID
u64 componentIDEx
u32 globalOrder
u32 groupOrder
u16 gus
u32 intaddr(网络序 IP)
u16 intport
u32 extaddr
u16 extport
str extaddrEx
u32 pid
f32 cpu
f32 mem
u32 usedmem
i8  state
u32 machineID
u64 extradata[4]
```

结构体语义（默认值）：`uid=0, componentType=-1, componentID=0, gus=0, state=0, machineID=0, extradata 清零`。

### 6.5 ServerCommandData 字段序

```
u8  ver(==1)
u32 uid
str username
u32 componentType
u64 cid           // 0 = 按类型解析目标
u16 gus
str targetHost    // 目标主机 IP；空 = 依据注册表/cid 定位
str rootPath      // KBE_ROOT
str resPath       // KBE_RES_PATH
str binPath       // KBE_BIN_PATH
```

> 该结构与原 machine startserver/stopserver/killserver 输入语义字段级兼容。

### 6.6 各消息载荷定义

| 消息 | 载荷布局 |
|---|---|
| MSG_QUERY_LEADER | 无 |
| MSG_REGISTER | `encodeComponentData` |
| MSG_RENEW | `i32 uid, i32 type, u64 cid, u32 pid`（注意 RENEW 载荷**不带**组件全量信息，服务端解析直接按此偏移） |
| MSG_UNREGISTER | `i32 uid, i32 type, u64 cid` |
| MSG_FIND | `i32 uid, i32 findType, u64 findCid` |
| MSG_QUERY_ALL | `i32 uid` |
| MSG_START/STOP/KILL_SERVER | `encodeServerCommand` |
| MSG_PROXY_* | `encodeServerCommand`（type 被替换为 0x0A-0x0C） |

### 6.7 服务请求处理流程

`handleServiceRequest(ev)`：

1. 取首字节 `mt`。
2. `mt == MSG_QUERY_LEADER`：leader → `MSG_RESP_LEADER(selfIP, servicePort)`；非 leader → `MSG_RESP_NOT_LEADER(leaderIdx≥0 ? members_[leaderIdx] : 0, ...)`。返回。
3. `mt` 为 MSG_PROXY_*：`baseOp = mt - (0x0A-0x07)`（映射回 MSG_START/STOP/KILL），`handleCtlCommand(..., localOnly=true)` 后返回。
4. 其余消息，若 `role_ != LEADER` → 统一 `MSG_RESP_NOT_LEADER`（含 leader 地址；未知 leader 时全 0，客户端应重试而非跟随 0.0.0.0）。返回。
5. 按 `mt` 分派（见 §8 状态机与 §9 指令处理）。

注册/续租/注销这类写操作走"appendLog + registerWaiter"：客户端连接**挂起等待 majority 提交**，提交成功由 `advanceCommit` 应答 `MSG_RESP_OK`，超过 4s 无应答则连接被关闭。

---

## 7. 副本间协议 PeerProto

定义在 `cluster_core.cpp`（仅头文件内部的 anonymous 空间，不导出）：

```cpp
enum MsgType {
  PM_VOTE_REQ      = 0x01,
  PM_VOTE_RESP     = 0x02,
  PM_APPEND        = 0x03, // kind=0 增量日志 / kind=1 快照
  PM_APPEND_RESP   = 0x04
};
```

### 7.1 帧格式

```
帧 = [u32 length(BE)][payload]
payload = [u8 消息类型][...]
```

- 单帧上限 `FRAME_MAX = 1MB`；长度非法/为空直接断开。
- 连接为**成员间长连接**（非短连接），持续双向读写。

### 7.2 消息布局

**PM_VOTE_REQ**

```
u8 type(0x01)
u64 term
i32 candidateIdx
u64 candLastIdx
u64 candLastTerm
```

**PM_VOTE_RESP**

```
u8 type(0x02)
u64 term
u8 granted(0/1)
```

**PM_APPEND（增量 kind=0）**

```
u8 type(0x03)
u64 term
u32 leader(selfIdx)
u64 leaderCommit
u8 kind(0)
u64 prevIndex
u64 prevTerm
u16 count
  每项: u64 index | u64 term | u8 cmdType | u32 dataLen | data
```

**PM_APPEND（快照 kind=1）**

```
u8 type(0x03)
u64 term
u32 leader(selfIdx)
u64 leaderCommit
u8 kind(1)
u32 snapLen
snapshot bytes (registrySnapshotBytes，无 magic)
```

**PM_APPEND_RESP**

```
u8 type(0x04)
u64 term
u8 ack(0/1)
u64 index        // ack=1: lastAcked(index 或 snapshotIndex)；ack=0: 本机 lastIndex
```

### 7.3 快照同步

- 触发条件见 §5.5。
- follower `decodeSnapshot` 后落盘 `saveSnapshot(snap)`（自动补 magic 头），回 ack。
- **a) 副本重启**：leader 对端 `connected=false`、重连后 `needSnapshot=true`（peerConnThread 断开时设置）或根据 `lastAcked=0 < snapshotIndex_` 判快照；**b) 日志缺口**：append 匹配失败 ack=0 → leader 置 `needSnapshot` → 下轮全量快照。
- 快照恢复将本机 registry/log 整体替换（`lastApplied_ = commitIndex`），确保追平。

---

## 8. 注册表状态机

### 8.1 数据模型与 key

```cpp
struct RegistryEntry {
  ClusterInterface::ComponentData data;
  uint64 lastRenewal;   // 最近一次注册/续租时间（leader 墙上时钟 epoch ms）
};
std::map<std::string, RegistryEntry> registry_;   // key 见下
```

key = `encodeKey(uid:int32, type:int32, cid:uint64)` = `u32 uid + u32 type + u64 cid`（16 字节二进制），即 **(uid, componentType, componentID) 全局唯一**。

查询为线性扫描当前规模（文档与实现一致：规模几十~几百条，无二级索引）。

### 8.2 applyEntry 语义（日志命令 → 状态机）

| 命令 | 载荷布局（日志项内 data） | 状态机动作 |
|---|---|---|
| `CMD_REGISTER` | `u64 time + encodeComponentData` | 写 `registry_[key] = {comp, lastRenewal=time}` |
| `CMD_RENEW` | `u64 time + u32 uid + u32 type + u64 cid + u32 pid` | 存在则更新 `data.pid=pid, lastRenewal=time`；不存在忽略（该情形在 leader 处理期已被 MSG_RESP_NOT_FOUND 拦截） |
| `CMD_UNREGISTER` | `u32 uid + u32 type + u64 cid` | `registry_.erase(key)` |
| `CMD_EXPIRE` | 同 UNREGISTER | `registry_.erase(key)` |

> 注意编码差异：日志内 CMD_RENEW/UNREGISTER/EXPIRE 的 uid/type 用 `u32`，而**服务面消息**（MSG_RENEW/MSG_UNREGISTER/MSG_FIND）的 uid/type 用 `i32`——两者位宽相同、网络序相同，互操作一致；命令在 **leader 处理期**负责把服务面载荷转成日志载荷。

### 8.3 注册冲突检测（leader 处理 MSG_REGISTER 时）

1. `key = encodeKey(uid, type, cid)`；查表。
2. 若存在旧条目：
   - `expired = (now - old.lastRenewal >= componentLeaseMS_) && old.lastRenewal > 0`；
   - **冲突**：`!expired && old.pid != 新进程 pid` → 应答 `MSG_RESP_IDENTITY_CONFLICT`（载荷=已存在组件 encodeComponentData）并 return（不写日志）。
   - 过期残留 / 同 pid（同进程重复注册）→ 允许覆盖（写日志更新 lastRenewal 与全量字段）。
3. 通过则把 `(now + comp)` 编码 append 日志，等待提交。

> 组件侧对应行为（components.cpp）：收到 IDENTITY_CONFLICT 时若冲突体 `machineID == 本机 macMD5` 且其 pid 已不存在（崩溃残留），则等待租约过期后自动重试；否则视为身份非法退出（`onIdentityillegal`）。

### 8.4 TTL 租约

- 注册成功即获得租约，租期 `componentLeaseSeconds`（默认 15s，测试 6s）。
- 组件每 `componentHeartbeatInterval`（默认 5s）向 leader Renew；Renew 本身是一条复制日志，leader 提交后更新 lastRenewal。
- 过期条目由 leader `ttlScan`（每秒）产出 `CMD_EXPIRE` 日志，随复制在**所有副本**统一移除。
- 优雅退出发 `MSG_UNREGISTER`，不等待 TTL。

---

## 9. 远程起停指令处理 handleCtlCommand

入口：leader 收到 `MSG_START/STOP/KILL_SERVER`（`handleCtlCommand(ev, op, off, localOnly=false)`）；或任意副本收到 `MSG_PROXY_*`（`localOnly=true`）。

### 9.1 通用校验

- 解码失败 → `CMD_RESULT(code=-1, "invalid server command payload")`。
- `componentValid(type)`：仅允许真正的服务组件——`type>UNKNOWN && type<COMPONENT_END_TYPE`，且**排除** CLIENT/MACHINE/CONSOLE/TOOL/CLUSTER 类型。不合法 → `code=-1, "invalid componentType: N"`。
- 统一应答为 `MSG_RESP_CMD_RESULT`：`i32 code(0/<0) + string message`。

### 9.2 START 语义

1. 目标主机解析：
   - `cmd.targetHost` 非空且非 localOnly：按地址匹配成员序号（非成员 → 报错 `targetHost xxx is not a cluster member`）；
   - 默认 `execHost = selfIdx_`（leader 本机）。
   - localOnly 强制 `execHost = selfIdx_`。
2. `execHost != selfIdx_` → **转发给目标副本**：
   - 把请求帧首字节改成 `MSG_PROXY_START_SERVER`，经 `clusterCtlRequest`（短连接、超时 3000ms）发给 `memberIP(execHost):servicePort`；
   - 若目标副本不可达 → `code=-1, "start X: member ip unreachable"`；
   - 解析目标副本的 `CMD_RESULT` 原样回报。
3. `execHost == selfIdx_`（本机代理）→ `ClusterProcess::startProcess(uid, type, cid, gus, err)`：
   - 成功 → `code=0, "start <name> ok"`；失败 → `code=-1, "start <name> failed: <err>"`。

> `cid=0` 时组件进程侧按本地确定性规则自生成（§12）；`--gus` 显式透传。

### 9.3 STOP / KILL 语义

`killOp = (op == MSG_KILL_SERVER)`。

1. 解析目标主机（同 9.2；`targetHost` 非空 → 指令只作用于该主机上的注册条目，保持 guiconsole 按布局逐主机停服的旧语义）。
2. 遍历注册表匹配 `uid`、`componentType`（cid≠0 再匹配 cid），得到 `matched`：
   - 主机判定：`memberIndexByAddr(comp.intaddr)`；解析不到 → 告警并按本机处理。
   - `targetIdx>=0 && host!=targetIdx` → 跳过；
   - 本机 → 收进 `localTargets`（pid/intaddr/intport）；
   - 其它主机 → 记 `remoteHosts[host]`；localOnly（代理执行）时跳过不属于本机的条目（leader 只把本机条目转给本副本）。
3. 无匹配 → `code=0, "no running X component found (nothing to do)"`（幂等）。仅"集群有同类型但目标主机无实例" → `code=0` 对应提示。
4. 本机部分：`killOp ? ClusterProcess::killProcess : ClusterProcess::stopProcess`；失败记 `code=-1`，detail 追加 `local:<err>`。
5. 其它主机逐个转发 `MSG_PROXY_STOP/KILL_SERVER`（超时 4000ms/主机），聚合各主机结果；任一失败则整体 `code=-1`。
6. 应答 `CMD_RESULT(code, "local:stopped host1:stopped host2:killed ...")`。

---

## 10. 本机代理执行器 ClusterProcess

`cluster_process.h/.cpp`：纯本机进程控制，不依赖组件网络协议；**在副本主循环内调用，操作有耗时上界**。

### 10.1 startProcess

- 类型越界（`<0 || >= COMPONENT_END_TYPE`）→ false。
- **Linux**（fork+exec）：
  - 环境：父进程必须已导出 `KBE_ROOT/KBE_RES_PATH/KBE_BIN_PATH`（缺任一 → 明确报错）；
  - 命令行 = `binPath + COMPONENT_NAME_EX(type)`，argv = `[bin, --cid=, --gus=, NULL]`；fork 前 setenv 三个 KBE 变量（仅影响子进程继承）；
  - 子进程内仅 async-signal-safe：`setuid(uid)`（uid>0，失败写 errno 到管道退出）、`execv`（失败写 errno 退出）；
  - **exec 成败探测**：CLOEXEC 管道——子进程 exec 成功写端随 exec 关闭（父进程读到 EOF），失败则子进程写入 errno 字节；父进程 `select` 上限 1s；超时按"已拉起"处理。
- **Windows**（CreateProcessW）：`exePath = binPath + name + ".exe"`，命令行 `"<exe>" --cid= --gus=`，`CREATE_NEW_CONSOLE`，工作目录取当前目录；成功后关句柄返回 pid。`KBE_BIN_PATH` 缺失 → 报错。

### 10.2 stopProcess（优雅停止）

- 每个 target：pid 不存在则跳过（幂等）；存在则：
  - Linux：`kill(pid, SIGINT)`（引擎 ServerApp 将 SIGINT 注册为优雅退出，等价原 reqCloseServer）；
  - Windows：`taskkill /t /pid N`（不带 /f）。
- 不等待进程退出（与原 machine 发指令即返回一致）。

### 10.3 killProcess（强制杀）

- 每个 target：pid 不存在则跳过；否则循环 ≤15 次（每次 `SIGKILL`/`taskkill /f /t` + 最多 10×100ms 轮询 `processExists`），窗口约 1.5s；仍存活 → false + timeout 错误。

> `processExists`：Linux `kill(pid,0)`（EPERM 视为存在）；Windows `OpenProcess + GetExitCodeProcess`（STILL_ACTIVE 或查询失败视为存在）。

---

## 11. 组件侧客户端 lib/server/components.cpp

### 11.1 clusterRequest 重试与重定向

`clusterRequest(msg, resp, timeoutMS)`（匿名命名空间）：

1. 读 `<cluster>`：`servicePort` + `cluster_addresses`（空则回退 `127.0.0.1`）。
2. 逐地址 `clusterRoundTrip`（EndPoint 短连接：connect 需 `htons(port)`，非阻塞 + select，首连超时 timeoutMS；收 4 字节长度 + 载荷，载荷上限 8MB）。
3. 每地址最多跟随 **4 次重定向**：
   - `MSG_RESP_NOT_LEADER` → 解析载荷内 `(leaderIP, leaderPort)`；IP=0（无 leader）→ 返回 -1 让上层重试；否则换连 leader 重发原消息。
4. 连接失败换下一地址；全部失败返回 -1。

### 11.2 注册状态机（Components::process）

- machine/cluster 组件自身**不注册不发现**。
- `state_==0`（未注册）：每 500ms 尝试注册一次（`clusterLastRegisterMS` 限频），构造 `ComponentData`（uid/username/type/cid/cidEx/globalOrder/groupOrder/gus/int/ext 地址/pid/cpu/mem/usedmem/state/machineID=getMacMD5()/extradata[1..4]），发 `MSG_REGISTER`：
  - `MSG_RESP_OK` → 置 `clusterRegistered=true`、`state_=1`；
  - `MSG_RESP_IDENTITY_CONFLICT` → §8.3 所述处理；
  - cluster 不可达/选举中 → 半秒重试（不 panic）。
- 注册成功才允许发现其它组件（保证引导时序）。

### 11.3 续租与发现

- 每 1s 尝试 `findComponents()`（引导目标组件，`findComponentTypes_` 定义见构造函数注释，逻辑沿用原 Components 轮询语义）；成功后 `state_=2`。
- **续租独立于发现进度**：已注册且距上次续租 ≥ `componentHeartbeatInterval` 秒，发 `MSG_RENEW(uid,type,cid,pid)`（超时 400ms）；收到 `MSG_RESP_NOT_FOUND` → 自身注册丢失（leader 切换/快照恢复后），回 `state_=0` 重新注册。

### 11.4 优雅注销

`Components::finalise()`：若已注册，发 `MSG_UNREGISTER(uid,type,cid)`（超时 800ms，不需应答）后置未注册。

---

## 12. 组件 ID 生成（kbemain.h）

`checkComponentID(componentType)`（不再走 machine/IDComponentQuerier 中央分配）：

```cpp
if (g_componentID == (COMPONENT_ID)-1)   // 未显式 --cid
{
    int macMD5 = getMacMD5();            // 本机 MAC 摘要
    g_componentID = (uid * COMPONENT_ID_MULTIPLE)
                  + (macMD5 * 10000)
                  + (componentType * 100)
                  + 1;
}
```

- `--cid=` 解析见 `parseCommandArgs`（非 --cid 参数出现会重置为 -1，要求 --cid 在前）。
- 多开同机同类型：靠 `--cid` / `--gus` 显式区分；重复身份由 cluster 注册冲突检测兜底（§8.3）。
- `MACHINE_TYPE/CLUSTER_TYPE/LOGGER_TYPE` 亦本地生成。

---

## 13. 快照文件格式

**文件名**：`cluster_state_<selfIdx_>.bin`（副本工作目录）。

**磁盘格式**：

```
[u32 magic = 0x4B42E533]
[registrySnapshotBytes ...]
```

`registrySnapshotBytes()` 布局：

```
u64 snapshotIndex_
u64 snapshotTerm_
u64 commitIndex_
u64 currentTerm_
u32 count
  每项: str key | str encodeComponentData | u64 lastRenewal
```

（`str = u32 len + 原始字节`。）

**写入路径**：

- 定期归档：`saveSnapshot()` = magic + `registrySnapshotBytes()`（基于当前 registry/snapshotIndex），随后截断日志；
- 收到 leader 快照：`decodeSnapshot(snap)`（snap 无 magic）→ `saveSnapshot(snap)` 补 magic 落盘。

**启动恢复**：`loadSnapshotFromDisk()` 读文件 → 校验 magic → 恢复。

> 落盘前请确保工作目录可写；日志提示 `saved cluster_state_N.bin registry=.. snapshotIndex=..`。

---

## 14. 单成员模式

`<addresses>` 为空（默认配置）→ `members = [selfIP]`，`start()` 直接置 `role_=LEADER, leaderIdx_=0`，**不选举**。appendLog 后立即 `advanceCommit`（majority=1）即刻提交应答。

单成员 = "高可用注册中心"的退化形态：无容错、无故障切换，适合单机开发/验证。

---

## 15. 关键数值速查

| 数值/常量 | 默认 | 位置 |
|---|---|---|
| 组件类型 CLUSTER_TYPE / END | 15 / 16 | lib/common/common.h |
| servicePort / peerPort | 20093 / 20094 | serverconfig.h、kbengine_defaults.xml、cluster_interface.h(共享默认) |
| PROTOCOL_VERSION | 1 | cluster_interface.h |
| 帧上限（服务/对端/代理） | 1MB / 组件侧 8MB | cluster_core.cpp FRAME_MAX、components.cpp |
| 服务连接等待应答上限 | 4000ms | cluster_core.cpp serviceConnThread |
| 代理转发超时（start/stop-kill） | 3000ms / 4000ms | handleCtlCommand |
| 游戏帧 | 100ms tick | cluster.cpp |
| 选举超时（默认） | 3000ms + [0, 2/3·T) 抖动 | cluster_core.cpp |
| 心跳间隔 | 1000ms | 同上 |
| TTL 扫描间隔 | 1000ms | 同上 |
| 快照周期/触发 | ≥60s 且日志>64 且 commit>snapshot | 同上 |
| 组件续租间隔 / 租约 | 5s / 15s（<cluster> 可配） | serverconfig.h |
| 单帧最大 select 等待 | 见函数注释 | — |

---

## 16. 已知限制与约定

1. **成员变更需停机**：不支持动态增删成员（无 joint consensus）；改 `<addresses>` + 重启全部副本。
2. **读也走 leader**：未实现 lease-read；follower 收到 FIND/QUERY_ALL 一律重定向。
3. **机器负载/资源监控不承接**：依赖 machine `queryLoad/onQueryMachines/lookApp` 的 guiconsole 视图在无 machine 后失效。
4. **注册信息为内存 + 定期快照**：极端情况下（leader 与所有副本同时丢失且快照过期）会丢已提交但未落快照的注册记录；组件侧靠"续租 NOT_FOUND → 重新注册"自愈。
5. **组件引导超时语义保持原样**：cluster 未就绪时组件持续重试、不 panic，等待先启动 cluster。
6. 网络分区下失去 majority 的副本不提供任何服务（含读），term 机制保证不双主；分区恢复后靠日志/快照收敛。
