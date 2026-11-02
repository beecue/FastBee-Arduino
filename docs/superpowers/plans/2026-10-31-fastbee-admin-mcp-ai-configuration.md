# FastBee Admin MCP 与 AI 设备配置实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `fastbee-admin` 仓库交付可独立部署的 MCP 服务和 Vue 配置/监测后台，使用户通过 AI 聊天完成文档辅助的外设、外设执行、规则脚本、Modbus RTU、MQTT、设备基础信息及功能配置，并以可审计的预检、确认、下发、回读和回滚保证设备安全。

**Architecture:** MCP 后端作为 `fastbee-admin/springboot/fastbee-ai-mcp` 独立 Java 21 子模块运行，不强迫现有 Java 8 / Spring Boot 2.5.14 主服务整体升级；现有后台提供登录、权限和 Vue 管理网关。云端不直接访问设备局域网地址，也不复用语义不完整的 MQTT 下行作为配置事务，而由已登录的设备浏览器连接器在 MCP 服务与同源固件 API 之间转发白名单任务。AI 模型和 BYOK Key 仍由 AI 助手侧管理，MCP 服务不保存模型 Key、不代替模型决策；模型只能通过类型化工具编辑配置草案，必须经设备真实预检和人类确认后才能应用。

**Tech Stack:** 现有主服务 Java 8、Spring Boot 2.5.14、Spring Security、MyBatis-Plus、Redis、MySQL；新增 MCP 模块 Java 21、Spring Boot 4.x、Spring AI 2.0.x、WebMVC `STREAMABLE`、Spring Security OAuth2 Resource Server，并在隔离模块内沿用现有 Controller/Service/Mapper、MyBatis-Plus 和返回结构习惯；MySQL 5.7+、Redis 7、受控文件/S3 兼容对象存储；文档 worker 使用 Python 3.12、FastAPI、pdfplumber、python-docx、LibreOffice、Pillow、Tesseract、openpyxl；Vue 3、TypeScript、Vite、Element Plus、Pinia；设备侧沿用 C++、Arduino、PlatformIO 和现有 Host/Playwright 测试。

**Spec:** 本文件第 1～9 节为执行规范，取代 `docs/superpowers/plans/2026-10-25-fastbee-mcp-cloud-service.md` 的独立新仓库、PostgreSQL、只读/简单输出控制及“文档不生成配置”范围。设备本地 AI 基础约束来自 `docs/superpowers/plans/2026-10-25-fastbee-device-ai-assistant.md`，本计划 Task 11 对其 MCP 模式做配套扩展；旧文件当前若有未提交修改，实施时必须先保留并重新核对，不得覆盖。

## Global Constraints

- 路径前缀：`ADMIN/` 表示 `D:/project/gitlab/fastbee-admin/`，`DEVICE/` 表示 `D:/project/gitee/FastBee-Arduino/`。两个仓库分别检查 `AGENTS.md` 和 `git status --short`，保留无关改动，未经明确授权不提交、不部署、不操作真实设备。
- MCP 代码必须留在 `fastbee-admin`：`springboot/` 是全部后端代码边界，`vue/` 是管理前端边界；固件仓库只实现设备配置契约、浏览器连接器、设备 AI 工具循环及相应测试，不创建独立 `fastbee-ai-mcp` 仓库。
- 现有 `license-server` 保持 Java 8；`fastbee-ai-mcp` 单独使用 Java 21 和独立进程/容器，不把 Spring Boot 4、Jakarta Servlet 或新 Spring Security 依赖传递给旧模块。Spring AI 2.0.x 的具体稳定补丁版、MCP SDK 协议版本和兼容 BOM 在 Task 1 锁定，禁止 SNAPSHOT 和动态版本。
- **不得影响既有功能：** 新 MCP 模块使用独立 POM，不加入旧 Maven reactor，也不要求旧工程修改 `springboot/pom.xml`；不修改旧模块依赖管理、全局 SecurityFilterChain、CORS、HTTP 拦截器、端口、数据源配置或既有表结构。旧后台只增加默认关闭的 `/iot/mcp/**` 网关和按权限显示的懒加载菜单，MCP 未部署、未启用或故障时，license、工单、系统管理等现有 API/页面保持原行为。
- 数据库只新增 `mcp_` 前缀表和独立升级 SQL，不修改已发布的 `fastbee-v2.5.sql` 基线；Vue 只增加按需加载路由/模块，不改现有全局请求与响应语义；Docker 使用独立 compose project、容器名、端口和卷。发布必须同时得到旧功能回归证据和 MCP 新功能证据，停止 MCP 后旧系统应可独立运行。
- **保持现有编程风格：** 后端参考 `LicenseAgentController` 等相邻实现，沿用 `BaseController`、`AjaxResult`、`TableDataInfo`、Controller/Service/ServiceImpl/Mapper、MyBatis-Plus、Lombok 和既有分页/权限方式；新模块可以按 MCP 业务分包，但不得另造响应包装、ORM、权限或通用框架。前端沿用现有 `http` 封装、`RouteConfigsTable`、Element Plus 表格/表单/弹窗和 Pinia 习惯，不新增 UI 或状态管理框架。
- **实用优先、避免过度优化：** 每个任务先找最接近的现有实现并复用，不做无关重构、格式化、依赖升级、通用低代码表单、可视化流程设计器、第二套 Agent、过早向量检索或多副本设计。固定几种真实配置工作流即可；没有明确用户场景的扩展接口、抽象层和“专业”功能不进入首版。
- `/mcp` 使用 `STREAMABLE + SYNC`，支持 initialize、tools/list、tools/call 和有状态 elicitation；不得新增已弃用的 HTTP+SSE MCP 传输，不把 STDIO 暴露为公网服务。
- 现有自定义登录 JWT 只用于 Vue 管理网关，不能冒充 OAuth 2.1 access token。公开 `/mcp` 校验固定 issuer/JWKS、过期时间、自身 audience 和 scope；`deptId` 是数据隔离边界，`userId` 是 actor，权限集合决定操作范围，任何输入都不能覆盖已验证身份。
- 管理 UI 走现有后台 `/iot/mcp/**`，由旧后台完成登录和权限校验，再以服务身份和签名 actor 上下文调用 MCP 模块内部 API；外部 MCP token、连接器 token、旧后台 JWT、服务凭据是四种不同凭据，禁止互相透传。
- 浏览器连接器只接受固定类型任务，不允许云端提交设备 IP、任意 URL、任意 HTTP 方法或任意文件路径。配对码和连接器 token 只存浏览器 `sessionStorage`，不写设备 Flash、URL、日志或模型上下文；云端不得保存设备登录 Cookie、CSRF token、MQTT 密码或 AI Key。
- AI 助手负责模型调用，MCP 服务负责工具、文档、配置草案和设备操作。设备 AI 的 Key 继续由用户在“连接配置”中手动设置；WiFi/AP/有线/蜂窝等网络配置保持现有手动页面，不纳入 AI 自动写入。
- 文档是不可信参考资料。文档文本、表格、图片 OCR、文件名和引用均不能扩大系统提示、工具权限或确认范围；文档中的“忽略指令”“自动执行”“写寄存器”等文字只作为普通内容处理。
- 不提供任意 JSON 文件写入或通用 `execute_device_api` 工具。每个可写配置节必须由固件能力目录和类型化 schema 注册，云端校验只负责早期反馈，设备生产解析器、资源预检和引用校验才是最终准入。
- 所有写操作遵循“读取基线 → 编辑草案 → 云端校验 → 设备预检 → 差异预览 → 人类确认 → 单次应用 → 回读核验”。模型提交的 `confirmed=true`、聊天中的模糊表述、文档内容或工具描述均不等于人类确认。
- 草案可保存 7 天；应用批准绑定 `deptId/userId/deviceId/draftHash/baseRevision`，120 秒过期且只能使用一次。设备全局/分节 revision 在执行前再次比较，冲突时保留原配置并要求重新同步。
- 所有设备写任务带幂等键和有界 deadline；超时、断连或正文不完整返回 `OUTCOME_UNKNOWN`，只允许查询和回读，禁止自动重试写入、自动回滚或恢复后重放。
- `accepted`、`persisted`、`readback`、`observed`、`physicalConfirmed` 必须分开返回和展示。配置保存、MQTT 发布、Modbus/SDK ACK、按钮响应均不等于物理负载已动作。
- 跨分节配置不得宣称天然原子。Task 4 未提供完整的组合预检、步骤日志和失败恢复前，MCP 只能应用单分节变更；多分节操作必须按依赖顺序执行，明确已完成步骤、待重启项和可恢复状态。
- 删除外设、Modbus 设备/任务、规则或动作前检查所有引用；数组缩容不得静默改变既有索引引用。规则默认先以 `enabled=false` 保存，启用需要独立确认和资源预检。
- 电机、继电器、Modbus 写、批量输出、OTA、重启、恢复出厂、账号/权限变更属于高风险操作，不从文档直接生成执行请求，不与普通配置共用确认。首版 AI 自动配置不包含这些动作；受控输出扩展必须另行通过风险等级、实物和故障恢复验收。
- 文档支持 `.pdf/.doc/.docx/.xls/.xlsx/.csv/.txt/.md/.json/.yaml/.yml/.xml/.html/.png/.jpg/.jpeg`；单文件最大 20 MiB、100 页、1000 万像素或 20000 表格行，单批最多 10 个且合计 100 MiB。加密、损坏、伪造扩展名、宏、外部实体、嵌入脚本、外链和超限文件明确拒绝，不执行其中任何代码。
- 文档 worker 无公网访问，限制 CPU、内存、进程数和处理时间；Java 网关流式保存文件，不在设备固件或浏览器内解析原始文档。文档服务故障只降级文档工具，不影响只读设备工具和已确认的配置恢复操作。
- MySQL 是设备绑定、草案、操作、文档元数据、配额和审计的事务真相；Redis 只保存可重建的限流、短期会话和队列租约。首版单副本，所有列表分页且单页最多 100 条，所有队列、历史、会话和文件保留期有上限。
- 审计只保存 actor、部门、设备、工具、参数哈希、结果码、耗时、配置 revision 和必要引用，不保存 Key、token、Cookie、密码、完整聊天、完整配置或供应商敏感正文。审计写入失败时写操作失败关闭。
- `fastbee-admin` 当前没有自动化测试目录；只有本计划经用户审核批准后，才在新增 MCP 模块和 Vue 新页面范围建立测试体系，不回填无关旧模块，不把 mock 通过描述为真实设备或生产通过。

## Review Focus

- **跨部门 deviceId、伪造 actor、错误 audience、连接器 token 混用：** 必须 401/403 或统一不存在错误，且没有设备任务和数据库写入；测试归 Task 2、Task 3。
- **文档中的寄存器地址、倍率、字节序或提示注入被误当成事实/命令：** 必须保留引用、歧义和置信度，不能自动应用或扩大权限；测试归 Task 8、Task 10、Task 11。
- **基线被另一页面修改、重复/并发确认、设备重启或连接中断：** 必须冲突拒绝或 `OUTCOME_UNKNOWN`，最多一个执行任务，不得重放；测试归 Task 4、Task 5。
- **外设/规则/Modbus 删除或重排破坏引用，功能未编译/未启用，资源不足：** 必须保留原运行态并返回可操作错误；测试归 Task 4、Task 6～9。
- **密钥进入模型、审计、浏览器持久存储或云端配置快照：** 必须只出现 `secretConfigured/secretRef`，秘密值由浏览器本地安全表单直送设备；测试归 Task 2、Task 4、Task 9、Task 12。
- **Vue 旧轮询/异步响应覆盖新设备或新草案：** 离页必须取消请求，响应必须核对 device/draft/revision，不得误确认旧操作；测试归 Task 12、Task 13。

