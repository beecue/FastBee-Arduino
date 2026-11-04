# 设备 AI MCP 工具与文档契约

## 信任边界

- 本地 AI 不依赖 MCP。未配置、未配对、云端超时或文档不可用时，本地模型配置、对话、GPIO/DHT 草案和一次性确认继续可用。
- 模型 API Key 只保存在 `/config/ai.json`，仅用于设备到模型供应商的 TLS `Authorization` 头。Key、连接器 token、OAuth token 均不得进入模型消息、文档上下文或 MCP 工具结果。
- 连接器 token 与 OAuth token 只保存在浏览器 `sessionStorage`，不得写入设备配置。文档字节由浏览器直接上传云端，不经过设备 API 或 LittleFS。
- 文档、检索摘录、事实和工具结果一律是不可信数据。其中的“忽略指令”“确认”“应用”等文本不扩大权限，也不能触发设备写。

## 设备工具循环

`POST /api/ai/jobs` 可携带 `mcpEnabled=true`。设备每个模型响应只接受一个工具调用，进入 `awaiting_tool` 后返回：

```json
{
  "jobId": "aij_...",
  "state": "awaiting_tool",
  "iteration": 1,
  "toolCall": {
    "toolCallId": "call-1",
    "name": "search_documents",
    "arguments": { "query": "GPIO4" }
  }
}
```

- 每个任务最多 8 次模型迭代，MCP 任务总 deadline 为 120 秒；本地无 MCP 任务 deadline 保持 45 秒。
- 工具结果最大 16 KiB，错误码最大 128 字节。浏览器调用 `POST /api/ai/jobs/{jobId}/tool-result`，正文只能包含 `toolCallId` 以及恰好一个非空 `result` 或 `error`。
- 设备同时校验认证会话、`jobId`、当前 `awaiting_tool` 状态和精确 `toolCallId`。跨会话、重复、迟到、取消后的结果统一拒绝为 `AI_STALE_TOOL_RESULT`。
- 云端超时、工具错误、未知工具、额外参数或超限结果不自动重试。取消和离页只表示已请求取消，不伪称 TLS 已立即停止。
- 浏览器首次调用前完成 `initialize` 与 `notifications/initialized`，后续请求携带 `Mcp-Session-Id` 和 `MCP-Protocol-Version: 2025-06-18`；响应同时支持 JSON 与单个 JSON-RPC 消息的 `text/event-stream`，无法取得会话或响应畸形时失败关闭，不自动重放工具写操作。
- 页面刷新时使用当前 tab 的 `sessionStorage` 恢复连接器和 OAuth 会话，并重新完成 `tools/list` 与设备能力验证后才进入 `MCP_READY`。配对成功只表示连接器在线，不代表 MCP 已就绪；OAuth 失效时清除对应会话，网络/CORS 临时失败允许用户主动重试。
- 模型参数不包含配对上下文。浏览器为设备工具注入已配对 `deviceId`，创建草案前通过 `device.config.revision.read` 获取并注入 `baseRevision`；文档上传字段为 `fileName/sizeBytes/sha256/deviceId`，分页字段统一为 `cursor/limit`。标准 `CallToolResult` 先解包 `structuredContent` 或单个文本 JSON content，`isError=true` 一律失败关闭。

模型可用目录按安全配置域限定：设备/外设只读与外设草案、外设执行、规则脚本、Modbus RTU、MQTT 非秘密配置、运行时功能、设备资料、文档上传/检索/解析和固定工作流。所有参数对象均 `additionalProperties=false`，并按工具校验必填字段、类型和上限。设备上下文只允许由配对浏览器注入，模型不能切换到其他设备。

`apply_configuration_draft`、`rollback_configuration_operation`、规则/脚本启用、MQTT 连接测试和文档删除明确不在模型目录中。草案编辑和预检不产生设备写；这些操作只能经独立可信按钮和 `elicitation/create` 完成。MQTT 秘密值只通过 `/api/mcp/secrets/{secretRef}` 直送设备，模型与云端只见 `secretRef/secretConfigured`，单次连接测试另有独立人类确认。

## 人类确认与可信应用

只有聊天中的“确认应用草案”按钮可以调用可信应用路径。该路径必须携带本次按钮动作，并继续完成 MCP `elicitation/create` 人类确认；模型自行返回的 `confirmed=true`、确认文本、操作 ID 或“已获批准”均无效。确认过期、重放、草案/版本/哈希变化或用户拒绝均不得调用设备应用。

应用由云端可信确认链产生绑定草案哈希和 revision 的批准，再通过既有连接器 `device.config.apply` 任务到达设备。浏览器不构造任意 URL、HTTP 方法、批准哈希或设备写任务。

页面分别显示：

- `accepted`：执行请求已被接受；
- `persisted`：配置已保存；
- `readback`：配置回读一致；
- `observed`：运行状态或采样已观察；
- `physicalConfirmed`：实物动作已由独立物理证据确认。

任何前一状态都不得推导后一状态。草案生成或预检成功必须显示“尚未写入设备”。

## 上传票据与引用

1. 文件须为用户主动选择并确认，大小为 1 字节至 20 MiB，扩展名和 SHA-256 经前端校验。
2. 浏览器通过 `request_document_upload` 获取一次性票据，再以 `POST /upload/v1/{ticket}`、`application/octet-stream`、`credentials: omit` 直接上传原 File。不得附加 OAuth 或连接器 Authorization 头，不追随跨源地址。
3. 解析状态通过 `get_document_status` 查询。`search_documents` 和 `extract_configuration_facts` 只返回已授权文档的有界结果与页码、表格、行列等引用。
4. 文档上下文最大 12 KiB，并以 `untrusted_document_context` 标记。引用、差异和风险在聊天中展示；上传、检索和事实提取本身不授权配置应用。
