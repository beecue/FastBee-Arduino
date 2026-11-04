(function () {
    'use strict';

    var activeJob = null;
    var activeProposal = null;
    var activeToolJob = null;
    var activeDraftId = '';
    var mcpReady = false;
    var mcpState = 'MCP_UNPAIRED';
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
    function setMcpState(state, detail) {
        mcpState = state;
        var labels = {
            AI_INACTIVE: 'AI 助手未启用', PROVIDER_REQUIRED: '请先配置模型', MCP_UNPAIRED: 'MCP 未配对',
            CONNECTING: 'MCP 连接中', CONNECTOR_ONLINE: '连接器已在线，正在验证 MCP', MCP_READY: 'MCP 已就绪',
            DEGRADED: 'MCP 降级，本地 AI 仍可用', SESSION_EXPIRED: 'MCP 会话已过期'
        };
        setStatus('ai-mcp-state', labels[state] || state, state === 'DEGRADED' || state === 'SESSION_EXPIRED');
        setStatus('ai-mcp-state-detail', detail || '', state === 'DEGRADED' || state === 'SESSION_EXPIRED');
    }
    function setMcpReady(ready) {
        mcpReady = ready === true;
        ['ai-document-file', 'ai-document-upload', 'ai-doc-search'].forEach(function (id) {
            if (byId(id)) byId(id).disabled = !mcpReady;
        });
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
        if (result.draftId) activeDraftId = result.draftId;
        if (result.draftHash || result.baseRevision || result.pendingRestart === true) {
            var metadata = document.createElement('p');
            metadata.textContent = '草案：' + activeDraftId +
                (result.draftHash ? '；draftHash=' + result.draftHash : '') +
                (result.baseRevision ? '；baseRevision=' + result.baseRevision : '') +
                (result.pendingRestart === true ? '；保存后需要重启，不会自动重启' : '');
            target.appendChild(metadata);
        }
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
        activeDraftId = '';
        setMcpReady(false);
        if (window.FastBeeAiMcpTools) FastBeeAiMcpTools.cancelAll();
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
        if (!active) setMcpState('AI_INACTIVE', '请先在功能配置中启用 AI 助手；MCP 配置会保留。');
        else if (latestConfig.apiKeyConfigured !== true) {
            setMcpState('PROVIDER_REQUIRED', '请先保存模型地址、模型名称和 API Key。');
        }
    }
    function confirmMcpRequest(request) {
        return Promise.resolve(window.confirm((request.message || '确认应用配置？') + '\n风险：' + (request.risk || '请核对差异')));
    }
    function resumeMcp(deviceId, cloudBase) {
        setMcpReady(false);
        setMcpState('CONNECTING', '正在初始化协议并核验 OAuth、工具目录和设备能力。');
        return FastBeeAiMcpTools.resume({ cloudBase: cloudBase, deviceId: deviceId,
            getBaseRevision: FastBeeAiMcpConnector.readGlobalRevision, confirm: confirmMcpRequest })
            .then(function () {
                setMcpReady(true);
                setMcpState('MCP_READY', 'OAuth、连接器和设备能力均已验证；文档按云端状态可用。');
                byId('ai-document-upload').textContent = '上传文档';
                setStatus('ai-config-status', 'MCP 已连接；令牌仅保留在当前浏览器会话中。');
                return true;
            }).catch(function (error) {
                setMcpReady(false);
                var expired = /UNAUTHENTICATED|EXPIRED|REVOKED/.test(error.message || '');
                setMcpState(expired ? 'SESSION_EXPIRED' : 'DEGRADED',
                    'MCP 初始化未完成：' + error.message + '；本地 AI 不受影响。');
                throw error;
            });
    }
    function restoreMcp() {
        if (!window.FastBeeAiMcpConnector || !window.FastBeeAiMcpTools) return Promise.resolve(false);
        var connector = FastBeeAiMcpConnector.status();
        if (!connector.paired || !connector.deviceId) {
            setMcpState('MCP_UNPAIRED', '填写 MCP 地址、OAuth 令牌和一次性配对码后开始。');
            return Promise.resolve(false);
        }
        FastBeeAiMcpConnector.start();
        return resumeMcp(connector.deviceId, connector.cloudBase || latestConfig.mcpBaseUrl)
            .catch(function () { return false; });
    }
    function beginOAuthFlow() {
        var redirectUri = window.location.href.split(/[?#]/)[0];
        FastBeeAiMcpTools.beginOAuth({
            authorizationEndpoint: byId('ai-oauth-authorize').value,
            tokenEndpoint: byId('ai-oauth-token-endpoint').value,
            clientId: byId('ai-oauth-client-id').value,
            redirectUri: redirectUri,
            allowedRedirects: [redirectUri],
            scopes: ['fastbee.read', 'fastbee.documents', 'fastbee.configure']
        }).then(function (authorization) {
            window.location.assign(authorization.url);
        }).catch(function (error) {
            setMcpState('DEGRADED', 'PKCE 授权无法启动：' + error.message + '；可改用短期 OAuth 令牌。');
        });
    }
    function completeOAuthIfNeeded() {
        var params = new URLSearchParams(window.location.search);
        if (!params.get('code') || !params.get('state')) return Promise.resolve(false);
        return FastBeeAiMcpTools.completeOAuth({ code: params.get('code'), state: params.get('state') })
            .then(function () {
                var clean = window.location.href.split(/[?#]/)[0];
                window.history.replaceState({}, document.title, clean);
                return true;
            });
    }
    function prepareMcp() {
        if (typeof ModuleLoader === 'undefined') return;
        ModuleLoader.loadModule('ai-mcp', function () {
            completeOAuthIfNeeded().catch(function (error) {
                setMcpState('DEGRADED', 'OAuth 回调处理失败：' + error.message + '；已有连接器会话不受影响。');
            }).then(function () { restoreMcp(); });
        });
    }
    function connectMcp() {
        var url = byId('ai-mcp-url').value;
        var token = byId('ai-oauth-token').value;
        var code = byId('ai-pair-code').value;
        if (!url || !token || !window.FastBeeAiMcpConnector || !window.FastBeeAiMcpTools) {
            setMcpState('MCP_UNPAIRED', '请填写 MCP 地址和 OAuth 令牌。');
            return;
        }
        var connector = FastBeeAiMcpConnector.status();
        if (connector.paired && connector.deviceId && connector.cloudBase === url) {
            byId('ai-mcp-connect').disabled = true;
            FastBeeAiMcpTools.configure({ cloudBase: url, oauthToken: token, deviceId: connector.deviceId,
                getBaseRevision: FastBeeAiMcpConnector.readGlobalRevision, confirm: confirmMcpRequest });
            FastBeeAiMcpConnector.start();
            resumeMcp(connector.deviceId, url).then(function () {
                byId('ai-pair-code').value = '';
                byId('ai-oauth-token').value = '';
            }).catch(function () {
                setMcpReady(false);
            }).finally(function () { byId('ai-mcp-connect').disabled = false; });
            return;
        }
        if (!code) {
            setMcpState('MCP_UNPAIRED', '首次配对或变更 MCP 地址时需要一次性配对码。');
            return;
        }
        byId('ai-mcp-connect').disabled = true;
        FastBeeAiMcpConnector.pair(url, code).then(function (paired) {
            if (!paired || paired.ok !== true) throw new Error((paired && paired.error) || 'MCP_PAIR_FAILED');
            FastBeeAiMcpConnector.start();
            setMcpState('CONNECTOR_ONLINE', '设备配对已完成，正在验证 MCP 会话。');
            FastBeeAiMcpTools.configure({ cloudBase: url, oauthToken: token, deviceId: paired.deviceId,
                getBaseRevision: FastBeeAiMcpConnector.readGlobalRevision, confirm: confirmMcpRequest });
            byId('ai-pair-code').value = '';
            byId('ai-oauth-token').value = '';
            return resumeMcp(paired.deviceId, url);
        }).catch(function () {
            setMcpReady(false);
        }).finally(function () { byId('ai-mcp-connect').disabled = false; });
    }
    function disconnectMcp() {
        if (window.FastBeeAiMcpConnector) FastBeeAiMcpConnector.revoke();
        if (window.FastBeeAiMcpTools) FastBeeAiMcpTools.disconnect();
        setMcpReady(false);
        setMcpState('MCP_UNPAIRED', '会话已断开；重新配对后可再次连接。');
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
    function submitMqttSecret() {
        if (!mcpReady || !window.FastBeeAiMcpConnector) return;
        var value = byId('ai-secret-value').value;
        var secretRef = byId('ai-secret-ref').value;
        var key = 'web-secret-' + Date.now() + '-' + Math.random().toString(36).slice(2, 12);
        byId('ai-secret-submit').disabled = true;
        FastBeeAiMcpConnector.submitSecret(secretRef, value, key).then(function () {
            byId('ai-secret-value').value = '';
            setStatus('ai-config-status', '秘密已仅提交到设备接口；云端和模型未接收该值。');
        }).catch(function (error) {
            setStatus('ai-config-status', '秘密提交失败：' + error.message, true);
        }).finally(function () { byId('ai-secret-submit').disabled = false; });
    }
    function testMqtt() {
        if (!mcpReady || !activeDraftId) {
            setStatus('ai-config-status', '请先让 AI 生成并预检一个 MQTT 配置草案。', true);
            return;
        }
        byId('ai-mqtt-test').disabled = true;
        FastBeeAiMcpTools.testMqttConfiguration(activeDraftId, { humanConfirmed: true }).then(function (result) {
            setStatus('ai-config-status', result.connected === true ? 'MQTT 单次连接测试成功。' : 'MQTT 连接测试未连接；请核对状态。', result.connected !== true);
        }).catch(function (error) {
            setStatus('ai-config-status', 'MQTT 测试失败：' + error.message, true);
        }).finally(function () { byId('ai-mqtt-test').disabled = false; });
    }
    function bind() {
        document.querySelectorAll('[data-ai-tab]').forEach(function (button) {
            button.addEventListener('click', function () { selectTab(button.getAttribute('data-ai-tab')); });
        });
        byId('ai-send').addEventListener('click', send);
        byId('ai-cancel').addEventListener('click', cancel);
        byId('ai-mcp-connect').addEventListener('click', connectMcp);
        byId('ai-oauth-begin').addEventListener('click', beginOAuthFlow);
        byId('ai-mcp-resume').addEventListener('click', function () { restoreMcp(); });
        byId('ai-mcp-disconnect').addEventListener('click', disconnectMcp);
        byId('ai-secret-submit').addEventListener('click', submitMqttSecret);
        byId('ai-mqtt-test').addEventListener('click', testMqtt);
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
