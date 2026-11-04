(function () {
    'use strict';

    var cloudBase = '';
    var oauthToken = '';
    var pairedDeviceId = '';
    var baseRevisionProvider = null;
    var confirmHandler = null;
    var defaultTimeoutMs = 20000;
    var nextId = 1;
    var active = Object.create(null);
    var listenersBound = false;
    var MCP_PROTOCOL_VERSION = '2025-06-18';
    var sessionId = '';
    var initializing = null;
    var MODEL_TOOLS = {
        get_device_capabilities: [], get_device_config_section: ['section'], get_device_status: [],
        list_peripherals: ['cursor', 'limit'], read_peripheral: ['peripheralId'],
        list_execution_rules: ['cursor', 'limit'], list_rule_scripts: ['cursor', 'limit'], get_protocol_configuration: [],
        get_configuration_history: ['cursor', 'limit'], create_config_draft: [],
        get_configuration_draft: ['draftId'], upsert_peripheral: ['draftId', 'expectedVersion', 'peripheral'],
        remove_peripheral: ['draftId', 'expectedVersion', 'peripheralId'],
        set_peripheral_enabled: ['draftId', 'expectedVersion', 'peripheralId', 'enabled'],
        update_device_profile: ['draftId', 'expectedVersion', 'profile'],
        configure_runtime_features: ['draftId', 'expectedVersion', 'features'],
        upsert_execution_rule: ['draftId', 'expectedVersion', 'rule'],
        remove_execution_rule: ['draftId', 'expectedVersion', 'ruleId'],
        dry_run_execution_rule: ['draftId', 'ruleId', 'inputs'],
        upsert_rule_script: ['draftId', 'expectedVersion', 'script', 'variables'],
        remove_rule_script: ['draftId', 'expectedVersion', 'scriptId'],
        configure_modbus_rtu: ['draftId', 'expectedVersion', 'bus'],
        upsert_modbus_device: ['draftId', 'expectedVersion', 'device'],
        remove_modbus_device: ['draftId', 'expectedVersion', 'slaveAddress'],
        upsert_modbus_poll_task: ['draftId', 'expectedVersion', 'taskIndex', 'task', 'source'],
        remove_modbus_poll_task: ['draftId', 'expectedVersion', 'taskIndex'],
        configure_mqtt: ['draftId', 'expectedVersion', 'connection'],
        upsert_mqtt_topic: ['draftId', 'expectedVersion', 'list', 'topic'],
        remove_mqtt_topic: ['draftId', 'expectedVersion', 'list', 'topic'],
        validate_configuration_draft: ['draftId'], preview_configuration_draft: ['draftId'],
        refresh_device_capabilities: [], get_configuration_operation: ['operationId'],
        list_workflow_templates: [], start_configuration_workflow: ['templateId', 'input'],
        get_workflow_run: ['runId'], cancel_workflow_run: ['runId'],
        request_document_upload: ['fileName', 'sizeBytes', 'sha256'], list_documents: ['cursor', 'limit'],
        get_document_status: ['documentId'], search_documents: ['query', 'documentId', 'cursor', 'limit'],
        extract_configuration_facts: ['documentId', 'kind'], parse_document: ['documentId'],
        retry_document_parsing: ['documentId'], get_operation_audit: ['tool', 'resultCode', 'pageNum', 'pageSize'],
        submit_feedback: ['category', 'message']
    };
    var REQUIRED = {
        get_device_config_section: ['section'], read_peripheral: ['peripheralId'],
        get_configuration_draft: ['draftId'], validate_configuration_draft: ['draftId'],
        preview_configuration_draft: ['draftId'], get_configuration_operation: ['operationId'],
        upsert_peripheral: ['draftId', 'expectedVersion', 'peripheral'],
        remove_peripheral: ['draftId', 'expectedVersion', 'peripheralId'],
        set_peripheral_enabled: ['draftId', 'expectedVersion', 'peripheralId', 'enabled'],
        update_device_profile: ['draftId', 'expectedVersion', 'profile'],
        configure_runtime_features: ['draftId', 'expectedVersion', 'features'],
        upsert_execution_rule: ['draftId', 'expectedVersion', 'rule'],
        remove_execution_rule: ['draftId', 'expectedVersion', 'ruleId'],
        dry_run_execution_rule: ['draftId', 'ruleId'],
        upsert_rule_script: ['draftId', 'expectedVersion', 'script'],
        remove_rule_script: ['draftId', 'expectedVersion', 'scriptId'],
        configure_modbus_rtu: ['draftId', 'expectedVersion', 'bus'],
        upsert_modbus_device: ['draftId', 'expectedVersion', 'device'],
        remove_modbus_device: ['draftId', 'expectedVersion', 'slaveAddress'],
        upsert_modbus_poll_task: ['draftId', 'expectedVersion', 'taskIndex', 'task'],
        remove_modbus_poll_task: ['draftId', 'expectedVersion', 'taskIndex'],
        configure_mqtt: ['draftId', 'expectedVersion', 'connection'],
        upsert_mqtt_topic: ['draftId', 'expectedVersion', 'list', 'topic'],
        remove_mqtt_topic: ['draftId', 'expectedVersion', 'list', 'topic'],
        start_configuration_workflow: ['templateId'], get_workflow_run: ['runId'], cancel_workflow_run: ['runId'],
        get_document_status: ['documentId'], search_documents: ['query'], extract_configuration_facts: ['documentId'],
        request_document_upload: ['fileName', 'sizeBytes', 'sha256'], parse_document: ['documentId'],
        retry_document_parsing: ['documentId'], submit_feedback: ['category', 'message']
    };
    var DEVICE_CONTEXT_TOOLS = ['get_device_capabilities', 'get_device_config_section', 'get_device_status',
        'list_peripherals', 'read_peripheral', 'list_execution_rules', 'list_rule_scripts', 'get_protocol_configuration',
        'get_configuration_history', 'create_config_draft', 'update_device_profile', 'configure_runtime_features',
        'upsert_execution_rule', 'remove_execution_rule', 'upsert_rule_script', 'remove_rule_script',
        'configure_modbus_rtu', 'upsert_modbus_device', 'remove_modbus_device', 'upsert_modbus_poll_task',
        'remove_modbus_poll_task', 'configure_mqtt', 'upsert_mqtt_topic', 'remove_mqtt_topic',
        'refresh_device_capabilities', 'get_operation_audit', 'submit_feedback'];

    function isObject(value) { return !!value && typeof value === 'object' && !Array.isArray(value); }
    function error(code) { return new Error(code || 'MCP_UNAVAILABLE'); }
    function utf8Size(text) {
        return typeof TextEncoder === 'function' ? new TextEncoder().encode(text).length : unescape(encodeURIComponent(text)).length;
    }
    function throwIfAborted(signal) {
        if (signal && signal.aborted) throw error('MCP_TIMEOUT');
    }
    function normalizeError(e, signal) {
        if (signal && signal.aborted) return 'MCP_TIMEOUT';
        var name = e && e.name ? String(e.name) : '';
        var message = e && e.message ? String(e.message) : '';
        if (name === 'AbortError' || name === 'TimeoutError' || /timeout|timed out|abort/i.test(message)) return 'MCP_TIMEOUT';
        return message || 'MCP_TOOL_FAILED';
    }
    function timeoutForTool(name) {
        if (defaultTimeoutMs > 0) return defaultTimeoutMs;
        if (['apply_configuration_draft', 'rollback_configuration_operation', 'start_configuration_workflow',
            'parse_document', 'retry_document_parsing', 'configure_runtime_features'].indexOf(name) >= 0) return 130000;
        if (['create_config_draft', 'validate_configuration_draft', 'upsert_peripheral', 'remove_peripheral',
            'set_peripheral_enabled'].indexOf(name) >= 0) return 45000;
        return 25000;
    }
    var pendingSseStreams = [];
    function closePendingSseStreams() {
        pendingSseStreams.forEach(function (stream) {
            stream.done = true;
            stream.reader.cancel().catch(function () {});
        });
        pendingSseStreams = [];
    }
    function parseSseEvent(eventText) {
        var data = [];
        eventText.split(/\r?\n/).forEach(function (line) {
            if (/^data:/.test(line)) data.push(line.slice(5).replace(/^ /, ''));
        });
        if (!data.length || data.join('\n') === '[DONE]') return null;
        try { return JSON.parse(data.join('\n')); } catch (_) { throw error('MCP_INVALID_RESPONSE'); }
    }
    function readSseMessage(stream, signal) {
        throwIfAborted(signal);
        if (stream.done) return Promise.reject(error('MCP_INVALID_RESPONSE'));
        var separator = /\r?\n\r?\n/.exec(stream.buffer);
        if (separator) {
            var eventText = stream.buffer.slice(0, separator.index);
            stream.buffer = stream.buffer.slice(separator.index + separator[0].length);
            var message = parseSseEvent(eventText);
            if (message) return Promise.resolve(message);
        }
        if (stream.closed) {
            stream.done = true;
            var remaining = stream.buffer;
            stream.buffer = '';
            var finalMessage = remaining ? parseSseEvent(remaining) : null;
            if (finalMessage) return Promise.resolve(finalMessage);
            return Promise.reject(error('MCP_INVALID_RESPONSE'));
        }
        var onAbort = function () { stream.reader.cancel().catch(function () {}); };
        if (signal) signal.addEventListener('abort', onAbort, { once: true });
        return stream.reader.read().then(function (part) {
            if (part.done) {
                stream.closed = true;
                stream.buffer += new TextDecoder().decode(part.value || new Uint8Array());
                return readSseMessage(stream, signal);
            }
            var value = part.value || new Uint8Array();
            stream.total += value.byteLength;
            if (stream.total > stream.maxBytes) {
                stream.done = true;
                return stream.reader.cancel().then(function () { throw error('MCP_RESULT_TOO_LARGE'); });
            }
            stream.buffer += new TextDecoder().decode(value, { stream: true });
            return readSseMessage(stream, signal);
        }).catch(function (e) {
            if (signal && signal.aborted) throw error('MCP_TIMEOUT');
            throw e;
        }).finally(function () {
            if (signal) signal.removeEventListener('abort', onAbort);
        });
    }
    function readPendingSseMessage(signal) {
        while (pendingSseStreams.length && pendingSseStreams[pendingSseStreams.length - 1].done) {
            pendingSseStreams.pop();
        }
        if (!pendingSseStreams.length) return Promise.reject(error('MCP_INVALID_RESPONSE'));
        return readSseMessage(pendingSseStreams[pendingSseStreams.length - 1], signal);
    }
    function readJsonResponse(response, maxBytes, signal, trackSse) {
        maxBytes = maxBytes || 65536;
        throwIfAborted(signal);
        var declared = Number(response.headers && response.headers.get('Content-Length'));
        if (Number.isFinite(declared) && declared > maxBytes) {
            if (response.body && typeof response.body.cancel === 'function') response.body.cancel().catch(function () {});
            throw error('MCP_RESULT_TOO_LARGE');
        }
        var contentType = response.headers && response.headers.get('Content-Type') || '';
        var isSse = /^text\/event-stream\b/i.test(contentType);
        function checkResponse(body) {
            if (!response.ok) {
                var statusCode = response.status === 401 ? 'MCP_UNAUTHENTICATED' :
                    (response.status === 403 ? 'FORBIDDEN' : 'MCP_UNAVAILABLE');
                throw error((body && body.code) || statusCode);
            }
            return body;
        }
        if (isSse && response.body && typeof response.body.getReader === 'function') {
            var stream = { reader: response.body.getReader(), buffer: '', total: 0, maxBytes: maxBytes,
                closed: false, done: false };
            if (trackSse) pendingSseStreams.push(stream);
            return readSseMessage(stream, signal).then(checkResponse).catch(function (e) {
                stream.reader.cancel().catch(function () {});
                stream.done = true;
                throw e;
            });
        }
        return response.text().then(function (text) {
            if (utf8Size(text) > maxBytes) throw error('MCP_RESULT_TOO_LARGE');
            if (!isSse) {
                try { return checkResponse(JSON.parse(text)); } catch (e) {
                    if (e && e.message && e.message.indexOf('MCP_') === 0) throw e;
                    throw error('MCP_INVALID_RESPONSE');
                }
            }
            var messages = [];
            text.split(/\r?\n\r?\n/).forEach(function (eventText) {
                var message = parseSseEvent(eventText);
                if (message) messages.push(message);
            });
            if (!messages.length) throw error('MCP_INVALID_RESPONSE');
            return checkResponse(messages[0]);
        });
    }
    function validatePeripheral(peripheral) {
        if (!isObject(peripheral) || utf8Size(JSON.stringify(peripheral)) > 2048) throw error('AI_INVALID_TOOL_ARGUMENTS');
        var allowed = ['id', 'name', 'type', 'enabled', 'pinCount', 'pins', 'activeHigh', 'sampleInterval', 'params'];
        Object.keys(peripheral).forEach(function (key) {
            if (allowed.indexOf(key) < 0) throw error('AI_INVALID_TOOL_ARGUMENTS');
        });
        if (typeof peripheral.name !== 'string' || !peripheral.name || peripheral.name.length > 64 ||
            !Number.isInteger(peripheral.type) || peripheral.type < 1 || peripheral.type > 60 ||
            !Array.isArray(peripheral.pins) || peripheral.pins.length < 1 || peripheral.pins.length > 8) {
            throw error('AI_INVALID_TOOL_ARGUMENTS');
        }
        peripheral.pins.forEach(function (pin) {
            if (!Number.isInteger(pin) || pin < 0 || pin > 48) throw error('AI_INVALID_TOOL_ARGUMENTS');
        });
        if (peripheral.id !== undefined && (typeof peripheral.id !== 'string' ||
            !/^[A-Za-z0-9_-]{1,40}$/.test(peripheral.id))) throw error('AI_INVALID_TOOL_ARGUMENTS');
        if (peripheral.enabled !== undefined && typeof peripheral.enabled !== 'boolean') throw error('AI_INVALID_TOOL_ARGUMENTS');
        if (peripheral.params !== undefined && !isObject(peripheral.params)) throw error('AI_INVALID_TOOL_ARGUMENTS');
        if (peripheral.activeHigh !== undefined && typeof peripheral.activeHigh !== 'boolean') throw error('AI_INVALID_TOOL_ARGUMENTS');
        if (peripheral.pinCount !== undefined && peripheral.pinCount !== peripheral.pins.length) throw error('AI_INVALID_TOOL_ARGUMENTS');
        if (peripheral.sampleInterval !== undefined && (!Number.isInteger(peripheral.sampleInterval) ||
            peripheral.sampleInterval < 20 || peripheral.sampleInterval > 3600000)) throw error('AI_INVALID_TOOL_ARGUMENTS');
    }
    function containsForbiddenSecret(value) {
        if (!value || typeof value !== 'object') return false;
        return Object.keys(value).some(function (key) {
            if (['password', 'mqttSecret', 'authCode', 'apiKey', 'authorization', 'authHeader'].indexOf(key) >= 0) return true;
            return containsForbiddenSecret(value[key]);
        });
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
        if (containsForbiddenSecret(call.arguments)) throw error('AI_INVALID_TOOL_ARGUMENTS');
        var required = REQUIRED[call.name] || [];
        for (var j = 0; j < required.length; j++) {
            var value = call.arguments[required[j]];
            if (value === undefined || value === null || value === '') throw error('AI_INVALID_TOOL_ARGUMENTS');
        }
        if (call.name === 'upsert_peripheral') validatePeripheral(call.arguments.peripheral);
        if (call.name === 'request_document_upload') {
            var size = call.arguments.sizeBytes;
            var digest = call.arguments.sha256;
            if (!Number.isInteger(size) || size < 1 || size > 20971520 || !/^[0-9a-f]{64}$/.test(digest)) {
                throw error('AI_INVALID_TOOL_ARGUMENTS');
            }
        }
    }
    function isLoopbackHttpBase(value) {
        var match = /^http:\/\/(127\.0\.0\.1|localhost)(?::(\d+))?(?:\/)?$/.exec(value);
        if (!match) return false;
        return !match[2] || (Number(match[2]) >= 1 && Number(match[2]) <= 65535);
    }
    function configure(options) {
        options = options || {};
        var nextCloudBase = typeof options.cloudBase === 'string' ? options.cloudBase.replace(/\/+$/, '') : '';
        if (!/^https:\/\//.test(nextCloudBase) && !isLoopbackHttpBase(nextCloudBase)) {
            throw error('AI_INVALID_MCP_CONFIG');
        }
        var nextOauthToken = typeof options.oauthToken === 'string' ? options.oauthToken : '';
        if (!nextOauthToken || nextOauthToken.length > 2048 || /[\r\n]/.test(nextOauthToken)) {
            throw error('MCP_UNAUTHENTICATED');
        }
        closePendingSseStreams();
        cloudBase = nextCloudBase;
        oauthToken = nextOauthToken;
        pairedDeviceId = typeof options.deviceId === 'string' ? options.deviceId : '';
        baseRevisionProvider = typeof options.getBaseRevision === 'function' ? options.getBaseRevision : null;
        confirmHandler = typeof options.confirm === 'function' ? options.confirm : null;
        defaultTimeoutMs = Number.isInteger(options.timeoutMs) ? Math.min(130000, Math.max(1, options.timeoutMs)) : 0;
        sessionId = '';
        initializing = null;
        try { sessionStorage.setItem('fastbee.mcp.oauth.session', oauthToken); } catch (_) {}
        if (!listenersBound) {
            listenersBound = true;
            window.addEventListener('pagehide', cancelAll);
        }
    }
    function postWithResponse(payload, signal, allowEmpty) {
        throwIfAborted(signal);
        var controller = new AbortController();
        var abort = function () { controller.abort(); };
        if (signal) signal.addEventListener('abort', abort, { once: true });
        var headers = { 'Content-Type': 'application/json', 'Accept': 'application/json, text/event-stream',
            'Authorization': 'Bearer ' + oauthToken };
        if (sessionId) {
            headers['Mcp-Session-Id'] = sessionId;
            headers['MCP-Protocol-Version'] = MCP_PROTOCOL_VERSION;
        }
        return window.fetch(cloudBase + '/mcp', {
            method: 'POST', credentials: 'omit', signal: controller.signal,
            headers: headers,
            body: JSON.stringify(payload)
        }).then(function (response) {
            if (response.status === 202) {
                if (allowEmpty) return { body: null, response: response };
                return readPendingSseMessage(signal).then(function (body) {
                    return { body: body, response: response };
                });
            }
            return readJsonResponse(response, 65536, signal, true).then(function (body) {
                return { body: body, response: response };
            });
        }).finally(function () {
            if (signal) signal.removeEventListener('abort', abort);
        });
    }
    function ensureSession(signal) {
        throwIfAborted(signal);
        if (sessionId) return Promise.resolve(sessionId);
        if (initializing) return initializing;
        var id = nextId++;
        initializing = postWithResponse({ jsonrpc: '2.0', id: id, method: 'initialize', params: {
            protocolVersion: MCP_PROTOCOL_VERSION, capabilities: { elicitation: {} },
            clientInfo: { name: 'fastbee-device-ai', version: '1.0' } } }, signal, false).then(function (exchange) {
            var body = exchange.body;
            var newSessionId = exchange.response.headers && exchange.response.headers.get('Mcp-Session-Id');
            if (!newSessionId || !body || body.id !== id || body.error || !isObject(body.result)) {
                throw error('MCP_INVALID_RESPONSE');
            }
            sessionId = newSessionId;
            return postWithResponse({ jsonrpc: '2.0', method: 'notifications/initialized' }, signal, true);
        }).then(function () {
            return sessionId;
        }).catch(function (e) {
            sessionId = '';
            throw e;
        }).finally(function () {
            initializing = null;
        });
        return initializing;
    }
    function post(payload, signal) {
        return ensureSession(signal).then(function () {
            return postWithResponse(payload, signal, false);
        }).then(function (exchange) {
            return exchange.body;
        });
    }
    function decodeCallToolResult(result) {
        if (!isObject(result)) throw error('MCP_INVALID_RESPONSE');
        var standard = Object.prototype.hasOwnProperty.call(result, 'content') ||
            Object.prototype.hasOwnProperty.call(result, 'structuredContent') ||
            Object.prototype.hasOwnProperty.call(result, 'isError');
        if (!standard) return result;
        var payload;
        if (isObject(result.structuredContent)) {
            payload = result.structuredContent;
        } else {
            if (!Array.isArray(result.content) || result.content.length !== 1 ||
                !isObject(result.content[0]) || result.content[0].type !== 'text' ||
                typeof result.content[0].text !== 'string') throw error('MCP_INVALID_RESPONSE');
            try { payload = JSON.parse(result.content[0].text); } catch (_) { throw error('MCP_INVALID_RESPONSE'); }
            if (!isObject(payload)) throw error('MCP_INVALID_RESPONSE');
        }
        if (result.isError === true) {
            throw error(typeof payload.code === 'string' ? payload.code : 'MCP_TOOL_FAILED');
        }
        return payload;
    }
    function prepareArguments(name, args) {
        var prepared = Object.assign({}, args || {});
        if (DEVICE_CONTEXT_TOOLS.indexOf(name) >= 0 || name === 'request_document_upload') {
            if (!pairedDeviceId) return Promise.reject(error('MCP_DEVICE_CONTEXT_REQUIRED'));
            prepared.deviceId = pairedDeviceId;
        }
        if (name === 'create_config_draft') {
            if (!baseRevisionProvider) return Promise.reject(error('MCP_DEVICE_CONTEXT_REQUIRED'));
            return Promise.resolve(baseRevisionProvider()).then(function (revision) {
                if (typeof revision !== 'string' || !revision || revision.length > 128) {
                    throw error('MCP_INVALID_RESPONSE');
                }
                prepared.baseRevision = revision;
                return prepared;
            });
        }
        if (name === 'start_configuration_workflow') {
            if (!pairedDeviceId) return Promise.reject(error('MCP_DEVICE_CONTEXT_REQUIRED'));
            prepared.input = Object.assign({}, prepared.input || {}, { deviceId: pairedDeviceId });
        }
        return Promise.resolve(prepared);
    }
    function confirmationContent(body) {
        var schema = body && body.params && body.params.requestedSchema;
        var property = schema && schema.properties && schema.properties.confirmationText;
        return { confirmationText: property && typeof property.const === 'string' && property.const ? property.const : '确认执行' };
    }
    function rpc(name, args, trusted, signal) {
        var id = nextId++;
        return prepareArguments(name, args).then(function (prepared) {
            return post({ jsonrpc: '2.0', id: id, method: 'tools/call', params: { name: name, arguments: prepared } }, signal);
        }).then(function message(body) {
            if (body && body.method === 'elicitation/create') {
                if (!trusted || !confirmHandler) return Promise.reject(error('AI_CONFIRMATION_REQUIRED'));
                return Promise.resolve(confirmHandler({ message: body.params && body.params.message,
                    risk: body.params && body.params.risk })).then(function (accepted) {
                    if (!accepted) throw error('CONFIRMATION_REJECTED');
                    return post({ jsonrpc: '2.0', id: body.id, result: {
                        action: 'accept', content: confirmationContent(body) } }, signal).then(message);
                });
            }
            if (!body || body.id !== id) throw error('MCP_INVALID_RESPONSE');
            if (body.error) throw error((typeof body.error.message === 'string' && body.error.message) || body.error.code || 'MCP_TOOL_FAILED');
            closePendingSseStreams();
            return decodeCallToolResult(body.result);
        }).catch(function (e) {
            closePendingSseStreams();
            throw e;
        });
    }
    function rpcTrustedWrite(name, args, signal) {
        var id = nextId++;
        var elicitationSeen = false;
        var accepted = false;
        throwIfAborted(signal);
        return post({ jsonrpc: '2.0', id: id, method: 'tools/call', params: { name: name, arguments: args } }, signal)
            .then(function message(body) {
                throwIfAborted(signal);
                if (body && body.method === 'elicitation/create') {
                    if (elicitationSeen || accepted) throw error('AI_UNEXPECTED_CONFIRMATION');
                    if (!confirmHandler) throw error('AI_CONFIRMATION_REQUIRED');
                    elicitationSeen = true;
                    return Promise.resolve(confirmHandler({ message: body.params && body.params.message,
                        risk: body.params && body.params.risk })).then(function (humanAccepted) {
                        throwIfAborted(signal);
                        if (!humanAccepted) throw error('CONFIRMATION_REJECTED');
                        accepted = true;
                        return post({ jsonrpc: '2.0', id: body.id, result: {
                            action: 'accept', content: confirmationContent(body) } }, signal).then(message);
                    });
                }
                if (!body || body.id !== id) throw error('MCP_INVALID_RESPONSE');
                if (body.error) throw error((typeof body.error.message === 'string' && body.error.message) || body.error.code || 'MCP_TOOL_FAILED');
                if (!elicitationSeen || !accepted) throw error('AI_CONFIRMATION_REQUIRED');
                closePendingSseStreams();
                return decodeCallToolResult(body.result);
            }).catch(function (e) {
                closePendingSseStreams();
                throw e;
            });
    }
    function executeJob(job) {
        if (!job || typeof job.jobId !== 'string' || job.state !== 'awaiting_tool' || active[job.jobId]) {
            return Promise.reject(error('AI_INVALID_JOB'));
        }
        var call = job.toolCall;
        try { validateModelCall(call); } catch (e) { return Promise.reject(e); }
        var controller = new AbortController();
        var timeout = setTimeout(function () { controller.abort('timeout'); }, timeoutForTool(call.name));
        active[job.jobId] = controller;
        var resultText = '';
        var resultError = '';
        return rpc(call.name, call.arguments, false, controller.signal).then(function (result) {
            resultText = JSON.stringify(result);
            if (utf8Size(resultText) > 16384) throw error('MCP_RESULT_TOO_LARGE');
        }).catch(function (e) {
            resultError = normalizeError(e, controller.signal).slice(0, 128);
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
        var timeout = setTimeout(function () { controller.abort(); }, timeoutForTool('apply_configuration_draft'));
        return rpcTrustedWrite('apply_configuration_draft', { draftId: draftId }, controller.signal).then(function (result) {
            window.dispatchEvent(new CustomEvent('fastbee:ai-tool-result', { detail: {
                draftId: draftId, name: 'apply_configuration_draft', result: result, humanConfirmed: true } }));
            return result;
        }).catch(function (e) {
            throw error(normalizeError(e, controller.signal));
        }).finally(function () {
            clearTimeout(timeout);
            delete active[key];
        });
    }
    function testMqttConfiguration(draftId, options) {
        if (!options || options.humanConfirmed !== true || typeof draftId !== 'string' || !draftId) {
            return Promise.reject(error('AI_CONFIRMATION_REQUIRED'));
        }
        var key = 'mqtt-test:' + draftId;
        if (active[key]) return Promise.reject(error('AI_JOB_ACTIVE'));
        var controller = new AbortController();
        active[key] = controller;
        var timeout = setTimeout(function () { controller.abort(); }, 130000);
        return rpcTrustedWrite('test_mqtt_configuration', { draftId: draftId }, controller.signal)
            .then(function (result) {
                window.dispatchEvent(new CustomEvent('fastbee:ai-tool-result', { detail: {
                    draftId: draftId, name: 'test_mqtt_configuration', result: result, humanConfirmed: true } }));
                return result;
            }).catch(function (e) { throw error(normalizeError(e, controller.signal)); })
            .finally(function () { clearTimeout(timeout); delete active[key]; });
    }
    function callTrustedTool(name, args, signal) {
        if (['apply_configuration_draft', 'rollback_configuration_operation', 'test_mqtt_configuration',
            'set_execution_rule_enabled', 'set_rule_script_enabled', 'delete_document'].indexOf(name) >= 0) {
            return Promise.reject(error('AI_CONFIRMATION_REQUIRED'));
        }
        if (signal) return rpc(name, args, false, signal);
        var controller = new AbortController();
        var timeout = setTimeout(function () { controller.abort(); }, timeoutForTool(name));
        return rpc(name, args, false, controller.signal).finally(function () { clearTimeout(timeout); });
    }
    function oauthBytes(count) {
        if (!window.crypto || !window.crypto.getRandomValues) throw error('MCP_PKCE_UNAVAILABLE');
        var bytes = new Uint8Array(count);
        window.crypto.getRandomValues(bytes);
        return bytes;
    }
    function oauthBase64Url(bytes) {
        var binary = '';
        bytes.forEach(function (byte) { binary += String.fromCharCode(byte); });
        return btoa(binary).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/g, '');
    }
    function oauthSha256Fallback(bytes) {
        var K = [0x428a2f98,0x71374491,0xb5c0fbcf,0x59f6f582,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3];
        var H = [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];
        var total = ((bytes.length + 72) >> 6) << 6;
        var data = new Uint8Array(total);
        data.set(bytes);
        data[bytes.length] = 0x80;
        var bits = bytes.length * 8;
        for (var n = 0; n < 8; n++) data[total - 1 - n] = n < 4 ? (bits >>> (n * 8)) & 255 : 0;
        var w = new Uint32Array(64);
        var rotr = function (x, n) { return (x >>> n) | (x << (32 - n)); };
        for (var offset = 0; offset < total; offset += 64) {
            for (var t = 0; t < 64; t++) {
                if (t < 16) {
                    var p = offset + t * 4;
                    w[t] = ((data[p] << 24) | (data[p + 1] << 16) | (data[p + 2] << 8) | data[p + 3]) >>> 0;
                } else {
                    var s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >>> 3);
                    var s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >>> 10);
                    w[t] = (w[t - 16] + s0 + s1 + w[t - 7] + K[t]) >>> 0;
                }
            }
            var a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
            for (var i = 0; i < 64; i++) {
                var S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                var temp1 = (h + S1 + ((e & f) ^ (~e)) + K[i] + w[i]) >>> 0;
                var S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                var temp2 = (S0 + ((a & b) ^ (a & c) ^ (b & c))) >>> 0;
                h=g;g=f;f=e;e=(d + temp1) >>> 0;d=c;c=b;b=a;a=(temp1 + temp2) >>> 0;
            }
            H[0]=(H[0]+a)>>>0;H[1]=(H[1]+b)>>>0;H[2]=(H[2]+c)>>>0;H[3]=(H[3]+d)>>>0;
            H[4]=(H[4]+e)>>>0;H[5]=(H[5]+f)>>>0;H[6]=(H[6]+g)>>>0;H[7]=(H[7]+h)>>>0;
        }
        var output = new Uint8Array(32);
        H.forEach(function (value, index) {
            output[index * 4] = value >>> 24;
            output[index * 4 + 1] = (value >>> 16) & 255;
            output[index * 4 + 2] = (value >>> 8) & 255;
            output[index * 4 + 3] = value & 255;
        });
        return output;
    }
    function oauthSha256(text) {
        var bytes = new TextEncoder().encode(text);
        if (window.crypto && window.crypto.subtle) {
            return window.crypto.subtle.digest('SHA-256', bytes).then(function (digest) {
                return oauthBase64Url(new Uint8Array(digest));
            });
        }
        return Promise.resolve(oauthBase64Url(oauthSha256Fallback(bytes)));
    }
    function beginOAuth(options) {
        options = options || {};
        if (!/^https:\/\//.test(options.authorizationEndpoint || '') || !/^https:\/\//.test(options.tokenEndpoint || '') ||
            typeof options.clientId !== 'string' || !options.clientId || typeof options.redirectUri !== 'string' ||
            !options.redirectUri) return Promise.reject(error('AI_INVALID_MCP_CONFIG'));
        var allowedRedirects = Array.isArray(options.allowedRedirects) ? options.allowedRedirects : [options.redirectUri];
        if (allowedRedirects.indexOf(options.redirectUri) < 0) return Promise.reject(error('MCP_OAUTH_REDIRECT_INVALID'));
        var state = oauthBase64Url(oauthBytes(24));
        var nonce = oauthBase64Url(oauthBytes(24));
        var verifier = oauthBase64Url(oauthBytes(48));
        return oauthSha256(verifier).then(function (challenge) {
            var record = { state: state, nonce: nonce, verifier: verifier, createdAt: Date.now(),
                tokenEndpoint: options.tokenEndpoint, clientId: options.clientId, redirectUri: options.redirectUri };
            try { sessionStorage.setItem('fastbee.mcp.oauth.pkce', JSON.stringify(record)); } catch (_) {}
            var url = new URL(options.authorizationEndpoint);
            url.searchParams.set('response_type', 'code');
            url.searchParams.set('client_id', options.clientId);
            url.searchParams.set('redirect_uri', options.redirectUri);
            url.searchParams.set('scope', (options.scopes || []).join(' '));
            url.searchParams.set('state', state);
            url.searchParams.set('nonce', nonce);
            url.searchParams.set('code_challenge', challenge);
            url.searchParams.set('code_challenge_method', 'S256');
            return { url: url.toString(), state: state };
        });
    }
    function completeOAuth(input) {
        input = input || {};
        var record = null;
        try { record = JSON.parse(sessionStorage.getItem('fastbee.mcp.oauth.pkce') || 'null'); } catch (_) {}
        try { sessionStorage.removeItem('fastbee.mcp.oauth.pkce'); } catch (_) {}
        if (!record || !input.code || !input.state || record.state !== input.state ||
            Date.now() - record.createdAt > 600000) return Promise.reject(error('MCP_OAUTH_STATE_INVALID'));
        var body = new URLSearchParams();
        body.set('grant_type', 'authorization_code');
        body.set('code', input.code);
        body.set('redirect_uri', record.redirectUri);
        body.set('client_id', record.clientId);
        body.set('code_verifier', record.verifier);
        return window.fetch(record.tokenEndpoint, { method: 'POST', credentials: 'omit', redirect: 'error',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: body.toString() })
            .then(function (response) {
                return response.json().then(function (data) {
                    if (!response.ok || !data || typeof data.access_token !== 'string' || !data.access_token ||
                        typeof data.token_type !== 'string' || data.token_type.toLowerCase() !== 'bearer') {
                        throw error('MCP_UNAUTHENTICATED');
                    }
                    try { sessionStorage.setItem('fastbee.mcp.oauth.session', data.access_token); } catch (_) {}
                    return { tokenType: 'Bearer', expiresIn: data.expires_in || null, scope: data.scope || '' };
                }, function () { throw error('MCP_INVALID_RESPONSE'); });
            });
    }
    function resume(options) {
        options = options || {};
        var token = '';
        try { token = sessionStorage.getItem('fastbee.mcp.oauth.session') || ''; } catch (_) {}
        if (!token) return Promise.reject(error('MCP_UNAUTHENTICATED'));
        if (!options.cloudBase || !options.deviceId) return Promise.reject(error('MCP_DEVICE_CONTEXT_REQUIRED'));
        configure({ cloudBase: options.cloudBase, oauthToken: token, deviceId: options.deviceId,
            getBaseRevision: options.getBaseRevision, confirm: options.confirm, timeoutMs: options.timeoutMs });
        var controller = new AbortController();
        var timeout = setTimeout(function () { controller.abort(); }, 25000);
        var requestId = nextId++;
        return ensureSession(controller.signal).then(function () {
            return post({ jsonrpc: '2.0', id: requestId, method: 'tools/list', params: {} }, controller.signal);
        }).then(function (body) {
            if (!body || body.id !== requestId || !isObject(body.result) || !Array.isArray(body.result.tools)) {
                throw error('MCP_INVALID_RESPONSE');
            }
            return callTrustedTool('get_device_capabilities', {}, controller.signal);
        }).then(function (capabilities) {
            return { ready: true, capabilities: capabilities };
        }).catch(function (e) {
            var code = normalizeError(e, controller.signal);
            if (['MCP_UNAUTHENTICATED', 'SESSION_EXPIRED', 'SESSION_REVOKED'].indexOf(code) >= 0) {
                try { sessionStorage.removeItem('fastbee.mcp.oauth.session'); } catch (_) {}
            }
            throw error(code);
        }).finally(function () { clearTimeout(timeout); });
    }
    function disconnect() {
        cancelAll();
        oauthToken = '';
        sessionId = '';
        initializing = null;
        try { sessionStorage.removeItem('fastbee.mcp.oauth.session'); } catch (_) {}
    }
    function cancel(jobId) {
        if (active[jobId]) {
            active[jobId].abort();
            delete active[jobId];
        }
    }
    function cancelAll() {
        closePendingSseStreams();
        Object.keys(active).forEach(function (jobId) {
            active[jobId].abort();
            delete active[jobId];
        });
    }

    window.FastBeeAiMcpTools = {
        configure: configure,
        beginOAuth: beginOAuth,
        completeOAuth: completeOAuth,
        resume: resume,
        disconnect: disconnect,
        decodeCallToolResult: decodeCallToolResult,
        executeJob: executeJob,
        applyDraft: applyDraft,
        testMqttConfiguration: testMqttConfiguration,
        callTrustedTool: callTrustedTool,
        cancel: cancel,
        cancelAll: cancelAll,
        activeCount: function () { return Object.keys(active).length; },
        baseUrl: function () { return cloudBase; },
        readJson: readJsonResponse,
        timeoutForTool: timeoutForTool,
        checkSignal: throwIfAborted,
        normalizeError: normalizeError
    };
})();
