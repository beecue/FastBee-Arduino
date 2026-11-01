(function () {
    'use strict';

    var cloudBase = '';
    var oauthToken = '';
    var confirmHandler = null;
    var defaultTimeoutMs = 20000;
    var nextId = 1;
    var active = Object.create(null);
    var listenersBound = false;
    var MODEL_TOOLS = {
        get_device_capabilities: [], list_peripherals: [], read_peripheral: ['peripheralId'],
        list_execution_rules: [], list_rule_scripts: [], get_protocol_configuration: [],
        get_configuration_history: [], create_config_draft: ['deviceId'],
        get_configuration_draft: ['draftId', 'expectedVersion'], upsert_peripheral: ['draftId', 'expectedVersion', 'peripheral'],
        remove_peripheral: ['draftId', 'expectedVersion', 'peripheralId'],
        set_peripheral_enabled: ['draftId', 'expectedVersion', 'peripheralId', 'enabled'],
        validate_configuration_draft: ['draftId', 'expectedVersion'], get_configuration_operation: ['operationId'],
        request_document_upload: ['fileName', 'size', 'contentType', 'sha256'], list_documents: ['page', 'pageSize'],
        get_document_status: ['documentId'], search_documents: ['query', 'documentId', 'page', 'pageSize'],
        extract_configuration_facts: ['documentId', 'kind', 'page', 'pageSize']
    };
    var REQUIRED = {
        read_peripheral: ['peripheralId'], create_config_draft: ['deviceId'],
        get_configuration_draft: ['draftId'], validate_configuration_draft: ['draftId'],
        get_configuration_operation: ['operationId'], upsert_peripheral: ['draftId', 'expectedVersion', 'peripheral'],
        remove_peripheral: ['draftId', 'expectedVersion', 'peripheralId'],
        set_peripheral_enabled: ['draftId', 'expectedVersion', 'peripheralId', 'enabled'],
        get_document_status: ['documentId'], search_documents: ['query'], extract_configuration_facts: ['documentId'],
        request_document_upload: ['fileName', 'size', 'contentType', 'sha256']
    };

    function isObject(value) { return !!value && typeof value === 'object' && !Array.isArray(value); }
    function error(code) { return new Error(code || 'MCP_UNAVAILABLE'); }
    function utf8Size(text) {
        return typeof TextEncoder === 'function' ? new TextEncoder().encode(text).length : unescape(encodeURIComponent(text)).length;
    }
    function validateModelCall(call) {
        if (!isObject(call) || Object.keys(call).length !== 3 || typeof call.toolCallId !== 'string' ||
            !call.toolCallId || call.toolCallId.length > 128 || typeof call.name !== 'string' ||
            !Object.prototype.hasOwnProperty.call(MODEL_TOOLS, call.name) || !isObject(call.arguments)) {
            throw error('AI_INVALID_TOOL_CALL');
        }
        var allowed = MODEL_TOOLS[call.name];
        var keys = Object.keys(call.arguments);
        for (var i = 0; i < keys.length; i++) {
            if (allowed.indexOf(keys[i]) < 0) throw error('AI_INVALID_TOOL_ARGUMENTS');
        }
        var required = REQUIRED[call.name] || [];
        for (var j = 0; j < required.length; j++) {
            var value = call.arguments[required[j]];
            if (value === undefined || value === null || value === '') throw error('AI_INVALID_TOOL_ARGUMENTS');
        }
        if (call.name === 'request_document_upload') {
            var size = call.arguments.size;
            var digest = call.arguments.sha256;
            if (!Number.isInteger(size) || size < 1 || size > 20971520 || !/^[0-9a-fA-F]{64}$/.test(digest)) {
                throw error('AI_INVALID_TOOL_ARGUMENTS');
            }
        }
    }
    function configure(options) {
        options = options || {};
        cloudBase = typeof options.cloudBase === 'string' ? options.cloudBase.replace(/\/+$/, '') : '';
        if (!/^https:\/\//.test(cloudBase)) throw error('AI_INVALID_MCP_CONFIG');
        oauthToken = typeof options.oauthToken === 'string' ? options.oauthToken : '';
        if (!oauthToken || oauthToken.length > 2048 || /[\r\n]/.test(oauthToken)) throw error('MCP_UNAUTHENTICATED');
        confirmHandler = typeof options.confirm === 'function' ? options.confirm : null;
        defaultTimeoutMs = Number.isInteger(options.timeoutMs) ? Math.min(120000, Math.max(1, options.timeoutMs)) : 20000;
        try { sessionStorage.setItem('fastbee.mcp.oauth.session', oauthToken); } catch (_) {}
        if (!listenersBound) {
            listenersBound = true;
            window.addEventListener('pagehide', cancelAll);
        }
    }
    function post(payload, signal) {
        var controller = new AbortController();
        var abort = function () { controller.abort(); };
        if (signal) signal.addEventListener('abort', abort, { once: true });
        return window.fetch(cloudBase + '/mcp', {
            method: 'POST', credentials: 'omit', signal: controller.signal,
            headers: { 'Content-Type': 'application/json', 'Authorization': 'Bearer ' + oauthToken },
            body: JSON.stringify(payload)
        }).then(function (response) {
            return response.text().then(function (text) {
                if (utf8Size(text) > 65536) throw error('MCP_RESULT_TOO_LARGE');
                var body;
                try { body = JSON.parse(text); } catch (_) { throw error('MCP_INVALID_RESPONSE'); }
                if (!response.ok) throw error((body && body.code) || 'MCP_UNAVAILABLE');
                return body;
            });
        }).finally(function () {
            if (signal) signal.removeEventListener('abort', abort);
        });
    }
    function rpc(name, args, trusted, signal) {
        var id = nextId++;
        return post({ jsonrpc: '2.0', id: id, method: 'tools/call', params: { name: name, arguments: args } }, signal)
            .then(function message(body) {
                if (body && body.method === 'elicitation/create') {
                    if (!trusted || !confirmHandler) return Promise.reject(error('AI_CONFIRMATION_REQUIRED'));
                    return Promise.resolve(confirmHandler({ message: body.params && body.params.message,
                        risk: body.params && body.params.risk })).then(function (accepted) {
                        if (!accepted) throw error('CONFIRMATION_REJECTED');
                        return post({ jsonrpc: '2.0', id: body.id, result: {
                            action: 'accept', content: { confirmationText: '确认执行' } } }, signal).then(message);
                    });
                }
                if (!body || body.id !== id) throw error('MCP_INVALID_RESPONSE');
                if (body.error) throw error((typeof body.error.message === 'string' && body.error.message) || body.error.code || 'MCP_TOOL_FAILED');
                if (!isObject(body.result)) throw error('MCP_INVALID_RESPONSE');
                return body.result;
            });
    }
    function executeJob(job) {
        if (!job || typeof job.jobId !== 'string' || job.state !== 'awaiting_tool' || active[job.jobId]) {
            return Promise.reject(error('AI_INVALID_JOB'));
        }
        var call = job.toolCall;
        try { validateModelCall(call); } catch (e) { return Promise.reject(e); }
        var controller = new AbortController();
        var timeout = setTimeout(function () { controller.abort('timeout'); }, defaultTimeoutMs);
        active[job.jobId] = controller;
        var resultText = '';
        var resultError = '';
        return rpc(call.name, call.arguments, false, controller.signal).then(function (result) {
            resultText = JSON.stringify(result);
            if (utf8Size(resultText) > 16384) throw error('MCP_RESULT_TOO_LARGE');
        }).catch(function (e) {
            resultError = e && e.name === 'AbortError' ? 'MCP_TIMEOUT' : (e && e.message) || 'MCP_TOOL_FAILED';
            resultError = resultError.slice(0, 128);
        }).then(function () {
            var body = { toolCallId: call.toolCallId };
            if (resultError) body.error = resultError;
            else body.result = resultText;
            return apiPost('/api/ai/jobs/' + encodeURIComponent(job.jobId) + '/tool-result', body);
        }).then(function (response) {
            if (!response || response.success !== true || !response.data || response.data.accepted !== true) {
                throw error((response && (response.code || response.error)) || 'AI_STALE_TOOL_RESULT');
            }
            if (resultError) return { error: resultError };
            var detail = { jobId: job.jobId, toolCallId: call.toolCallId, name: call.name, result: JSON.parse(resultText) };
            window.dispatchEvent(new CustomEvent('fastbee:ai-tool-result', { detail: detail }));
            return detail.result;
        }).finally(function () {
            clearTimeout(timeout);
            delete active[job.jobId];
        });
    }
    function applyDraft(draftId, options) {
        if (!options || options.humanConfirmed !== true || typeof draftId !== 'string' || !draftId) {
            return Promise.reject(error('AI_CONFIRMATION_REQUIRED'));
        }
        var key = 'apply:' + draftId;
        if (active[key]) return Promise.reject(error('AI_JOB_ACTIVE'));
        var controller = new AbortController();
        active[key] = controller;
        var timeout = setTimeout(function () { controller.abort(); }, defaultTimeoutMs);
        return rpc('apply_configuration_draft', { draftId: draftId }, true, controller.signal).then(function (result) {
            window.dispatchEvent(new CustomEvent('fastbee:ai-tool-result', { detail: {
                draftId: draftId, name: 'apply_configuration_draft', result: result, humanConfirmed: true } }));
            return result;
        }).catch(function (e) {
            if (e && e.name === 'AbortError') throw error('MCP_TIMEOUT');
            throw e;
        }).finally(function () {
            clearTimeout(timeout);
            delete active[key];
        });
    }
    function callTrustedTool(name, args, signal) {
        if (name === 'apply_configuration_draft') return Promise.reject(error('AI_CONFIRMATION_REQUIRED'));
        return rpc(name, args, false, signal);
    }
    function cancel(jobId) {
        if (active[jobId]) {
            active[jobId].abort();
            delete active[jobId];
        }
    }
    function cancelAll() {
        Object.keys(active).forEach(function (jobId) {
            active[jobId].abort();
            delete active[jobId];
        });
    }

    window.FastBeeAiMcpTools = {
        configure: configure,
        executeJob: executeJob,
        applyDraft: applyDraft,
        callTrustedTool: callTrustedTool,
        cancel: cancel,
        cancelAll: cancelAll,
        activeCount: function () { return Object.keys(active).length; },
        baseUrl: function () { return cloudBase; }
    };
})();