---

## 1. 当前代码事实与方案调整

### 1.1 已核实的 `fastbee-admin` 现状

- 根目录只有 `springboot/`、`vue/`、`docker/` 等现有工程内容，没有 MCP 模块和 `AGENTS.md`。
- 后端是 Java 8、Spring Boot 2.5.14 多模块 Maven 工程；`license-server` 是启动入口，`fastbee-open-api` 是控制器层，`fastbee-service/fastbee-iot-service` 是设备业务模块，认证是自定义 JWT + Redis `LoginUser`。
- 身份包含 `userId/deptId/permissions`，没有独立 `tenantId`；本计划使用 `deptId` 做隔离，不新增平行租户体系。
- 当前开源控制器以 license、工单、系统管理为主，没有可直接复用的完整设备配置 API；常见 MQTT/Modbus 模型不能完全证明目标 FastBee-Arduino 配置契约存在。因此必须建立真实设备能力目录和固件桥接，不能拿旧平台模型冒充设备配置。
- 前端是 Vue 3 + TypeScript + Element Plus + Pinia，现有 `src/api`、`src/router/modules`、`src/views`、`src/store/modules` 结构可直接扩展。
- 现有 Docker 以 MySQL 5.7、Redis 7、OpenJDK 8 为主；新 MCP 服务需要独立 Java 21 容器，不能替换旧 `license-server` 镜像。

### 1.2 已核实的固件现状

- 已有外设 CRUD/预检、外设执行 CRUD/导入、规则脚本 CRUD/导入、协议/MQTT/Modbus 配置、功能估算与应用、设备信息和配置迁移接口。
- 现有配置迁移支持候选、图校验、准备、重启激活和恢复日志，但不是一个可供云端直接调用的统一“全配置即时事务”。
- 现有协议读取会脱敏，配置写入经过认证和开发者模式；MCP 桥接必须复用这些管理器与校验，不得直接覆盖 LittleFS 文件。
- 设备 AI 计划当前仅允许 `create_gpio_output` 和 `create_dht_sensor`，且目前未发现对应 AI 实现文件。本计划不是给这两个技能简单加文档，而是新增可选的 MCP 多工具模式，同时保留无 MCP 的本地核心。
- 固件已有 Host、Node 和 Playwright 测试，可承载新配置 revision、应用状态机、连接器和 AI 工具循环回归。

### 1.3 相对旧 MCP 计划的关键变化

| 旧方案 | 新方案 |
| --- | --- |
| 新建独立 `fastbee-ai-mcp` 仓库 | 在 `fastbee-admin/springboot/fastbee-ai-mcp` 开发 |
| 假设 Java 21 / Spring Boot 4 是整个平台基线 | 仅新 MCP 子模块使用新栈，旧主服务保持 Java 8 |
| PostgreSQL + pgvector | 复用 MySQL/Redis；结构化字段 + ngram 全文检索优先，向量检索不成为首发依赖 |
| 平台 HTTP 直连设备业务接口 | 浏览器连接器转发白名单任务，云端不访问设备内网地址 |
| 只读、反馈和简单 HIGH/LOW 控制 | 完整类型化配置草案、预检、应用、回读和回滚 |
| 文档只检索，不生成配置 | 文档提取带引用的事实候选，经 AI 和双重校验生成配置草案 |
| 云端只做 MCP，管理界面未覆盖 | Vue 提供服务配置、设备、文档、草案、下发、监测和审计完整后台 |
| 设备 AI 只传文档上下文 | 设备 AI 增加有界多轮 MCP 工具调用，无 MCP 本地模式继续可用 |

### 1.4 开源项目评估与采用边界

以下来源在 2026-10-31 重新查阅，只采用经过契约、安全和许可证审查的设计或依赖，不以“开源”替代生产验收：

