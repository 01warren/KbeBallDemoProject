# Dbmgr 接入 Router —— 用例骨架（D 段，SKELETON）

> 状态：骨架。本轮**不实现断言**，只固化 ID、场景与判定标准。

## 目标

dbmgr 作为无状态服务提供者接入 Router（注册 Service 或组件 Actor），
使 baseapp/cellapp 通过 Router 寻址它，而不是依赖 `Components` 直连。

## 用例

### D01 dbmgr 组件 Actor 注册

- 层：L3（真实 router 进程）/ L4
- 前置：router + dbmgr 启动
- 步骤：dbmgr 连 Router → 查询 `componentMailbox(dbmgrID, DBMGR_TYPE)`
- 判定标准：注册成功；断连重连后自动重建（对齐 `onRouterReady` 语义）
- 状态：SKELETON
- 实现位置：`server/dbmgr/*`（待接入）

### D02 写库请求的投递与失败可见

- 层：L4
- 前置：dbmgr 已注册
- 步骤：baseapp 发起写库 → 观察回包
- 判定标准：写库成功并回调；若目标不可达，发送方收到失败回报（不静默丢）
- 状态：SKELETON

### D03 直连通道下线后的等价性（P5 门禁）

- 层：L4
- 前置：Router 模式打开且 `Components` 直连已下线
- 步骤：跑一遍 dbmgr 相关回归（建号/写库/查库）
- 判定标准：与下线前行为一致；无 `TARGET_NOT_FOUND` 爆发
- 状态：SKELETON

## 备注

dbmgr 无实体 Actor，主要是**组件 Actor + Service** 寻址。
若最终按 Service 名寻址，则 D01 应改为 `findService("dbmgr")` 的注册/发现断言。
