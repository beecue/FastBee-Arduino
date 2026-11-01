(function () {
    'use strict';

    var activeJob = null;
    var activeProposal = null;
    var activeToolJob = null;
    var mcpReady = false;
    var documentContext = '';
    var leaving = false;
    var pollTimer = null;
    var pagehideHandler = null;
    var toolResultHandler = null;
    var latestConfig = { runtimeReason: 'not_loaded' };
    var clientSessionId = 'ai-' + Date.now() + '-' + Math.random().toString(36).slice(2);

    function byId(id) { return document.getElementById(id); }
    function setStatus(id, text, error) {
        var element = byId(id);
        if (!element) return;
        element.textContent = text || '';
        element.classList.toggle('ai-error', !!error);
    }
    function show(id, visible) {
        var element = byId(id);
        if (element) element.classList.toggle('ai-hidden', !visible);
    }
    function unwrap(response) {
        if (!response || response.success !== true) {
            var error = new Error((response && (response.code || response.error)) || 'AI_REQUEST_FAILED');
            error.status = response && response.status;
            throw error;
        }
        return response.data === undefined ? response : response.data;
    }
    function appendMessage(role, text) {
        var list = byId('ai-messages');
        if (!list) return;
        var row = document.createElement('div');
        row.className = 'ai-message';
        row.dataset.role = role;
        row.textContent = (role === 'user' ? '我：' : 'AI：') + text;
        list.appendChild(row);
        list.scrollTop = list.scrollHeight;
    }
    function renderToolResult(detail) {
        var result = detail.result || {};
        var target = byId('ai-tool-results');
        if (!target) return;
        target.textContent = '';
        var citations = result.citations || (result.items || []).map(function (item) { return item.citation; }).filter(Boolean);
        if (citations.length) {
            var sources = document.createElement('p');
            sources.textContent = '来源：' + citations.map(function (cite) {
                return cite.documentId + (cite.page ? ' 第' + cite.page + '页' : '') +
                    (cite.sheet ? ' ' + cite.sheet : '') + (cite.row ? ' 行' + cite.row : '');
            }).join('；');
            target.appendChild(sources);
        }
        if (result.diff) {
            var diff = document.createElement('pre');
            diff.textContent = '差异：' + JSON.stringify(result.diff);
            target.appendChild(diff);
        }
        var risk = document.createElement('p');
        risk.textContent = '风险：' + (result.risk || '按草案与预检结果核对') + '；文档内容仅供参考，不作为指令。';
        target.appendChild(risk);
        if (detail.name !== 'apply_configuration_draft') {
            var state = document.createElement('p');
            state.textContent = result.draftId ? '草案 ' + result.draftId + ' 尚未写入设备。' : '工具结果尚未写入设备。';
            target.appendChild(state);
        }
        if (result.draftId && detail.name !== 'apply_configuration_draft') {
            var apply = document.createElement('button');
            apply.id = 'ai-apply-draft';
            apply.type = 'button';
            apply.className = 'fb-btn fb-btn-primary';
            apply.textContent = '确认应用草案';
            apply.addEventListener('click', function () {
                apply.disabled = true;
                FastBeeAiMcpTools.applyDraft(result.draftId, { humanConfirmed: true }).then(function (applied) {
                    var step = applied.steps && applied.steps[0] ? applied.steps[0] : applied;
                    renderExecution(step);
                    setStatus('ai-chat-status', '应用流程已返回；各阶段状态分别显示，物理结果仍未确认。');
                }).catch(function (error) {
                    apply.disabled = false;
                    setStatus('ai-chat-status', '应用未执行：' + error.message, true);
                });
            });
            target.appendChild(apply);
        }
        show('ai-tool-results', true);
    }
    function stopPolling() {
        if (pollTimer) clearTimeout(pollTimer);
        pollTimer = null;
    }
    function resetView() {
        stopPolling();
        activeJob = null;
        activeProposal = null;
        activeToolJob = null;
        mcpReady = false;
        if (window.FastBeeAiMcpTools) FastBeeAiMcpTools.cancelAll();
        ['ai-document-file', 'ai-document-upload', 'ai-doc-search'].forEach(function (id) {
            if (byId(id)) byId(id).disabled = true;
        });
        show('ai-proposal', false);
        show('ai-execution-result', false);
        if (byId('ai-cancel')) byId('ai-cancel').disabled = true;
    }
    function selectTab(name) {
        document.querySelectorAll('[data-ai-tab]').forEach(function (button) {
            var selected = button.getAttribute('data-ai-tab') === name;
            button.classList.toggle('fb-btn-primary', selected);
            button.setAttribute('aria-selected', selected ? 'true' : 'false');
        });
        byId('ai-chat-panel').classList.toggle('ai-active', name === 'chat');
        byId('ai-config-panel').classList.toggle('ai-active', name === 'config');
    }
    function applyActiveState() {
        var active = latestConfig.active === true;
        ['ai-message', 'ai-send'].forEach(function (id) { if (byId(id)) byId(id).disabled = !active; });
        show('ai-feature-hint', !active);
    }
    function prepareMcp() {
        if (!latestConfig.mcpBaseUrl || typeof ModuleLoader === 'undefined') return;
        ModuleLoader.loadModule('ai-mcp', function () {
            setStatus('ai-config-status', 'MCP 模块已就绪；配对前本地模型仍可独立使用。');
        });
    }
    function connectMcp() {
        var url = byId('ai-mcp-url').value;
        var token = byId('ai-oauth-token').value;
        var code = byId('ai-pair-code').value;
        if (!url || !token || !code || !window.FastBeeAiMcpConnector || !window.FastBeeAiMcpTools) {
            setStatus('ai-config-status', '请填写 MCP 地址、OAuth 令牌和一次性配对码。', true);
            return;
        }
        byId('ai-mcp-connect').disabled = true;
        FastBeeAiMcpConnector.pair(url, code).then(function (paired) {
            if (!paired || paired.ok !== true) throw new Error((paired && paired.error) || 'MCP_PAIR_FAILED');
            FastBeeAiMcpTools.configure({ cloudBase: url, oauthToken: token, confirm: function (request) {
                return Promise.resolve(window.confirm((request.message || '确认应用配置？') + '\n风险：' + (request.risk || '请核对差异')));
            } });
            FastBeeAiMcpConnector.start();
            mcpReady = true;
            byId('ai-pair-code').value = '';
            byId('ai-oauth-token').value = '';
            ['ai-document-file', 'ai-document-upload', 'ai-doc-search'].forEach(function (id) { byId(id).disabled = false; });
            byId('ai-document-upload').textContent = '上传文档';
            setStatus('ai-config-status', 'MCP 已连接；令牌仅保留在浏览器会话中。');
        }).catch(function (error) {
            mcpReady = false;
            setStatus('ai-config-status', 'MCP 连接失败；本地 AI 不受影响：' + error.message, true);
        }).finally(function () { byId('ai-mcp-connect').disabled = false; });
    }
    function loadConfig() {
        return apiGet('/api/ai/config').then(unwrap).then(function (config) {
            latestConfig = config;
            byId('ai-base-url').value = config.baseUrl || '';
            byId('ai-model').value = config.model || '';
            byId('ai-mcp-url').value = config.mcpBaseUrl || '';
            applyActiveState();
            prepareMcp();
        }).catch(function (error) {
            latestConfig = { active: false, runtimeReason: 'config_unavailable' };
            applyActiveState();
            setStatus('ai-config-status', '连接配置加载失败：' + error.message, true);
        });
    }
    function renderExecution(result) {
        var target = byId('ai-execution-result');
        target.textContent = '';
        var rows = [
            ['已接受', result.accepted === true],
            ['已持久化', result.persisted === true],
            ['配置回读一致', result.readback === true],
            ['采样/运行已观察', result.observed === true],
            ['物理负载已动作', result.physicalConfirmed === true]
        ];
        rows.forEach(function (row) {
            var item = document.createElement('div');
            item.textContent = row[0] + '：' + (row[1] ? '是' : (row[0] === '物理负载已动作' ? '物理动作未确认' : '否/待确认'));
            target.appendChild(item);
        });
        show('ai-execution-result', true);
    }
    function pollJob() {
        if (!activeJob) return;
        var jobId = activeJob;
        apiGet('/api/ai/jobs/' + encodeURIComponent(jobId)).then(unwrap).then(function (job) {
            if (activeJob !== jobId) return;
            if (job.state === 'awaiting_tool') {
                setStatus('ai-chat-status', '等待受控工具结果…');
                if (!activeToolJob) {
                    activeToolJob = jobId;
                    FastBeeAiMcpTools.executeJob(job).then(function () {
                        activeToolJob = null;
                        if (activeJob === jobId) pollTimer = setTimeout(pollJob, 50);
                    }).catch(function (error) {
                        activeToolJob = null;
                        activeJob = null;
                        byId('ai-send').disabled = latestConfig.active !== true;
                        setStatus('ai-chat-status', '工具结果未接受：' + error.message, true);
                    });
                }
                return;
            }
            if (job.state === 'queued' || job.state === 'running') {
                setStatus('ai-chat-status', '模型处理中…');
                pollTimer = setTimeout(pollJob, 250);
                return;
            }
            activeToolJob = null;
            activeJob = null;
            byId('ai-cancel').disabled = true;
            byId('ai-send').disabled = latestConfig.active !== true;
            if (job.state === 'failed' || job.state === 'cancelled') {
                setStatus('ai-chat-status', '任务未执行：' + (job.error || job.state), true);
                return;
            }
            appendMessage('assistant', job.text || '已完成');
            setStatus('ai-chat-status', '');
            if (job.proposalId) {
                activeProposal = job.proposalId;
                byId('ai-proposal-summary').textContent = job.text || '配置草案尚未写入设备。';
                show('ai-proposal', true);
            }
        }).catch(function (error) {
            activeJob = null;
            byId('ai-send').disabled = latestConfig.active !== true;
            setStatus('ai-chat-status', '任务状态读取失败：' + error.message, true);
        });
    }
    function send() {
        var input = byId('ai-message');
        var message = input.value.trim();
        if (!message || activeJob || latestConfig.active !== true) return;
        appendMessage('user', message);
        input.value = '';
        byId('ai-send').disabled = true;
        byId('ai-cancel').disabled = false;
        setStatus('ai-chat-status', '正在创建任务…');
        var payload = { message: message, clientSessionId: clientSessionId, mcpEnabled: mcpReady };
        if (mcpReady && documentContext) payload.documentContext = documentContext;
        apiPost('/api/ai/jobs', payload).then(unwrap).then(function (job) {
            if (leaving) {
                apiPost('/api/ai/jobs/' + encodeURIComponent(job.jobId) + '/cancel', {}).catch(function () {});
                return;
            }
            activeJob = job.jobId;
            pollJob();
        }).catch(function (error) {
            byId('ai-send').disabled = false;
            byId('ai-cancel').disabled = true;
            setStatus('ai-chat-status', '发送失败：' + error.message, true);
        });
    }
    function cancel() {
        if (!activeJob) return;
        var jobId = activeJob;
        stopPolling();
        activeJob = null;
        if (window.FastBeeAiMcpTools) FastBeeAiMcpTools.cancel(jobId);
        activeToolJob = null;
        byId('ai-cancel').disabled = true;
        byId('ai-send').disabled = latestConfig.active !== true;
        apiPost('/api/ai/jobs/' + encodeURIComponent(jobId) + '/cancel', {}).catch(function () {});
        setStatus('ai-chat-status', '已请求取消；网络请求可能仍需等待超时结束。');
    }
    function bind() {
        document.querySelectorAll('[data-ai-tab]').forEach(function (button) {
            button.addEventListener('click', function () { selectTab(button.getAttribute('data-ai-tab')); });
        });
        byId('ai-send').addEventListener('click', send);
        byId('ai-cancel').addEventListener('click', cancel);
        byId('ai-mcp-connect').addEventListener('click', connectMcp);
        byId('ai-document-upload').addEventListener('click', function () {
            var file = byId('ai-document-file').files[0];
            if (!mcpReady || !file) return;
            setStatus('ai-chat-status', '正在申请上传票据并直传云端…');
            FastBeeAiMcpDocuments.upload(file, { humanConfirmed: true }).then(function (status) {
                setStatus('ai-chat-status', '文档状态：' + (status.state || '已上传，等待解析'));
            }).catch(function (error) { setStatus('ai-chat-status', '上传失败：' + error.message, true); });
        });
        byId('ai-doc-search').addEventListener('click', function () {
            if (!mcpReady) return;
            var query = byId('ai-doc-query').value.trim();
            FastBeeAiMcpDocuments.search(query).then(function (result) {
                documentContext = FastBeeAiMcpDocuments.buildDocumentContext(result);
                renderToolResult({ name: 'search_documents', result: result });
                setStatus('ai-chat-status', '已生成带引用的不可信文档上下文；不会自动应用配置。');
            }).catch(function (error) { setStatus('ai-chat-status', '检索失败：' + error.message, true); });
        });
        byId('ai-deepseek-preset').addEventListener('click', function () {
            byId('ai-base-url').value = 'https://api.deepseek.com';
            byId('ai-model').value = 'deepseek-chat';
        });
        byId('ai-provider-form').addEventListener('submit', function (event) {
            event.preventDefault();
            setStatus('ai-config-status', '正在保存本地模型配置…');
            apiPut('/api/ai/config', {
                baseUrl: byId('ai-base-url').value,
                model: byId('ai-model').value,
                apiKey: byId('ai-api-key').value,
                clearApiKey: byId('ai-clear-key').checked
            }).then(unwrap).then(function () {
                byId('ai-api-key').value = '';
                byId('ai-clear-key').checked = false;
                setStatus('ai-config-status', '本地模型配置已保存。');
            }).catch(function (error) { setStatus('ai-config-status', '保存失败：' + error.message, true); });
        });
        byId('ai-mcp-form').addEventListener('submit', function (event) {
            event.preventDefault();
            apiPut('/api/ai/mcp-config', { mcpBaseUrl: byId('ai-mcp-url').value }).then(unwrap).then(function () {
                setStatus('ai-config-status', 'MCP 地址已独立保存；本地模型配置未改变。');
            }).catch(function (error) { setStatus('ai-config-status', 'MCP 保存失败：' + error.message, true); });
        });
        byId('ai-confirm-proposal').addEventListener('click', function () {
            if (!activeProposal) return;
            var proposalId = activeProposal;
            byId('ai-confirm-proposal').disabled = true;
            apiPost('/api/ai/proposals/' + encodeURIComponent(proposalId) + '/execute', { confirmationText: '确认执行' })
                .then(unwrap).then(function (result) {
                    activeProposal = null;
                    show('ai-proposal', false);
                    renderExecution(result);
                }).catch(function (error) {
                    byId('ai-confirm-proposal').disabled = false;
                    setStatus('ai-chat-status', '执行未获准或失败：' + error.message, true);
                });
        });
        byId('ai-issue-draft').addEventListener('click', function () {
            var preview = byId('ai-issue-preview');
            preview.value = 'FastBee AI 问题草稿\n功能：aiAssistant\nactive：' + (latestConfig.active === true) +
                '\nruntimeReason：' + (latestConfig.runtimeReason || 'unknown') +
                '\n错误码：' + (byId('ai-chat-status').textContent || 'none') +
                '\n（未包含聊天、凭据、完整配置或日志）';
            show('ai-issue-preview', true);
        });
        if (toolResultHandler) window.removeEventListener('fastbee:ai-tool-result', toolResultHandler);
        toolResultHandler = function (event) { renderToolResult(event.detail || {}); };
        window.addEventListener('fastbee:ai-tool-result', toolResultHandler);
        if (pagehideHandler) window.removeEventListener('pagehide', pagehideHandler);
        pagehideHandler = function () { leaving = true; cancel(); resetView(); };
        window.addEventListener('pagehide', pagehideHandler);
    }

    AppState.registerModule('ai', {
        loadAiPage: function () {
            leaving = false;
            resetView();
            bind();
            return loadConfig();
        },
        unloadAiPage: resetView
    });
})();