| 项目 | 可复用内容 | 本计划决策 |
| --- | --- | --- |
| [`imaad786/mcp-upload`](https://github.com/imaad786/mcp-upload)（Apache-2.0） | 一次性上传票据、固定 destination、owner 绑定、流式字节、大小/SHA-256 校验、单次 claim、Redis 租约及慢客户端/并发测试 | **采用设计并做 Java 等价实现。** MCP 新增 `request_document_upload`，模型只接触 documentId/URI，不接触 Base64；字节通过独立 HTTPS 上传端点进入受控存储。其核心是 Python，不直接嵌入 Java 进程；SEP-2631 仍是提案，只做可选适配，不作为首发硬依赖。 |
| [FastMCP `FileUpload`](https://gofastmcp.com/apps/providers/file-upload) | 拖拽 UI 与模型上下文分离、app-only 存储工具、单文件/总量/TTL、按认证用户隔离、自定义持久化存储 | **采用交互与隔离语义。** Vue/设备页面提供原生上传，模型只调用 list/search/read 引用；无稳定认证身份的 stateless HTTP 请求不得共享文件桶。默认内存存储不能用于本计划，必须使用 MySQL 元数据 + 对象存储。 |
| [`tanvir-ux/agentic-mcp-gateway`](https://github.com/tanvir-ux/agentic-mcp-gateway)（MIT，示例项目） | MCP 与产品 HTTP/UI 共用类型化工具注册表；ReAct 工具循环；确定性 FakeLLM 测试；检索引用、写操作幂等和审计 | **采用共享注册表和可测试任务编排，不引入演示 ReAct 作为第二套 Agent。** `FastBeeToolRegistry` 同时服务 MCP 和 Vue 工作流；设备 AI 仍是唯一模型工具循环。 |
| `Simple-Agentic-Stack` | 用户建议的流程串联思路 | 本次未核实到明确、唯一的官方仓库和许可证，**不直接引用代码或增加依赖**；待提供准确仓库后再做依赖审查，不影响当前设计。 |
| [`modelcontextprotocol/servers`](https://github.com/modelcontextprotocol/servers) | Everything 的协议能力示例；Filesystem 的允许目录、路径/链接防护、dry-run diff、ToolAnnotations；Git/数据库示例 | **作为协议和安全参考，不作为生产依赖。** 使用 Everything/Inspector 做互操作；所有工具标注 readOnly/idempotent/destructive/openWorld。官方明确说明示例不是生产就绪，不能直接开放任意文件系统、Git、SQL 或 shell。 |
| [`sujianqingfeng/mcp-upload-file`](https://github.com/sujianqingfeng/mcp-upload-file) | 基础 multipart 上传示例 | 不集成其“模型提供 URL、服务端下载并转发”的路径，避免任意 URL/SSRF、目标地址混淆和文件来源不可控。 |

第三方依赖在 Task 1 保存 `THIRD_PARTY_NOTICES.md`、版本/commit、许可证和改动清单。优先通过稳定包依赖集成；确需复制代码时保留许可证和来源，放入新模块的明确 `thirdparty` 边界并补安全测试，不把整个开源 Agent/文件服务器打包进产品。

## 2. 产品范围与用户闭环

### 2.1 主要用户流程

1. 用户在设备现有网络页面完成联网，在 AI“连接配置”中手动设置 OpenAI 兼容地址、模型和 Key。
2. 用户在 Vue“MCP 管理 → 设备接入”或设备 AI 页面生成一次性配对码；设备浏览器交换短时令牌并显示连接器在线。
3. 用户在聊天窗口上传数据手册、寄存器表、接线说明、截图或既有配置样例；云端完成解析、表格提取和检索，返回页码/工作表/行列引用。
4. 用户描述目标，例如“按这份 SHT31 手册添加传感器，每 10 秒采样，并通过 MQTT 上报温湿度”。AI 先读取设备能力、现有配置和文档事实，缺少引脚、地址、波特率等必要参数时反问，不猜值。
5. AI 调用类型化工具创建/更新配置草案；每次工具结果只说明草案和校验状态，不宣称已经写入设备。
6. 云端完成 schema、依赖和引用检查，再通过连接器请求设备真实预检；页面展示新增/修改/删除差异、风险、资源影响和文档依据。
7. 用户在设备聊天窗口或 Vue 配置工作台确认具体摘要；高危或含秘密字段使用独立确认/本地秘密表单，模型无法代替。
8. 连接器把一次性任务交给固件配置应用服务，云端记录每个步骤；成功后回读并比较 revision/hash，需要重启时明确显示“已持久化，待重启”。
9. 用户在 Vue 查看设备在线状态、配置版本、工具调用、文档队列、操作时间线、失败/未知结果和审计；结果不明时先回读，只有用户明确选择才执行恢复或新操作。
10. MCP 未配置、云端离线或文档解析失败时，无 MCP 的本地 AI 核心和既有 Web 表单仍可使用；Vue 明确显示降级范围，不伪称配置已同步。

### 2.2 首版配置覆盖

| 配置域 | AI/MCP 能力 | 关键约束 |
| --- | --- | --- |
| 设备基础信息 | 读取/修改名称、时区、NTP 等非网络身份配置 | deviceId 变更单独确认；不自动改开发者模式和安全策略 |
| 外设配置 | 按设备目录新增、修改、启停、删除全部已注册类型 | 真实引脚/总线/驱动/资源预检；不硬编码 GPIO/DHT 两种 |
| 外设执行 | 触发器、条件、动作、事件、启停、导入和依赖校验 | 使用设备动作目录；规则先禁用保存，危险动作另行审批 |
| 规则脚本 | 新增、修改、校验、启停、删除、模板转换 | 遵循脚本长度和变量契约；不在 Java/Python 中任意执行脚本 |
| Modbus RTU | 串口/DE、从站、功能码、寄存器映射、轮询任务、数据类型、字节序、倍率、单位 | 地址基准和表格歧义必须确认；写寄存器不随配置自动执行 |
| MQTT/MQTTS | 连接参数、认证方式、发布/订阅主题、QoS、retain、遗嘱、前缀、连接测试 | 密码/Secret 走本地秘密表单；主题和外设/规则引用一致 |
| 功能配置 | 查询编译/启用/运行状态，估算并修改用户功能意图 | 资源不足只拒绝或待恢复，不永久清除用户其他功能 |
| 屏幕及其他已注册配置 | 通过分节适配器逐步覆盖屏幕面板、IR/RF、SD、天气等 | 只有真实 schema/预检/回读适配器就绪后才开放对应工具 |
| 网络与 AI Key | 仅读取状态 | 继续由用户在现有页面手动配置，不通过聊天或 MCP 写入 |
| OTA、重启、恢复出厂、用户权限 | 不纳入普通配置工具 | 后续独立高风险设计和验收，不能由文档触发 |

### 2.3 非目标

- 不让 MCP 服务保存或代理用户的模型 Key，不把 MCP 服务变成另一个模型供应商。
- 不用大模型自由生成任意 LittleFS 文件，不允许模型绕过现有 manager/service/driver 分层。
- 不在 ESP32 上上传、存储或解析 PDF/Office/图片，不让文档 worker 直接持有设备凭据。
- 不在首版承诺多副本水平扩展、无人值守自动运维、自动修复死循环或所有文档内容的百分之百识别准确率。
- 不把 Vue 管理后台复制成另一套绕过设备预检的独立配置系统；所有页面和 AI 工具共用同一个草案与设备应用服务。

## 3. 总体架构与信任边界

```text
外部 MCP 客户端 ── OAuth 2.1 ─┐
                               │
设备 AI 浏览器 ── MCP 工具桥 ───┼── fastbee-ai-mcp (Java 21)
                               │    ├─ MCP 工具与授权/配额/审计
Vue ──旧后台登录── /iot/mcp/** ─┘    ├─ 文档元数据/检索/事实候选
                                      ├─ 配置快照/草案/差异/提案
                                      └─ 连接器任务与操作状态机
                                               │
                                      MySQL + Redis + 对象存储
                                               │
                                      隔离文档 worker（无公网）

设备 AI 浏览器连接器（配对 token 仅 sessionStorage）
        │ 云端长轮询仅取白名单任务
        │ 本机同源请求仍使用设备现有认证/CSRF
        ▼
FastBee-Arduino ManagedConfigApplyService
        ├─ 统一配置 snapshot/revision
        ├─ 生产解析器 + 资源/引用预检
        ├─ 分节应用 + 步骤日志 + 回读
        └─ Peripheral / PeriphExec / RuleScript / Protocol / Feature managers
```

- **旧后台管理网关：** Vue 不直接处理内部服务凭据；旧后台只代理有权限的 `/iot/mcp/**` 请求，并对 actor 上下文签名。
- **MCP Resource Server：** 只接受目标 OAuth issuer 的 token，提供 Protected Resource Metadata，不转发 token 给设备或旧后台。
- **浏览器连接器：** 云端只保存设备身份和任务，不保存局域网地址；浏览器定时取任务、调用固定固件 API、返回有界结果。
- **设备配置应用：** 设备始终重新鉴权、校验和检查资源；云端“有效”不代表设备“可应用”。
- **AI 工具桥：** 设备模型只看到当前所需的有界工具 schema 和结果，无法直接访问数据库、文档存储或管理器。

### 3.1 共享工具注册表与可重复任务编排

`FastBeeToolRegistry` 是工具定义的唯一来源，每个定义包含稳定名称、JSON Schema、所需 scope/权限、风险等级、MCP ToolAnnotations、超时、幂等策略和执行服务。MCP 注解层、设备 AI 工具目录、Vue 工具目录和固定工作流服务都读取同一注册表，禁止 Java MCP、旧后台和前端各自维护不一致的字段副本。该注册表只是现有工具元数据的集中定义，不另外搭建规则引擎或通用插件框架。

工作流是**几种固定任务的持久化状态机**，不是再引入一个自主 Agent，也不开发可视化流程设计器、自定义步骤脚本或通用工作流 DSL。首版内置：

| 模板 | 固定步骤 |
| --- | --- |
| `document_to_peripheral` | 上传/选择文档 → 解析 → 事实候选 → 用户补充 → 外设草案 → 双重预检 → 确认 → 应用/回读 |
| `document_to_modbus` | 文档表格 → 地址/倍率/字序消歧 → Modbus 草案 → 引用检查 → 确认 → 应用/回读 |
| `mqtt_reporting_setup` | 外设/脚本/主题草案 → 依赖排序 → 连接测试 → 确认 → 应用 → 数据有效性观察 |
| `configuration_review_apply` | 现有草案 → 差异/风险审查 → 设备预检 → 确认 → 应用/回滚入口 |
| `device_health_review` | 设备/资源/协议状态 → 问题摘要 → 可选配置草案，不自动写设备 |

每次运行保存 `workflowRunId/templateVersion/inputHash/currentStep/state`，状态为 `WAITING_INPUT/RUNNING/WAITING_CONFIRMATION/SUCCEEDED/FAILED/CANCELLED/UNKNOWN`。步骤调用共享工具服务并具备幂等键、deadline、输出摘要和恢复点；恢复只能继续只读/等待步骤，写步骤结果不明必须先回读。用户可通过 `list_workflow_templates`、`start_configuration_workflow`、`get_workflow_run`、`cancel_workflow_run` 管理，AI 不得借工作流绕过逐步授权。

操作完成、失败或结果不明可通过旧后台现有通知能力发送脱敏订阅消息；通知只包含设备别名、操作 ID、结果码和 Vue 链接，不包含配置正文、秘密或文档内容，通知失败不反写设备操作结果。

## 4. MCP 工具契约

### 4.1 只读与诊断工具

| 工具 | 返回 |
| --- | --- |
| `list_devices` | 当前 actor 可见设备、在线/连接器状态、固件和 schema 版本，游标分页 |
| `get_device_capabilities` | 编译能力、运行功能、板卡、外设/动作目录、配置分节适配器和限制 |
| `get_device_configuration` | 指定分节的脱敏快照、`sectionRevisions/globalRevision`、采样时间 |
| `get_device_status` | 在线、内存、最大连续块、PSRAM、任务栈、MQTT/Modbus/规则状态及有效性 |
| `list_peripherals` / `read_peripheral` | 外设配置和带时间/有效性的读数，有效零值与缺失值分离 |
| `list_execution_rules` / `list_rule_scripts` | 规则、脚本、启用状态、引用摘要和 revision |
| `get_protocol_configuration` | 脱敏 MQTT/Modbus 配置，秘密只返回 `secretConfigured` |
| `get_configuration_history` / `get_operation` | 当前部门可见的配置历史和分级操作结果 |
| `get_operation_audit` | 有界审计列表，不返回秘密或完整业务正文 |
| `submit_feedback` | 预览并确认后提交有界、脱敏的问题或不支持能力草稿 |

### 4.2 配置草案与应用工具

| 工具 | 行为 |
| --- | --- |
| `create_configuration_draft` | 基于指定设备当前 `globalRevision` 创建草案，不修改设备 |
| `get_configuration_draft` | 返回当前差异、校验问题、文档依据和状态 |
| `update_device_profile` | 修改允许的设备基础字段 |
| `upsert_peripheral` / `remove_peripheral` / `set_peripheral_enabled` | 类型化外设草案操作 |
| `upsert_execution_rule` / `remove_execution_rule` / `set_execution_rule_enabled` | 类型化外设执行草案操作 |
| `upsert_rule_script` / `remove_rule_script` / `set_rule_script_enabled` | 类型化脚本草案操作 |
| `configure_modbus_rtu` | 设置总线和传输方式，不执行总线写 |
| `upsert_modbus_device` / `remove_modbus_device` | 修改从站定义并检查规则引用 |
| `upsert_modbus_poll_task` / `remove_modbus_poll_task` | 修改寄存器映射和轮询任务 |
| `configure_mqtt` / `upsert_mqtt_topic` / `remove_mqtt_topic` | 修改非秘密连接字段和主题；秘密字段只接受 `secretRef` |
| `test_mqtt_configuration` | 经确认后让设备执行有界连接测试，不保存秘密到云端 |
| `configure_runtime_features` | 修改功能意图并返回资源估算/重启需求 |
| `update_registered_config_section` | 仅用于 Task 9 已注册的其他分节；按具名 schema 严格拒绝未知字段 |
| `validate_configuration_draft` | 云端校验并请求设备预检，返回结构化 issues 和资源影响 |
| `preview_configuration_draft` | 固定草案 hash、可读摘要、精确差异、风险和确认文本 |
| `apply_configuration_draft` | 通过 MCP elicitation 或 Vue 已验证确认后创建一次性应用操作 |
| `get_configuration_operation` | 查询步骤、accepted/persisted/readback/observed、错误和恢复建议 |
| `rollback_configuration_operation` | 对明确成功且支持恢复的操作生成新的确认与恢复操作，不自动执行 |

工具不以套餐动态增删注册；scope 至少分为 `fastbee.read`、`fastbee.documents`、`fastbee.configure`、`fastbee.control`、`fastbee.audit`。普通配置需要 `fastbee.configure`，受控输出仍需要独立 `fastbee.control`，`tools/list` 可见不代表有调用权限。

### 4.3 文档工具

| 工具/接口 | 行为 |
| --- | --- |
| `request_document_upload` | 创建绑定 dept/user/device、固定 destination、声明大小/SHA-256 的一次性上传票据，15 分钟过期；只返回上传 URL/descriptor，不接收 Base64 |
| `POST /upload/v1/{ticket}` 及既有管理/连接器上传接口 | 浏览器或支持上传的 MCP 客户端流式提交字节；校验票据、owner、大小和摘要后返回 documentId，不经过设备固件 |
| `list_documents` / `get_document_status` | 分页查看上传、解析、索引、失败状态和安全错误码；上传记录只能被原 owner 单次 claim 到配置工作流 |
| `search_documents` | 返回最多 12 KiB 的授权摘录及页码/工作表/行列引用，不提供任意服务器路径读取 |
| `extract_configuration_facts` | 返回寄存器、引脚、参数、单位、倍率等事实候选、原文引用、置信度和歧义 |
| `retry_document_parsing` | 仅对可重试的解析失败创建新任务，不重复上传原文件 |
| `delete_document` | 经 elicitation/Vue 确认后删除原文件、切块、索引和事实候选 |

## 5. 核心接口与状态机

### 5.1 Java 边界接口

```java
public record ActorContext(
        Long deptId, Long userId, Set<String> scopes,
        Set<String> permissions, String tokenId) {}

public interface DeviceConnectorClient {
    DeviceSnapshot snapshot(ActorContext actor, String deviceId,
                            Set<ConfigSection> sections);
    ConfigPreflightResult preflight(ActorContext actor, String deviceId,
                                    ConfigDraft draft, String idempotencyKey);
    ConfigApplyResult apply(ActorContext actor, String deviceId,
                            ConfigDraft approvedDraft, String approvalHash,
                            String idempotencyKey);
    ConfigApplyResult rollback(ActorContext actor, String deviceId,
                               String operationId, String idempotencyKey);
}

public interface ConfigurationDraftService {
    ConfigDraft create(ActorContext actor, String deviceId, String baseRevision);
    ConfigDraft applyOperation(ActorContext actor, UUID draftId,
                               TypedConfigOperation operation);
    ConfigValidationResult validate(ActorContext actor, UUID draftId);
    ConfigPreview preview(ActorContext actor, UUID draftId);
    ConfigurationOperation apply(ActorContext actor, UUID draftId,
                                 UserConfirmation confirmation);
    ConfigurationOperation rollback(ActorContext actor, UUID operationId,
                                    UserConfirmation confirmation);
}

public interface DocumentFactService {
    PageResult<DocumentFact> extract(ActorContext actor, UUID documentId,
                                     FactKind kind, PageRequest page);
}
```

`UserConfirmation` 只能由 MCP elicitation 适配器或 Vue 管理网关的已验证确认创建，工具 DTO 不允许模型直接构造。所有 `applyOperation` 只修改草案，方法名中的 `apply` 不得被误解为设备写入。

### 5.2 设备桥接 API

| 方法与路径 | 行为 |
| --- | --- |
| `GET /api/mcp/capabilities` | 编译/运行能力、配置 schema 版本、分节适配器和当前 revision |
| `GET /api/mcp/config/snapshot` | 返回所选分节脱敏快照、hash、revision 和快照时间 |
| `POST /api/mcp/config/preflight` | 接收类型化分节补丁，只做生产解析、组合资源和引用检查，不保存 |
| `POST /api/mcp/config/apply` | 接收 `{draftId,baseRevision,approvalHash,operations,idempotencyKey}`，单次应用 |
| `GET /api/mcp/config/operations/{id}` | 返回本启动内的操作步骤和结果；重启后由云端记录和设备回读共同恢复语义 |
| `POST /api/mcp/config/rollback` | 对已确认恢复操作执行受控回滚，不能接受任意历史文件 |
| `POST /api/mcp/secrets/{secretRef}` | 浏览器本地秘密表单直送一次性秘密；不进云端和模型，应用后立即清除 |

所有接口要求设备现有认证和开发者模式，写接口继续使用同源/CSRF、防重放、低内存准入和有界正文；不得在 AsyncWebServer 回调中执行长时间总线或网络动作。

### 5.3 草案与操作状态

```text
EDITING → VALIDATING → READY → AWAITING_CONFIRMATION → APPLYING
   ↑          │             │              │               ├─→ SUCCEEDED
   └──────────┴─────────────┴──────────────┘               ├─→ FAILED
                                                           └─→ UNKNOWN

SUCCEEDED →（新的确认与恢复操作）→ ROLLING_BACK → ROLLED_BACK / FAILED / UNKNOWN
```

- 任一草案内容变化都会使旧预检和旧确认失效。
- `baseRevision` 变化返回 `CONFIG_REVISION_CONFLICT`，不得自动覆盖他人的新配置。
- `APPLYING` 只能由一次数据库条件更新取得；重复和并发请求返回原 operationId。
- 服务重启后遗留 `APPLYING/ROLLING_BACK` 一律转为 `UNKNOWN`，启动时不自动重放。
- 多分节按 `features → peripherals → protocol/modbus → ruleScripts → periphExec → dependent UI` 的依赖顺序预检/应用；删除按反向依赖顺序。实际顺序由分节适配器声明，不允许模型自行决定。
- 某步失败保留已完成步骤的事实和可恢复快照；无法保证安全自动回滚时停止并请求人工选择，不能为“看起来原子”而掩盖部分成功。

### 5.4 固定错误码

至少包含：`UNAUTHENTICATED`、`FORBIDDEN`、`DEVICE_NOT_FOUND`、`DEVICE_CHANNEL_UNAVAILABLE`、`DEVICE_IDENTITY_CONFLICT`、`CAPABILITY_UNSUPPORTED`、`SCHEMA_VERSION_UNSUPPORTED`、`INVALID_ARGUMENT`、`SECRET_REQUIRED`、`DOCUMENT_NOT_FOUND`、`DOCUMENT_INVALID`、`DOCUMENTS_UNAVAILABLE`、`DRAFT_NOT_FOUND`、`DRAFT_CONFLICT`、`CONFIG_REVISION_CONFLICT`、`PREFLIGHT_FAILED`、`CONFIRMATION_REQUIRED`、`CONFIRMATION_EXPIRED`、`OPERATION_IN_PROGRESS`、`QUOTA_EXCEEDED`、`AUDIT_UNAVAILABLE`、`OUTCOME_UNKNOWN`。

跨部门资源统一返回不存在；协议认证使用 HTTP 401/403，工具业务错误使用结构化 `CallToolResult.isError=true`，不暴露 SQL、堆栈、内部地址或原始秘密。

## 6. 数据与文档设计

### 6.1 MySQL 表

- `mcp_device`：部门、设备唯一键、名称、固件/芯片/schema、能力 JSON、当前配置 revision、状态、lastSeen、创建/更新时间；`(dept_id,device_id)` 唯一。
- `mcp_connector_session`：设备、配对码/token 哈希、精确 Origin、actor、过期、撤销、lastSeen；不保存明文长期秘密。
- `mcp_config_snapshot`：设备、分节、脱敏 JSON、hash、revision、采集时间；按设备/分节/revision 有界保留。
- `mcp_config_draft`：UUID、部门、actor、设备、baseRevision、当前补丁、draftHash、状态、issues、文档引用、过期和版本号。
- `mcp_config_template`：部门、名称、schema 版本、脱敏补丁、来源设备可选、创建者和时间；不包含秘密、设备身份或私有文档正文。
- `mcp_config_operation`：UUID、draft、批准 hash、幂等键、状态、当前步骤、结果分级、错误、恢复操作引用和时间。
- `mcp_config_operation_step`：操作、分节、顺序、请求/响应摘要哈希、状态、accepted/persisted/readback/observed、重试边界和时间。
- `mcp_document`：部门、actor、设备可选、文件名、真实媒体类型、大小、SHA-256、存储键、状态、安全错误码和保留期限。
- `mcp_upload_ticket`：票据 SHA-256、owner、固定 destination、声明大小/摘要、过期、redeemed/claimed 状态、尝试次数和并发租约；不明文保存可复用票据。
- `mcp_document_chunk`：文档、页码/工作表/行列、文本、数值 token、内容 hash；MySQL ngram FULLTEXT 与部门/文档过滤共同使用。
- `mcp_document_fact`：文档、事实类型、规范化 JSON、原文引用、置信度、歧义、审核状态；事实被采用到草案时记录引用，不自动写设备。
- `mcp_workflow_run` / `mcp_workflow_step`：模板版本、输入 hash、当前步骤、状态、幂等键、等待输入/确认、恢复点和安全输出摘要；不把模型自由文本当作恢复指令。
- `mcp_client_grant`：部门、外部 subject/clientId、允许 scope、撤销和过期；活动会话仍使用有界短期状态，不保存 token 明文。
- `mcp_setting` / `mcp_notification_subscription`：白名单配置值、通知通道引用、事件类型、actor 和启用状态；不保存通道秘密或通知正文。
- `mcp_usage_daily`、`mcp_operation_audit`、`mcp_feedback`：原子配额、追加审计和确认后的反馈。

所有表必须包含 `dept_id` 和必要索引；跨部门查询在 service 和 SQL 双层过滤。配置/文档正文设置保留期与清理任务，审计只允许追加，数据库账号按最小权限授权。

### 6.2 文档解析与事实质量

- PDF/DOCX 优先提取原生文本和表格；扫描页、PNG/JPEG 使用中英 OCR；DOC/XLS 通过隔离 LibreOffice 转换；XLSX/CSV 保留工作表、行列和单位表头。
- TXT/MD/JSON/YAML/XML/HTML 按有界纯文本解析，禁用 XML 外部实体、HTML 脚本/iframe/远程资源；ZIP、可执行文件和含宏文件不自动展开或运行。
- 切块同时保留标题路径、页码、表格坐标和原始数值 token。检索先精确匹配寄存器地址、型号、功能码等结构化 token，再用 ngram 排序，避免纯语义相似掩盖数值差异。
- `DocumentFact` 至少区分 `PIN_MAPPING`、`I2C_ADDRESS`、`UART_PARAMETER`、`MODBUS_REGISTER`、`DATA_TYPE`、`BYTE_ORDER`、`SCALE`、`UNIT`、`MQTT_TOPIC`、`LIMIT`、`OTHER`。
- 地址必须保存原文（如 `0x0000`、`40001`）、解析值和地址基准候选；倍率必须保留公式方向；字节序/字序未明确时返回多个候选，不得由模型任选。
- 首个标准答案集至少覆盖 SHT31/AHT20/ADS1115、常见继电器/串口设备、一个含 40001 与 0 基地址歧义的 Modbus 表、扫描 PDF、Excel 寄存器表和含提示注入的文档。每个案例断言事实、引用和必须保留的歧义，不以“成功生成 JSON”代替内容正确性。

## 7. Vue 配置与监测后台

### 7.1 菜单和页面

| 页面 | 主要功能 |
| --- | --- |
| `/mcp/dashboard` | 服务/Worker 健康、设备与连接器在线数、文档队列、工具调用量、配置成功/失败/未知数、延迟和错误趋势 |
| `/mcp/devices` | 设备搜索、一次性配对、能力/版本/revision、连接器状态、手动同步、解绑和详情 |
| `/mcp/configuration` | 设备选择、分节快照、表格/表单编辑、草案列表、引用依据、校验问题、差异预览和确认 |
| `/mcp/operations` | 应用步骤时间线、回读、待重启、失败/未知恢复、受控回滚；只重试查询，不直接重试写 |
| `/mcp/workflows` | 模板目录、运行列表、等待输入/确认、步骤时间线、取消和脱敏通知订阅；不允许恢复未知写步骤 |
| `/mcp/documents` | 多文件上传、进度/取消、解析状态、原文/表格预览、事实候选审核、检索、重解析和删除 |
| `/mcp/clients` | 外部 MCP 客户端说明、已授权 scope、活动会话和撤销；OAuth client 的创建/密钥仍由授权服务器负责 |
| `/mcp/tools` | 共享注册表中的工具/schema/风险/annotations/所需权限和调用统计，只读展示，不开放任意测试写调用 |
| `/mcp/audit` | 按 actor/设备/工具/结果/时间筛选，查看参数 hash 和安全摘要，不显示秘密 |
| `/mcp/settings` | MCP canonical URL、OAuth issuer/audience/JWKS、Origin、配额、会话期限、存储、Worker、保留期、通知和功能开关 |

### 7.2 配置工作台细节

- 分节 Tab 至少包含“设备信息、功能配置、外设配置、外设执行、规则脚本、Modbus RTU、MQTT、其他已注册配置”；未具备适配器的分节只读显示原因，不能显示为可写。
- 外设/规则/Modbus 使用与设备 schema 对应的轻量字段映射和 Element Plus 表单；同时提供只读 JSON 和可视化 diff，不允许通过自由 JSON 绕过字段校验，也不为此开发通用低代码表单设计器。
- 页面优先复用现有 `http` 请求封装、查询表单、分页表格、弹窗、权限指令和样式习惯；不新增 UI/状态框架，不改全局样式，不为少量页面引入大型组件库。
- 文档事实可插入草案，但每个插入值带来源标签、置信度和歧义；用户可查看原文定位。
- 草案可保存为带 schema 版本的配置模板并复制到其他设备；设备 ID、引脚/地址冲突项、秘密和文档私有内容默认不复制，目标设备必须重新能力检查、预检、差异预览和确认，禁止批量静默套用。
- 确认弹窗固定显示设备身份、baseRevision、变更计数、删除项、输出/重启风险、draftHash 短摘要和需要输入的确认文本。
- MQTT/其他秘密通过独立密码框进入设备本地秘密通道，页面只保存 `secretConfigured/secretRef`，禁止把秘密写入 Pinia、localStorage、URL 或审计。
- 操作详情分别展示已接受、已保存、回读一致、观察有效、物理确认；物理确认缺失显示“未验证”，不能显示成功勾。
- 所有请求带 device/draft/revision 上下文，离页取消轮询和 AbortController；旧响应不得覆盖新设备、新草案或新操作。

### 7.3 权限与监测口径

- 权限建议：`fastbee:mcp:view`、`fastbee:mcp:pair`、`fastbee:mcp:document`、`fastbee:mcp:configure`、`fastbee:mcp:apply`、`fastbee:mcp:rollback`、`fastbee:mcp:audit`、`fastbee:mcp:settings`。
- 服务监测区分 MCP 核心、MySQL、Redis、对象存储、文档 worker 和连接器，不用一个 `/health` 聚合成功掩盖文档降级。
- 指标不包含 user/device/文件名等高基数字段；明细只在受权限保护的审计页查询。
- 设备趋势保留内部 DRAM、最大连续块、PSRAM、任务栈、在线状态、MQTT/Modbus 状态、配置 revision 和操作积压；读数保留采样时间与有效性。

## 8. 实施任务

### Task 1: 旧功能基线、独立 MCP 模块与真实契约

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/pom.xml`（独立 POM，不继承旧 Boot 2.5 依赖管理，不加入旧 reactor）
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/FastBeeMcpApplication.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/McpServerConfig.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/FastBeeToolRegistry.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/resources/application.yml`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/McpTransportContractTest.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/docs/legacy-baseline.md`
- Create: `ADMIN/springboot/fastbee-ai-mcp/docs/dependency-baseline.md`
- Create: `ADMIN/springboot/fastbee-ai-mcp/docs/device-contract-inventory.md`
- Create: `ADMIN/springboot/fastbee-ai-mcp/THIRD_PARTY_NOTICES.md`

**Interfaces:**
- Consumes: 当前 Maven reactor、Spring AI 稳定文档、已评估开源项目及 Task 1 时固件真实路由/配置/测试。
- Produces: 旧功能基线、独立 Java 21 `/mcp` 服务、锁定依赖、共享工具定义、设备配置分节/schema/限制/缺口清单和第三方许可证清单。

- [ ] **Step 1: 在改代码前记录 fastbee-admin 旧功能基线**
  用原 JDK 8 执行既有 Maven 打包，记录旧依赖树和 `springboot/pom.xml` 摘要；在隔离测试环境启动无 MCP 配置的旧主服务，验证登录、license/licenseAgent 只读接口、工单、用户/角色/菜单及现有 Vue 关键页面。记录既有业务表结构和实际结果到 `legacy-baseline.md`；已有失败只标记为预存问题，不借本任务修复或掩盖。

- [ ] **Step 2: 写 MCP 传输失败测试并确认失败**
  断言 WebMVC `STREAMABLE + SYNC`、initialize/tools/list、协议版本拒绝、ToolAnnotations 和旧模块不依赖 Java 21。Run: `cd ADMIN/springboot && mvn -f fastbee-ai-mcp/pom.xml -Dtest=McpTransportContractTest test`。
  Expected: 因子模块和传输配置尚未完成而 FAIL。

- [ ] **Step 3: 建立独立模块并固定设备契约**
  锁定 Java 21、Spring Boot 4.x、Spring AI 2.0.x 及与其兼容的 MyBatis-Plus 稳定组合，版本只作用于新模块；记录旧 Java 8 构建所需 JDK 与新模块 JDK。覆盖 `device/features/peripherals/periphExec/ruleScripts/protocol/screenPanel/network/ai` 的读写边界、秘密字段和 revision 缺口。`FastBeeToolRegistry` 统一声明 schema/scope/风险/annotations/deadline/幂等策略。

- [ ] **Step 4: 完成开源审查和风格/隔离检查**
  保存开源项目的版本/commit、许可证和采用范围，未实际引入的只标“设计参考”。检查新模块只新增文件，未修改旧 POM/全局配置；相邻代码采用与现有项目相同的 Controller/Service/Mapper、分页和返回结构，未引入另一套通用框架。

- [ ] **Step 5: 验证新模块并复跑旧功能基线**
  Run: `mvn -f fastbee-ai-mcp/pom.xml -Dtest=McpTransportContractTest test`、`mvn -f fastbee-ai-mcp/pom.xml -DskipTests package`，再按 Step 1 复跑旧构建和关键冒烟。
  Expected: 新模块 PASS，旧 reactor 仍不包含 MCP，旧依赖、API/页面行为与基线一致；差异必须阻断后续任务，而不是到最后统一处理。

- [ ] **Step 6: 授权后提交**
  ```bash
  git add springboot/fastbee-ai-mcp
  git commit -m "feat: bootstrap FastBee MCP module"
  ```

### Task 2: OAuth、旧后台管理网关、配额与审计

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/security/ActorContext.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/security/McpSecurityConfig.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/security/InternalActorVerifier.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/audit/AuditService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/quota/QuotaService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/resources/db/migration/V1__mcp_security.sql`
- Create: `ADMIN/springboot/fastbee-open-api/src/main/java/com/fastbee/controller/mcp/McpGatewayController.java`
- Create: `ADMIN/springboot/fastbee-service/fastbee-iot-service/src/main/java/com/fastbee/iot/mcp/McpGatewayClient.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/security/McpSecurityIT.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/audit/AuditRedactionTest.java`

**Interfaces:**
- Consumes: Task 1 模块、旧后台 `LoginUser(userId,deptId,permissions)`、OAuth issuer/JWKS。
- Produces: `ActorContext`、管理网关、scope/permission 策略链、`QuotaService.checkAndConsume()`、`AuditService.append()/searchPage()`。

- [ ] **Step 1: 写身份与审计失败测试**
  覆盖无 token、过期、错误 issuer/audience、query token、无效 Origin、actor 签名篡改、deptId 越权、旧 JWT 直接访问 `/mcp`、四种凭据混用、并发配额、审计不可更新/删除以及秘密/配置正文脱敏。

- [ ] **Step 2: 运行测试确认失败**
  Run: `mvn -f fastbee-ai-mcp/pom.xml -Dtest=McpSecurityIT,AuditRedactionTest test`。
  Expected: FAIL，安全链和审计尚未实现。

- [ ] **Step 3: 实现四条独立认证路径**
  `/mcp` 使用 OAuth Resource Server；`/iot/mcp/**` 只经旧后台登录和 `@PreAuthorize`；旧后台到 `/internal/v1/**` 使用服务凭据 + 签名 actor；连接器 token 留到 Task 3。`ActorContext` 只由已验证身份构造，管理网关不允许客户端提交任意 dept/user。旧后台网关以独立 `fastbee.mcp.gateway.enabled=false` 默认关闭，只在新 Controller/Client 内读取配置，不修改现有 SecurityConfig、permitAll、CORS 或全局异常/请求拦截器；关闭时仅新路径返回 404/503，既有路径不变。

- [ ] **Step 4: 实现配额和追加审计并回归**
  使用 MySQL 原子增量和有界查询；审计失败时写操作失败关闭。Run: 同一测试命令及 `mvn -f fastbee-ai-mcp/pom.xml verify`。
  Expected: 越权零内部调用、配额并发准确、日志/异常无 token/秘密。

- [ ] **Step 5: 授权后提交**
  ```bash
  git add springboot/fastbee-ai-mcp springboot/fastbee-open-api/src/main/java/com/fastbee/controller/mcp springboot/fastbee-service/fastbee-iot-service/src/main/java/com/fastbee/iot/mcp
  git commit -m "feat: secure MCP and admin gateway"
  ```

### Task 3: 设备配对、注册与浏览器连接器

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/device/DeviceRegistryService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/connector/ConnectorController.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/connector/ConnectorSessionService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/connector/ConnectorJobService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/resources/db/migration/V2__device_connector.sql`
- Create: `DEVICE/web-src/modules/ai/ai-mcp-connector.js`
- Create: `DEVICE/test/unit/test_ai_mcp_connector.py`
- Create: `DEVICE/test/browser/suites/52-ai-mcp-connector.spec.ts`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/connector/ConnectorIT.java`

**Interfaces:**
- Consumes: Task 2 身份/审计、设备 `/api/device/info` 和 `/api/system/capabilities`。
- Produces: 配对/交换/心跳/长轮询/结果接口、`DeviceConnectorClient` 的有界任务通道、设备注册与在线状态。

- [ ] **Step 1: 写配对与连接器失败测试**
  覆盖配对码一次性/5 分钟过期/最多 5 次尝试、精确 Origin、设备 MAC+deviceId 冲突、跨部门 token、撤销、15 分钟令牌续期和 8 小时总上限、长轮询超时、重复结果、任务租约过期和离页取消；断言云端任务不能携带 URL/任意方法。

- [ ] **Step 2: 分别运行失败测试**
  Run: `mvn -f fastbee-ai-mcp/pom.xml -Dtest=ConnectorIT test`；在 DEVICE 构建锁下运行 `npm run test:unit -- test/unit/test_ai_mcp_connector.py` 和对应 Playwright 单套件。
  Expected: 连接器和云端状态机未实现而 FAIL。

- [ ] **Step 3: 实现设备注册与白名单任务通道**
  配对时核对设备身份、芯片/固件/schema 和部门绑定；同名不同 MAC 不得静默接管。连接器只从 `sessionStorage` 取 token，通过最长 20 秒长轮询获取任务，使用设备现有同源认证调用允许的本机 API；页面离线/离页停止轮询并上报最后状态。

- [ ] **Step 4: 实现任务租约和幂等结果**
  云端任务带 jobId/lease/idempotencyKey/deadline，只有当前租约可提交结果；重复 jobId 返回已记录结果，断连只转 `UNKNOWN/OFFLINE`，不自动重放。文档上传不经过设备 API。

- [ ] **Step 5: 运行双向回归并授权提交**
  Run: 上述 Java、DEVICE 定向测试及 `npm run test:e2e:collect`。
  Expected: PASS；无有效配对时本地设备页面和 AI 配置不受影响。

### Task 4: 固件统一配置快照、Revision、预检与应用桥

**Files:**
- Create: `DEVICE/include/core/ManagedConfigApplyService.h`
- Create: `DEVICE/src/core/ManagedConfigApplyService.cpp`
- Create: `DEVICE/include/core/ManagedConfigRevision.h`
- Create: `DEVICE/src/core/ManagedConfigRevision.cpp`
- Create: `DEVICE/include/network/handlers/McpConfigRouteHandler.h`
- Create: `DEVICE/src/network/handlers/McpConfigRouteHandler.cpp`
- Modify: `DEVICE/src/network/WebConfigManager.cpp`
- Modify: `DEVICE/scripts/device-api-test-matrix.json`
- Create: `DEVICE/test/test_mcp_config_apply.cpp`
- Create: `DEVICE/test/unit/test_mcp_config_route_contract.py`
- Create: `DEVICE/test/browser/suites/53-mcp-config-lifecycle.spec.ts`

**Interfaces:**
- Consumes: 现有配置管理器、预检、配置迁移 journal、认证/CSRF、功能资源估算。
- Produces: 第 5.2 节设备 API；`ManagedConfigApplyService::snapshot/preflight/apply/rollback` 和分节 revision。

- [ ] **Step 1: 写固件配置生命周期失败测试**
  覆盖各分节脱敏快照、revision 稳定/变化、秘密不回读、未知分节/字段、全局与分节冲突、重复/并发幂等、多分节依赖、资源不足、保存失败、回读不一致、待重启、崩溃恢复、回滚确认和结果不明不重放。

- [ ] **Step 2: 运行 Native/契约测试确认失败**
  Run: 在 DEVICE 构建锁下运行 `npm run test:native` 和 `npm run test:unit -- test/unit/test_mcp_config_route_contract.py`。
  Expected: 新服务/路由未实现而 FAIL；不为新测试另造平行入口。

- [ ] **Step 3: 实现分节适配器和真实预检**
  每个适配器声明 `section/schemaVersion/readRedacted/validate/preflight/apply/readback/dependencies/secretFields`。网络与 AI Key 只读；协议、外设、规则和功能分别调用现有生产解析与管理器。统一 revision 由规范序列化分节 hash 派生，不能以时间戳代替内容版本。

- [ ] **Step 4: 实现有界应用与恢复状态机**
  预检不实例化驱动、不执行输出；应用验证 approvalHash/baseRevision/idempotencyKey，记录步骤和结果分级。单分节与多分节都保留旧有效配置；无法安全即时回滚时停止并标记人工恢复，需要重启时复用现有迁移/待重启语义，不自动重启。

- [ ] **Step 5: 运行相关回归并授权提交**
  Run: `npm run test:native`、两个新增套件、既有配置迁移/外设/规则/协议定向测试以及 `npm run test:e2e:collect`。
  Expected: PASS；随后按 Task 15 要求补固件矩阵和实机证据，不以 Native 代替设备通过。

### Task 5: 配置草案、操作与共享工作流核心

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/ConfigurationDraftService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/TypedConfigOperation.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/ConfigSectionRegistry.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/ConfigurationOperationService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/ConfirmationService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/ConfigTemplateService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/workflow/WorkflowService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/workflow/WorkflowToolAdapter.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/resources/db/migration/V3__config_draft_operation.sql`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/config/ConfigurationDraftServiceIT.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/config/ConfigurationOperationServiceIT.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/workflow/WorkflowServiceIT.java`

**Interfaces:**
- Consumes: Task 1 共享工具注册表、Task 2 身份/审计、Task 3 连接器、Task 4 设备 API。
- Produces: 第 5.1 节 `ConfigurationDraftService`、草案/操作状态机、统一确认入口、脱敏配置模板和第 3.1 节持久化工作流。

- [ ] **Step 1: 写草案、操作与工作流失败测试**
  覆盖基线变化、draftHash 变化、过期、重复/并发确认、无 elicitation、DECLINE/CANCEL、跨部门 draftId、步骤部分成功、服务重启、设备断连、回读冲突和回滚新确认；配置模板测试秘密/设备身份剔除和跨设备重新预检；工作流覆盖模板版本、等待输入/确认、取消、恢复点和通知降级。断言拒绝路径设备写调用为 0，重复执行最多 1。

- [ ] **Step 2: 运行测试确认失败**
  Run: `mvn -f fastbee-ai-mcp/pom.xml -Dtest=ConfigurationDraftServiceIT,ConfigurationOperationServiceIT,WorkflowServiceIT test`。
  Expected: FAIL。

- [ ] **Step 3: 实现持久化草案、配置模板与规范化差异**
  所有类型化操作只修改草案；每次修改递增版本并使旧预检/确认失效。差异按分节和稳定 ID 排序，秘密只显示配置状态；保存文档引用、资源 issues、删除影响和待重启说明。`ConfigTemplateService` 保存前剔除秘密、设备身份和私有文档正文，复制到目标设备时生成新的草案并重新预检。

- [ ] **Step 4: 实现一次性应用和恢复操作**
  通过数据库条件更新取得执行权，调用连接器并持久化每一步；服务恢复只把中断状态改为 `UNKNOWN`。回滚是针对原操作快照的新操作，必须重新预检/确认，不自动覆盖更新 revision。

- [ ] **Step 5: 实现共享工作流状态机**
  工作流步骤只能调用 `FastBeeToolRegistry` 已注册服务，不能自行拼 SQL/URL/设备请求。持久化模板版本、输入 hash、等待输入/确认和恢复点；写步骤 UNKNOWN 只能回读，通知失败只记录告警，不改变设备结果。

- [ ] **Step 6: 运行完整核心回归并授权提交**
  Run: 上述测试及 `mvn -f fastbee-ai-mcp/pom.xml verify`。
  Expected: PASS，所有状态/审计/配额一致。

### Task 6: 只读工具与完整外设配置工具

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/DeviceReadTools.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/PeripheralConfigTools.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/peripheral/PeripheralOperationValidator.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/tool/DeviceReadToolsIT.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/tool/PeripheralConfigToolsIT.java`

**Interfaces:**
- Consumes: Task 3 设备能力、Task 4 快照/预检、Task 5 草案。
- Produces: 第 4.1 节只读工具和第 4.2 节外设工具。

- [ ] **Step 1: 写只读与外设失败测试**
  覆盖分页、有效零值、过期读数、离线、脱敏；动态外设目录、引脚/总线冲突、驱动未编译/未启用、非法枚举、重复 ID、初始输出风险、删除引用、资源不足和跨部门设备。

- [ ] **Step 2: 运行测试确认失败**
  Run: `mvn -f fastbee-ai-mcp/pom.xml -Dtest=DeviceReadToolsIT,PeripheralConfigToolsIT test`。
  Expected: FAIL。

- [ ] **Step 3: 实现工具与能力驱动校验**
  工具只从 `ActorContext` 取身份，外设 schema 来自设备目录而不是硬编码两种类型。所有 upsert/remove/enable 返回 draftId/draftHash/issues，不直接写设备；资源与驱动最终结论来自设备预检。

- [ ] **Step 4: 运行工具回归与 MCP Inspector 检查**
  Run: 上述测试、`mvn -f fastbee-ai-mcp/pom.xml verify`，用 MCP Inspector 完成 initialize/tools/list、只读调用、草案编辑和无确认零写入。
  Expected: PASS，工具描述明确“草案”与“应用”的区别。

- [ ] **Step 5: 授权后提交**
  ```bash
  git add springboot/fastbee-ai-mcp
  git commit -m "feat: add device and peripheral MCP tools"
  ```

### Task 7: 外设执行与规则脚本配置工具

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/ExecutionRuleTools.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/RuleScriptTools.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/rule/ConfigurationGraphValidator.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/tool/ExecutionRuleToolsIT.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/tool/RuleScriptToolsIT.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/resources/fixtures/rules/`

**Interfaces:**
- Consumes: 设备 trigger/event/action/controls 目录、Task 5 草案、Task 6 外设引用。
- Produces: 外设执行与规则脚本工具、跨分节引用图校验。

- [ ] **Step 1: 写规则与脚本失败测试**
  覆盖触发器/条件/动作 schema、未知动作、目标类型不匹配、数据源过期、重复 ID、循环启用依赖、脚本变量未定义、长度超限、危险动作、删除目标引用、规则默认禁用和独立启用确认。

- [ ] **Step 2: 运行测试确认失败**
  Run: `mvn -f fastbee-ai-mcp/pom.xml -Dtest=ExecutionRuleToolsIT,RuleScriptToolsIT test`。
  Expected: FAIL。

- [ ] **Step 3: 实现图校验和类型化规则工具**
  云端只进行早期结构与引用检查；动作参数以设备目录和生产契约为准。脚本作为有界文本/模板处理，不在云端 eval；最终导入/保存由设备 `RuleScriptManager` 和 `PeriphExecManager` 校验。

- [ ] **Step 4: 验证安全模拟与真实预检边界**
  使用 fixtures 做无硬件 dry-run，断言 dry-run 不调用 Modbus 写、GPIO 写、重启或 OTA；调用设备预检后返回引用和资源报告。
  Expected: 所有模拟结果标记 `simulated=true`，不冒充实物执行。

- [ ] **Step 5: 运行回归并授权提交**
  Run: 上述测试及 `mvn -f fastbee-ai-mcp/pom.xml verify`。
  Expected: PASS。

### Task 8: Modbus RTU 配置工具与文档标准答案

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/ModbusConfigTools.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/modbus/ModbusMappingValidator.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/config/modbus/ModbusAddress.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/document/DocumentFact.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/document/DocumentFactService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/tool/ModbusConfigToolsIT.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/resources/fixtures/modbus/`

**Interfaces:**
- Consumes: Task 5 草案、Task 7 引用图。
- Produces: Modbus 总线/设备/轮询任务工具、地址/映射规范化模型，以及 Task 10 实现的 `DocumentFactService` 和 `DocumentFact` 契约。

- [ ] **Step 1: 写 Modbus 失败测试和标准答案**
  覆盖 0/1 基地址、40001 记法、hex/decimal、线圈/输入/保持寄存器、功能码、数量、16/32/64 位、符号、字节序/字序、倍率方向、单位、串口/DE 引用、轮询周期、重复映射、数组缩容和规则索引引用。

- [ ] **Step 2: 运行测试确认失败**
  Run: `mvn -f fastbee-ai-mcp/pom.xml -Dtest=ModbusConfigToolsIT test`。
  Expected: FAIL。

- [ ] **Step 3: 实现显式地址与映射校验**
  `ModbusAddress` 保存 `rawText/value/baseKind/registerKind`，歧义未解决时不能形成 READY 草案。工具只生成配置，不执行寄存器写；设备最终校验 mapping、索引引用和串口资源。

- [ ] **Step 4: 运行文档到草案标准答案联调**
  对每个 fixture 断言生成字段、保留歧义和引用；模型缺失时可用固定事实候选运行确定性校验，但不能把固定候选测试称为模型识别准确率。
  Expected: 无歧义样本字段准确，歧义样本必须要求用户选择。

- [ ] **Step 5: 运行回归并授权提交**
  Run: 上述测试及 `mvn -f fastbee-ai-mcp/pom.xml verify`。
  Expected: PASS。

### Task 9: MQTT、设备信息、功能及其他分节配置工具

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/MqttConfigTools.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/DeviceProfileTools.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/FeatureConfigTools.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/RegisteredSectionTools.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/tool/MqttConfigToolsIT.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/tool/FeatureConfigToolsIT.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/tool/RegisteredSectionToolsIT.java`

**Interfaces:**
- Consumes: Task 4 分节适配器、Task 5 草案、Task 6～8 引用与配置工具。
- Produces: MQTT/设备/功能工具和其他已注册分节的受控扩展入口。

- [ ] **Step 1: 写协议与功能失败测试**
  覆盖 mqtt/mqtts、端口、认证类型、主题前缀、QoS/retain、遗嘱、OTA 主题保护、密码/secretRef、连接测试确认；功能编译/启用/运行差异、资源估算、依赖顺序、网络/AI Key 写入拒绝和未知分节字段拒绝。

- [ ] **Step 2: 运行测试确认失败**
  Run: `mvn -f fastbee-ai-mcp/pom.xml -Dtest=MqttConfigToolsIT,FeatureConfigToolsIT,RegisteredSectionToolsIT test`。
  Expected: FAIL。

- [ ] **Step 3: 实现 MQTT 与秘密引用流程**
  云端草案只保存非秘密字段和 `secretRef`；Vue/设备浏览器把秘密值直送 Task 4 设备秘密接口，测试连接和应用都只返回脱敏结果。`test_mqtt_configuration` 有独立确认、超时和恢复语义，不自动重连循环。

- [ ] **Step 4: 实现设备/功能/注册分节工具**
  设备字段使用允许列表；功能变更复用设备 estimate/apply 结果。屏幕、IR/RF、SD、天气等只有对应 schema、预检、回读适配器完成才注册可写，其余保持只读并返回 `CAPABILITY_UNSUPPORTED`。

- [ ] **Step 5: 运行回归并授权提交**
  Run: 上述测试及 `mvn -f fastbee-ai-mcp/pom.xml verify`。
  Expected: PASS；配置快照、日志和工具结果均无秘密值。

### Task 10: 多格式文档、检索与事实提取

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/document/DocumentController.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/document/UploadTicketController.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/document/UploadTicketService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/document/DocumentService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/document/DocumentWorkerClient.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/tool/DocumentTools.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/resources/db/migration/V4__documents.sql`
- Create: `ADMIN/springboot/fastbee-ai-document-worker/pyproject.toml`
- Create: `ADMIN/springboot/fastbee-ai-document-worker/app/main.py`
- Create: `ADMIN/springboot/fastbee-ai-document-worker/app/parser.py`
- Create: `ADMIN/springboot/fastbee-ai-document-worker/app/tables.py`
- Create: `ADMIN/springboot/fastbee-ai-document-worker/app/ocr.py`
- Create: `ADMIN/springboot/fastbee-ai-document-worker/app/facts.py`
- Create: `ADMIN/springboot/fastbee-ai-document-worker/tests/test_parser.py`
- Create: `ADMIN/springboot/fastbee-ai-document-worker/tests/test_security_limits.py`
- Create: `ADMIN/springboot/fastbee-ai-document-worker/tests/test_facts.py`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/document/DocumentApiIT.java`

**Interfaces:**
- Consumes: Task 2 身份/配额/审计、Task 3 连接器身份与会话、Task 8 `DocumentFactService` 契约、MySQL ngram、对象存储。
- Produces: 第 4.3 节文档工具、隔离解析、检索引用和 `DocumentFactService` 持久化实现。

- [ ] **Step 1: 写上传票据、格式、安全和标准答案失败测试**
  覆盖票据一次性/过期/owner/固定 destination/单次 claim、声明大小与 SHA-256 不符、慢客户端、并发上传、跨部门 documentId；覆盖 Global Constraints 中全部格式、伪造扩展名、加密/损坏、ZIP bomb、图片炸弹、宏/外链/XXE、页数/行数/像素/大小/超时；事实测试覆盖数值、表格坐标、地址/倍率/字序歧义和提示注入。

- [ ] **Step 2: 分别运行 Java/Python 失败测试**
  Run: `mvn -f fastbee-ai-mcp/pom.xml -Dtest=DocumentApiIT test`；`cd ADMIN/springboot/fastbee-ai-document-worker && python -m pytest -q`。
  Expected: FAIL，解析/索引/事实服务未实现。

- [ ] **Step 3: 实现票据化流式上传、隔离解析和检索**
  按 mcp-upload/FastMCP 的隔离语义实现 Java 等价上传票据，模型只得到引用，字节不经过 MCP JSON/Base64；无稳定认证身份不得共享文件桶。Java 不加载完整文件；worker 单任务最多 120 秒、2 CPU/2 GiB、无公网，转换进程有界。原文件、切块和事实均带部门/文档过滤，检索最多 12 KiB；核心 MCP readiness 不依赖 worker。

- [ ] **Step 4: 实现事实候选和删除闭环**
  事实保存原文、规范值、坐标、置信度和歧义，不能自动写草案或设备。重解析创建新任务并保留历史失败；删除经确认后清除原文件、索引、事实和草案引用中的可用摘录，草案保留“来源已删除”标记。

- [ ] **Step 5: 运行文档及核心隔离回归并授权提交**
  Run: 上述测试、`mvn -f fastbee-ai-mcp/pom.xml verify`，再在禁用 documents profile 下运行核心测试。
  Expected: 文档测试 PASS；worker 不可用时核心设备工具 PASS 且文档返回固定错误。

### Task 11: 设备 AI 多轮 MCP 工具调用与文档辅助配置

**Files:**
- Create: `DEVICE/include/core/AiMcpToolBridge.h`
- Create: `DEVICE/src/core/AiMcpToolBridge.cpp`
- Create: `DEVICE/web-src/modules/ai/ai-mcp-tools.js`
- Create: `DEVICE/web-src/modules/ai/ai-mcp-documents.js`
- Create: `DEVICE/docs/ai-mcp-tool-contract.md`
- Modify: `DEVICE/include/core/AiConversationManager.h`
- Modify: `DEVICE/src/core/AiConversationManager.cpp`
- Modify: `DEVICE/src/network/handlers/AiRouteHandler.cpp`
- Modify: `DEVICE/web-src/modules/ai/`
- Modify: `DEVICE/docs/superpowers/plans/2026-10-25-fastbee-device-ai-assistant.md`
- Create: `DEVICE/test/test_ai_mcp_tools.cpp`
- Create: `DEVICE/test/browser/suites/54-ai-mcp-chat.spec.ts`

**Interfaces:**
- Consumes: Task 3 连接器、Task 4 配置 API、Task 6～10 MCP 工具；若设备 AI 基础任务尚未实现，先完成/接管其 Task 1～6，不从空文件假设已有管理器。
- Produces: 有界 `awaiting_tool` 工具循环、文档引用上下文、聊天内草案/确认/结果展示；无 MCP 本地模式保持独立。

- [ ] **Step 1: 写 AI 工具循环失败测试**
  覆盖工具目录裁剪、单轮一个工具调用、最多 8 次迭代/120 秒、未知工具/额外字段、文档提示注入、跨会话 toolCallId、取消/离页、云端超时、用户拒绝确认、草案成功但设备未写和应用后回读。

- [ ] **Step 2: 运行测试确认失败**
  Run: DEVICE 新增 Native/浏览器套件及既有 AI 核心套件。
  Expected: MCP 模式未实现而 FAIL；无 MCP 核心测试必须继续 PASS。

- [ ] **Step 3: 实现浏览器代理的工具状态机**
  设备 job 增加 `awaiting_tool`，返回单个有界 toolCall；浏览器校验工具白名单后调用 MCP，再把有界结果回填原 job。模型 Key 只用于设备到供应商的请求，连接器 token 和 OAuth token 不进入模型提示或设备 Flash。

- [ ] **Step 4: 实现聊天内文档与配置闭环**
  附件直传云端；AI 可搜索文档、提取事实、读取设备、编辑草案和请求预检。只有页面完成人类确认后才调用 apply 工具；聊天展示来源引用、差异、风险和分级结果，用户可在多轮对话中继续修改同一草案。

- [ ] **Step 5: 运行有/无 MCP 对照回归并同步旧计划**
  Run: 新增套件、既有 AI 套件、`npm run test:native`、`npm run test:e2e:offline`。
  Expected: 两组分别 PASS；更新设备 AI 计划时保留现有未提交内容，明确本地两技能核心与完整 MCP 模式的边界。

### Task 12: Vue 设备接入、文档与配置工作台

**Files:**
- Create: `ADMIN/vue/src/api/mcp.ts`
- Create: `ADMIN/vue/src/router/modules/mcp.ts`
- Create: `ADMIN/vue/src/store/modules/mcp.ts`
- Create: `ADMIN/vue/src/views/mcp/devices/index.vue`
- Create: `ADMIN/vue/src/views/mcp/configuration/index.vue`
- Create: `ADMIN/vue/src/views/mcp/configuration/components/ConfigSectionEditor.vue`
- Create: `ADMIN/vue/src/views/mcp/configuration/components/DraftDiffDialog.vue`
- Create: `ADMIN/vue/src/views/mcp/configuration/components/PreflightIssueList.vue`
- Create: `ADMIN/vue/src/views/mcp/configuration/components/ApplyConfirmDialog.vue`
- Create: `ADMIN/vue/src/views/mcp/documents/index.vue`
- Create: `ADMIN/vue/src/views/mcp/documents/components/DocumentFactTable.vue`
- Create: `ADMIN/vue/src/views/mcp/operations/index.vue`
- Create: `ADMIN/vue/src/views/mcp/workflows/index.vue`
- Create: `ADMIN/vue/src/views/mcp/workflows/components/WorkflowRunTimeline.vue`
- Create: `ADMIN/vue/src/views/mcp/configuration/components/ConfigTemplateDialog.vue`
- Create: `ADMIN/vue/src/views/mcp/mcp.test.ts`
- Modify: `ADMIN/vue/package.json`（只增加新页面测试脚本和 devDependencies，不升级现有运行依赖）
- Modify: `ADMIN/vue/pnpm-lock.yaml`（仅锁定上述新增测试依赖）
- Create: `ADMIN/springboot/sql/fastbee-mcp-v1.sql`

**Interfaces:**
- Consumes: Task 2 管理网关、Task 3 设备配对、Task 5 草案/操作/工作流、Task 10 文档 API。
- Produces: 第 7 节设备、配置、文档、操作和工作流页面及权限菜单。

- [ ] **Step 1: 写 Vue 组件与 API 失败测试**
  使用审核后新增的 Vitest + Vue Test Utils，覆盖加载/空/错误、设备切换、动态 schema 表单、文档上传进度、事实引用、diff、秘密框、确认文本、工作流等待/取消、重复点击、旧响应覆盖、离页取消和权限禁用。

- [ ] **Step 2: 运行测试确认失败**
  Run: `cd ADMIN/vue && pnpm test:unit -- src/views/mcp/mcp.test.ts`。
  Expected: 页面和 API 尚未实现而 FAIL。

- [ ] **Step 3: 实现管理 API、路由、菜单和状态**
  API 只访问 `/iot/mcp/**`；Pinia 保存设备/草案的非秘密状态，所有请求带上下文标识。菜单权限与后端一致，按钮隐藏不是授权边界。所有菜单和 `mcp_` 表只写入独立 `fastbee-mcp-v1.sql`，执行前检查菜单 ID/权限冲突，不修改已发布的 `fastbee-v2.5.sql` 或既有业务表。

- [ ] **Step 4: 实现完整配置与文档交互**
  完成设备配对/同步、动态分节编辑、事实引用、校验、diff、确认、应用时间线、回读和受控回滚；秘密只通过本地设备通道提交。移动端/窄屏、长文档、空数据、失败和待重启状态均有明确展示。

- [ ] **Step 5: 运行前端验证并授权提交**
  Run: `pnpm test:unit -- src/views/mcp/mcp.test.ts`、`pnpm typecheck`、`pnpm build`。
  Expected: 全部 PASS，构建无新增类型错误；再执行一次真实网关的手工冒烟，不用 mock 结果替代。

### Task 13: Vue 服务设置、监测与审计后台

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/observability/McpObservabilityController.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/observability/ToolCatalogController.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/security/McpClientSessionService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/settings/McpSettingsService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/java/com/fastbee/mcp/settings/NotificationSubscriptionService.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/main/resources/db/migration/V5__settings_monitoring.sql`
- Create: `ADMIN/vue/src/views/mcp/dashboard/index.vue`
- Create: `ADMIN/vue/src/views/mcp/audit/index.vue`
- Create: `ADMIN/vue/src/views/mcp/clients/index.vue`
- Create: `ADMIN/vue/src/views/mcp/tools/index.vue`
- Create: `ADMIN/vue/src/views/mcp/settings/index.vue`
- Create: `ADMIN/vue/src/views/mcp/settings/components/NotificationSubscriptionForm.vue`
- Create: `ADMIN/vue/src/views/mcp/dashboard/components/DeviceResourceTrend.vue`
- Create: `ADMIN/vue/src/views/mcp/monitoring.test.ts`
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/observability/ObservabilityContractTest.java`

**Interfaces:**
- Consumes: Task 2 审计、Task 3 设备状态、Task 5 操作、Task 10 worker 状态。
- Produces: 服务配置、独立健康、指标、审计查询和监测页面。

- [ ] **Step 1: 写设置与监测失败测试**
  覆盖非法 issuer/audience/Origin、会话上下限、Worker 并发、保留期、配置生效/重启提示、指标脱敏、核心与文档独立健康、设备 stale 数据、审计权限和旧趋势响应覆盖。

- [ ] **Step 2: 运行失败测试**
  Run: `mvn -f fastbee-ai-mcp/pom.xml -Dtest=ObservabilityContractTest test`；`cd ADMIN/vue && pnpm test:unit -- src/views/mcp/monitoring.test.ts`。
  Expected: FAIL。

- [ ] **Step 3: 实现受控设置和独立健康**
  设置按权限和字段白名单更新，秘密只引用环境变量/秘密管理器名称；readiness 分开核心/MySQL/Redis/存储/worker，liveness 不做外部写操作。指标不含高基数身份或秘密。

- [ ] **Step 4: 实现 Dashboard、客户端、工具目录、审计和设置页面**
  展示第 7 节全部指标、筛选、时间线、资源趋势和错误状态；客户端页只管理本服务授权映射/会话撤销，不伪造授权服务器 client 管理；工具目录直接读取共享注册表。未知操作提供“查询回读”而不是“重试写入”。通知订阅复用旧后台通道且内容脱敏。离页停止轮询，图表只使用带时间/有效性的样本。

- [ ] **Step 5: 运行验证并授权提交**
  Run: 上述 Java/Vue 测试、`pnpm typecheck`、`pnpm build`、`mvn -f fastbee-ai-mcp/pom.xml verify`。
  Expected: PASS，核心降级与文档降级显示不同。

### Task 14: 容器、备份恢复、安全与容量验收

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/Dockerfile`
- Create: `ADMIN/springboot/fastbee-ai-document-worker/Dockerfile`
- Create: `ADMIN/docker/fastbee-mcp-compose.yml`
- Create: `ADMIN/springboot/fastbee-ai-mcp/.env.example`
- Create: `ADMIN/springboot/fastbee-ai-mcp/docs/runbook.md`
- Create: `ADMIN/springboot/fastbee-ai-mcp/scripts/backup-mysql.sh`
- Create: `ADMIN/springboot/fastbee-ai-mcp/scripts/restore-mysql.sh`
- Create: `ADMIN/springboot/fastbee-ai-mcp/scripts/load-smoke.k6`
- Modify: `ADMIN/docker/nginx/` 中实际生效的反向代理样例（先定位，不直接改现场配置）

**Interfaces:**
- Consumes: Task 1～13 全部服务与 UI。
- Produces: 可重复部署的单副本核心、可选文档 profile、备份恢复和运维手册。

- [ ] **Step 1: 定义部署失败场景与容量门槛**
  覆盖非 root、无公网 worker、秘密缺失启动失败、优雅停机、会话/草案/操作恢复、对象存储缺失、审计失败、核心与文档独立健康、Nginx `/mcp` 长连接和 `/iot/mcp/**` 旧后台路由不混淆。

- [ ] **Step 2: 构建并启动隔离测试环境**
  Run: `mvn -f fastbee-ai-mcp/pom.xml verify`、`docker compose -f ADMIN/docker/fastbee-mcp-compose.yml build`，再启动本次专用 compose project。
  Expected: 核心无 worker 时健康，启用 documents profile 后文档独立健康；不停止其他项目容器。

- [ ] **Step 3: 执行备份恢复和中断演练**
  在独立测试库验证设备绑定、草案、操作、文档元数据/原文件、配额和审计恢复；强制停止服务后确认中断写操作只变为 UNKNOWN，无自动重放。

- [ ] **Step 4: 执行安全和容量检查**
  使用 mock 设备运行 k6，不对真实设备压测或写配置；验证队列上限、分页、20 MiB 流式上传、worker 资源限制、限流、超时、审计增长和清理任务。记录单副本实测容量，不外推多副本。

- [ ] **Step 5: 执行 fastbee-admin 既有功能隔离回归**
  用 JDK 8 构建并启动不包含 MCP 依赖的旧主服务，不配置任何 MCP 环境变量；按 Task 1 `legacy-baseline.md` 逐项复测登录、license/licenseAgent 只读接口、工单、用户/角色/菜单和现有 Vue 页面，比较响应结构、权限和预存问题，不用新测试改写旧预期。确认 `springboot/pom.xml` 无改动、默认 reactor 无 MCP、旧安全链/CORS/数据源配置无 diff、既有业务表无结构变更，MCP 停止和网关关闭时以上用例仍通过。

- [ ] **Step 6: 完成 runbook 并授权提交**
  包含证书/OAuth、配对撤销、文档故障、结果不明处理、人工恢复、备份保留、升级回滚、旧功能回退验证和监测告警；保存本次验证输入与结果。

### Task 15: 端到端标准场景、全矩阵与发布验收

**Files:**
- Create: `ADMIN/springboot/fastbee-ai-mcp/src/test/java/com/fastbee/mcp/McpEndToEndIT.java`
- Create: `ADMIN/springboot/fastbee-ai-mcp/docs/verification.md`
- Create: `DEVICE/docs/device-test-configs/mcp-ai-assistant.json`
- Modify: `DEVICE/docs/superpowers/plans/2026-10-25-fastbee-device-ai-assistant.md` 的实施记录（只追加本计划已验证事实）
- Modify: `ADMIN/README.md` 和 `ADMIN/AGENTS.md` 的实际命令/边界（若与实现有差异）

**Interfaces:**
- Consumes: Task 1～14、真实 OAuth 测试 issuer、MCP Inspector、至少一种目标 MCP 客户端、授权测试设备和文档。
- Produces: 可重复的本地、集成、设备、实物和长期运行分级证据。

- [ ] **Step 1: 执行六条标准端到端场景**
  至少完成：GPIO/传感器外设；外设执行联动；规则脚本 MQTT 转换；PDF/Excel Modbus 寄存器到轮询配置；MQTT 主题与功能配置；多分节变更后的冲突、断连、回读和回滚。每条均从上传/对话或具名工作流开始，经过预检、确认和回读，并在 Vue 工作流/操作中心核对同一 runId/operationId。

- [ ] **Step 2: 执行权限、文档和 AI 负例矩阵**
  两部门越权、错误 audience、无 scope、无 elicitation、上传票据重放/摘要错误、文档注入、错误地址/倍率、重复确认、设备身份冲突、秘密泄露扫描和旧 Vue 响应；断言所有拒绝路径没有设备写调用。

- [ ] **Step 3: 运行服务端与前端完整验证**
  Run: `mvn -f fastbee-ai-mcp/pom.xml verify`、文档 worker 全量 pytest、`pnpm test:unit`、`pnpm typecheck`、`pnpm build`、MCP Inspector、官方 Everything 参考服务对照和目标真实客户端互操作。
  Expected: 所有命令本次退出码为 0；分别保存核心与文档结果，不相互覆盖失败；参考服务只用于协议对照，不进入生产部署。

- [ ] **Step 4: 运行固件回归和授权实机验收**
  在 DEVICE 构建锁下运行相关 `npm test`、`npm run test:native`、离线浏览器回归；因公共配置/API 变化，按影响执行九个正式环境构建。三个 S3 验证 AI/MCP，其他芯片验证无 AI 时配置桥或无回归；实机前核对 MAC/芯片/IP/Flash/PSRAM、备份配置并取得授权。

- [ ] **Step 5: 完成实物与长期验收并记录边界**
  外设读数、Modbus 数据和必要输出必须取得实物证据；按 DEVICE `AGENTS.md` 执行 8 小时 Web、24 小时及后续 72 小时观察，正常负载与故障注入分开。缺少任何硬件、时长、客户端或实物证据均标“待验证”，不得宣称完整发布或长期稳定通过。

## 9. 发布门槛、排期与审核决策

### 9.1 分阶段交付

| 阶段 | 任务 | 可用结果 |
| --- | --- | --- |
| P0 基础闭环 | Task 1～6 + Task 12 的设备/配置最小页面 | 安全配对、只读、外设草案、真实预检、确认应用、回读与基础后台 |
| P1 零代码配置完整 | Task 7～9 + Task 12 全部配置域 | 外设执行、脚本、Modbus、MQTT、功能和已注册分节 |
| P2 文档与 AI 完整体验 | Task 10～11 | 多格式文档、引用事实、多轮聊天配置闭环 |
| P3 运维与发布 | Task 13～15 | 设置/监测/审计、部署恢复、标准场景与全矩阵证据 |

P0 不是“只读演示”，必须包含一次真实配置写入和回读；P1 每个配置域可独立验收。文档或 AI 延期不得阻止已完成的设备配置工具通过 Vue 使用，但不得把未完成项从总计划完成范围中隐去。

### 9.2 统一发布门槛

- 新旧 Maven 构建、MCP 协议/安全/状态机测试、worker 测试、Vue 单测/typecheck/build 和 DEVICE 相关回归全部有本次退出码。
- `fastbee-admin` 默认旧构建、旧安全/数据源配置和 license、工单、系统管理 API/页面回归通过；无 MCP 配置、MCP 停止、网关关闭三种情况均不影响既有功能，既有数据库基线未修改。
- MCP Inspector 与至少一种真实客户端完成 OAuth、工具列表、票据上传、文档、草案、elicitation、工作流、冲突和未知结果；Vue 真实网关完成全部页面关键路径。
- 两部门越权矩阵、秘密扫描、文档恶意样本、并发/重复执行、服务重启、设备断连和备份恢复通过。
- 所有配置域都以真实设备生产预检为最终门槛；标准答案集的事实和引用准确，歧义样本不会自动生成 READY 草案。
- 本地通过、构建通过、设备 API 通过、实物通过、长期稳定通过分别记录；没有用 mock、单台 S3 或短时测试替代完整矩阵。

### 9.3 粗略排期

| 工作项 | 估算 |
| --- | ---: |
| Task 1～3 | 13～19 人日 |
| Task 4～5 | 14～21 人日 |
| Task 6～9 | 22～34 人日 |
| Task 10 | 10～15 人日 |
| Task 11 | 7～10 人日 |
| Task 12～13 | 12～18 人日 |
| Task 14～15 | 10～16 人日 |
| **合计** | **88～133 人日** |

估算不包含既有设备 AI 基础计划尚未完成的工作量；若 Task 11 执行时本地 AI 核心仍为空，需要先把该计划的剩余工作量单独计入。多分节配置事务和真实硬件组合若存在额外缺口，也在 Task 4 后重新评估，不通过减少校验压缩排期。

### 9.4 请审核的默认决策

1. **同仓库、独立进程：** MCP 在 `fastbee-admin` 内开发，使用独立 Java 21 POM/进程，不加入旧 Maven reactor、不修改旧 POM，也不升级旧 Java 8 主服务。
2. **浏览器连接器：** 云端不直连设备内网，也不以 MQTT publish 冒充配置事务；设备浏览器在线是云端配置下发的必要条件，离线只保留草案。
3. **BYOK 留在 AI 助手：** MCP 服务不调用模型、不保存模型 Key；设备 AI 通过浏览器桥接 MCP 工具，外部 MCP 客户端也可复用同一服务。
4. **复用现有数据基础设施：** 使用 MySQL + ngram 和 Redis，不引入 PostgreSQL/pgvector 作为首发硬依赖；后续如语义检索实测不足，再单独立项评估向量索引。
5. **设备侧补统一配置桥：** 为防冲突、回读和回滚，需要修改固件增加 revision 与受控应用 API，不是只开发 Java/Vue 就能安全完成。
6. **完整 Vue 后台：** 包含设置、设备、文档、草案、工作流、客户端、工具目录、下发、回滚、监测和审计，不以一个聊天页面代替管理界面。
7. **新增模块测试体系：** 当前 `fastbee-admin` 无自动化测试；批准本计划即表示允许仅为新 MCP 模块和新 Vue 页面引入标准测试，不扩散到无关旧业务。
8. **高风险边界：** 网络与模型 Key 手动配置；OTA、重启、恢复出厂、账号权限和实物输出不随普通 AI 配置自动执行，后续单独验收。
9. **开源复用有边界：** 采用 mcp-upload/FastMCP 的上传隔离语义、agentic-mcp-gateway 的共享工具注册表和官方 servers 的安全/互操作参考；不把 Python 服务、演示 Agent、任意 URL 下载、任意文件/SQL/Git 工具带入生产，Simple-Agentic-Stack 待准确仓库核实后再评估。
10. **旧功能与实用优先：** Task 1 先建立旧功能基线，每个阶段对照回归；新增代码沿用现有后端/前端风格，只实现实际配置场景，不做通用工作流 DSL、低代码设计器、无关重构或过早优化。
