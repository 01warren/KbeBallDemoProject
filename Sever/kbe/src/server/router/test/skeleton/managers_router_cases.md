# baseappmgr / cellappmgr 接入 Router —— 用例骨架（BM / CM 段，SKELETON）

> 状态：骨架。本轮**不实现断言**，只固化 ID、场景与判定标准。

## 目标

两个 mgr 负责组件发现与负载均衡。接入 Router 后，组件启动期的注册/发现
（设计文档 §10 明确"启动期注册/发现仍走 cluster"）与运行期寻址需要划清边界，
本段用例用于固化这条边界。

## 共用前置

完整集群（cluster + router + mgr + baseapp/cellapp + dbmgr）。

## BM 段（baseappmgr）

### BM01 运行期寻址走 Router

- 层：L4
- 步骤：baseapp 需要找 baseappmgr 时观察通道
- 判定标准：Router 模式下经 Router 投递；关闭开关时走原直连（逐字节等价）
- 状态：SKELETON

### BM02 启动期发现仍走 cluster

- 层：L4
- 判定标准：组件启动/上线阶段仍使用 cluster 的注册与发现（**本阶段不改**）
- 状态：SKELETON

### BM03 mgr 重启后的重建

- 层：L4
- 步骤：kill baseappmgr → 重启
- 判定标准：Router 上 mgr 的组件 Actor 重新注册；其它组件的地址解析恢复
- 状态：SKELETON

## CM 段（cellappmgr）

### CM01 运行期寻址走 Router

- 层：L4 · 状态：SKELETON（同 BM01，对象换成 cellappmgr）

### CM02 负载均衡请求不丢

- 层：L4
- 步骤：连续创建实体触发负载均衡
- 判定标准：分配请求全部得到应答；无超时与静默丢弃
- 状态：SKELETON

### CM03 与 cellapp 迁移的时序

- 层：L4
- 步骤：迁移期间触发负载统计
- 判定标准：迁移与统计互不阻塞；实体 Actor 的 SUSPEND/RESUME 不受影响
- 状态：SKELETON
