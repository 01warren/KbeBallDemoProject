# Loginapp 接入 Router —— 用例骨架（L 段，SKELETON）

> 状态：骨架。本轮**不实现断言**，只固化 ID、场景与判定标准，
> 待 loginapp 真正接入 Router 后按本文档补实现。

## 目标

登录期把"客户端该连谁"从 baseapp 地址改为 Router 地址，并在 Router 上完成
client proxy Actor 的绑定前置。

## 用例

### L01 登录应答返回 Router 地址

- 层：L4
- 前置：完整集群 + Router 已启动；客户端走 `loginapp`
- 步骤：客户端发起登录 → 观察应答中的接入地址
- 判定标准：应答携带 `clientPort`(默认 20095) 而非 baseapp 外部端口；`<router>` 配置覆盖生效
- 状态：SKELETON
- 实现位置：`server/loginapp/*`（待接入）

### L02 Router 不可达时的降级

- 层：L4
- 前置：停掉 router 进程
- 步骤：客户端发起登录
- 判定标准：返回明确错误码并记日志；**不得**静默返回旧 baseapp 地址（避免半迁移状态）
- 状态：SKELETON

### L03 重登录时 proxyId 重新绑定

- 层：L4
- 前置：同一账号先后两次登录
- 步骤：第一次登录 → 踢线 → 第二次登录
- 判定标准：Router 上 `MAILBOX_ENTITY{proxyId, CLIENT_TYPE}` 指向**新连接**；旧映射不残留
  （与 `Baseapp::bindClientProxyActor` 的"先注销再注册"一致）
- 状态：SKELETON

### L04 多 router 地址列表故障转移

- 层：L4
- 前置：`<router><addresses>` 配多个，第一个不可达
- 步骤：登录
- 判定标准：按序尝试直到成功；失败项有日志
- 状态：SKELETON
