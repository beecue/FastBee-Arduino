/**
 * FastBee AI MCP 浏览器连接器（Task 3）
 *
 * 在已登录的设备 Web 页面内运行，作为云端 MCP 服务与设备同源 API 之间的有界转发通道：
 * - 配对码/连接器 token 只存 sessionStorage，不写设备 Flash、URL、日志或模型上下文；
 * - 只执行固定类型白名单任务，拒绝任何携带 URL/方法的任务；
 * - 设备调用使用页面现有同源认证；写操作仅限 Task 4 固定配置预检/应用/回读/回滚事务；
 * - 长轮询等待上限 20 秒；页面离线/隐藏/离页时停止轮询并上报最后状态，bfcache 恢复（pageshow persisted）自动继续；
 * - 连续失败达到上限后停机并上报最后状态，失效会话不会无限轮询/心跳；
 * - 令牌 15 分钟到期前自动续期，到达云端 8 小时总会话上限后停止并清除会话。
 */
(function () {
    'use strict';

    var STORAGE_KEY = 'fastbee.mcp.connector.session';
    var SCHEMA_VERSION = '1';
    var MAX_POLL_WAIT_MS = 20000;
    var MAX_RESULT_BYTES = 16384;
    // 配对能力载荷上限（云端注册存储上限 16 KiB，留 1 KiB 余量）；
    // 超限时放弃外设目录而不是截断伪造，云端外设工具保持失败关闭
    var MAX_CAPABILITIES_BYTES = 15360;
    var TOKEN_REFRESH_MARGIN_MS = 60000;
    var HEARTBEAT_INTERVAL_MS = 60000;
    var POLL_RETRY_BASE_MS = 2000;
    var POLL_RETRY_MAX_MS = 30000;
    // 连续失败上限：达到后停机并上报最后状态，失效会话不得无限轮询/心跳
    var MAX_CONSECUTIVE_FAILURES = 10;

    // 固定任务类型白名单；云端任务不允许携带 URL/任意方法，只允许下列类型与字段
    var SECTION_PATHS = {
        'device': '/api/device/config',
        'features': '/api/features/config',
        'peripherals': '/api/peripherals',
        'periphExec': '/api/periph-exec',
        'ruleScripts': '/api/rule-script',
        'protocol': '/api/protocol/config',
        'network': '/api/network/config',
        'screenPanel': '/api/screen-panel/config'
    };
    var ALLOWED_JOB_KEYS = ['jobId', 'type', 'section', 'idempotencyKey', 'deadlineAt', 'leaseExpiresAt',
        'draftId', 'baseRevision', 'currentRevision', 'approvalHash', 'deviceIdempotencyKey', 'operations', 'operationId'];
    var FORBIDDEN_JOB_KEYS = ['url', 'method', 'headers', 'body', 'path'];
    var UNCERTAIN_RESULT_JOB_TYPES = ['device.config.apply', 'device.config.rollback', 'device.config.readback'];

    var session = null; // {cloudBase, token, tokenExpiresAt, sessionExpiresAt, deviceId}
    var running = false;
    var wantResume = false;
    var listenersBound = false;
    var pollController = null;
    var heartbeatTimer = null;
    var refreshTimer = null;
    var capTimer = null;
    var retryTimer = null;
    var consecutiveErrors = 0;
    var lastState = 'online';

    // ---------- 设备同源只读调用（页面现有认证，不引入新凭据） ----------

    function _deviceGet(path) {
        if (typeof window.apiGet === 'function') {
            return Promise.resolve(window.apiGet(path)).then(function (resp) {
                if (!resp || resp.success !== true) {
                    throw new Error('DEVICE_API_FAILED');
                }
                return resp.data;
            });
        }
        return window.fetch(path, { credentials: 'same-origin' }).then(function (resp) {
            return resp.json().then(function (body) {
                if (!resp.ok || !body || body.success !== true) {
                    throw new Error('DEVICE_API_FAILED');
                }
                return body.data;
            });
        });
    }

    /** 原始 body 读取（不解包 data）：/api/peripherals/types 的 profile/hardware 在 data 之外。 */
    function _deviceGetRaw(path) {
        if (typeof window.apiGet === 'function') {
            return Promise.resolve(window.apiGet(path)).then(function (resp) {
                if (!resp || resp.success !== true) {
                    throw new Error('DEVICE_API_FAILED');
                }
                return resp;
            });
        }
        return window.fetch(path, { credentials: 'same-origin' }).then(function (resp) {
            return resp.json().then(function (body) {
                if (!resp.ok || !body || body.success !== true) {
                    throw new Error('DEVICE_API_FAILED');
                }
                return body;
            });
        });
    }

    function _isObject(value) {
        return !!value && typeof value === 'object' && !Array.isArray(value);
    }

    function _validOperationState(state) {
        return ['SUCCEEDED', 'FAILED', 'UNKNOWN', 'ROLLED_BACK'].indexOf(state) >= 0;
    }

    function _validManagedResult(path, data) {
        if (!_isObject(data)) return false;
        if (path === '/api/mcp/config/preflight') return typeof data.allowed === 'boolean';
        if (typeof data.operationId !== 'string' || !/^[A-Za-z0-9-]{1,128}$/.test(data.operationId)) return false;
        if (!_validOperationState(data.state)) return false;
        if (data.state === 'FAILED' && !_definitiveDeviceRejection(data.error || data.code)) return false;
        if (data.steps !== undefined) {
            if (!Array.isArray(data.steps)) return false;
            for (var i = 0; i < data.steps.length; i++) {
                var step = data.steps[i];
                if (!_isObject(step) || typeof step.section !== 'string' || !step.section
                        || !_validOperationState(step.state)) return false;
                var flags = ['accepted', 'persisted', 'readback', 'observed', 'physicalConfirmed', 'pendingRestart'];
                for (var j = 0; j < flags.length; j++) {
                    if (step[flags[j]] !== undefined && typeof step[flags[j]] !== 'boolean') return false;
                }
            }
        }
        return true;
    }

    function _definitiveDeviceRejection(code) {
        return ['INVALID_ARGUMENT', 'SCHEMA_VERSION_UNSUPPORTED', 'CAPABILITY_UNSUPPORTED', 'CONFIG_REVISION_CONFLICT',
            'PREFLIGHT_FAILED', 'CONFIRMATION_REQUIRED', 'CONFIRMATION_EXPIRED', 'SECRET_REQUIRED', 'DRAFT_CONFLICT',
            'DRAFT_NOT_FOUND', 'DEVICE_NOT_FOUND', 'DEVICE_IDENTITY_CONFLICT', 'OPERATION_IN_PROGRESS', 'QUOTA_EXCEEDED',
            'MANUAL_RECOVERY_REQUIRED', 'IDEMPOTENCY_CONFLICT', 'IDEMPOTENCY_KEY_CONFLICT', 'AUDIT_UNAVAILABLE',
            'FORBIDDEN', 'UNAUTHENTICATED', 'SAVE_FAILED', 'PERSIST_FAILED'].indexOf(code) >= 0;
    }

    // 仅固定 Task 4 配置路由使用；结构化设备拒绝是已知失败，断连/无效正文是 UNKNOWN。
    function _devicePost(path, payload) {
        function unwrap(resp) {
            if (!_isObject(resp)) throw new Error('OUTCOME_UNKNOWN');
            if (resp.success === true) {
                if (!_validManagedResult(path, resp.data)) throw new Error('OUTCOME_UNKNOWN');
                return resp.data;
            }
            var code = resp.code || resp.error;
            if (resp.success !== false || typeof code !== 'string' || !/^[A-Z][A-Z0-9_]{1,63}$/.test(code)
                    || !_definitiveDeviceRejection(code)) {
                throw new Error('OUTCOME_UNKNOWN');
            }
            var err = new Error(code);
            err.known = true;
            throw err;
        }
        if (typeof window.apiPost === 'function') {
            return Promise.resolve(window.apiPost(path, payload)).then(unwrap);
        }
        return window.fetch(path, {
            method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        }).then(function (resp) {
            return resp.json().then(function (body) {
                if (!resp.ok && (!body || body.success !== false)) throw new Error('OUTCOME_UNKNOWN');
                return unwrap(body);
            }, function () { throw new Error('OUTCOME_UNKNOWN'); });
        });
    }

    // ---------- 云端调用（token 只在 Authorization 头，绝不进 URL） ----------

    function _cloudPost(url, body, token, signal, keepalive) {
        var headers = { 'Content-Type': 'application/json' };
        if (token) {
            headers['Authorization'] = 'Bearer ' + token;
        }
        return window.fetch(url, {
            method: 'POST',
            headers: headers,
            body: JSON.stringify(body || {}),
            credentials: 'omit',
            signal: signal,
            keepalive: !!keepalive
        }).then(function (resp) {
            return resp.json().then(function (data) {
                return { status: resp.status, body: data };
            }, function () {
                return { status: resp.status, body: {} };
            });
        });
    }

    // ---------- 会话存取（仅 sessionStorage） ----------

    function _save() {
        if (!session) return;
        try {
            window.sessionStorage.setItem(STORAGE_KEY, JSON.stringify(session));
        } catch (e) { /* 存储不可用时仅内存保留 */ }
    }

    function _load() {
        if (session) return;
        try {
            var raw = window.sessionStorage.getItem(STORAGE_KEY);
            if (raw) session = JSON.parse(raw);
        } catch (e) {
            session = null;
        }
    }

    function _clearSession() {
        session = null;
        try {
            window.sessionStorage.removeItem(STORAGE_KEY);
        } catch (e) { /* 忽略 */ }
    }

    // ---------- 配对 ----------

    // 执行目录端点（固件生产只读路由；云端外设执行规则工具的 trigger/event/action schema 来源）
    var EXEC_TRIGGER_TYPES_PATH = '/api/periph-exec/trigger-types';
    var EXEC_EVENTS_PATH = '/api/periph-exec/events/static';
    var EXEC_ACTION_TYPES_PATH = '/api/periph-exec/action-types';
    var EXEC_CATALOG_SCHEMA_VERSION = 1;

    function _isNonBlankString(value, maxLen) {
        return typeof value === 'string' && value.length > 0 && value.length <= maxLen;
    }

    function _isUint8(value) {
        return typeof value === 'number' && Number.isInteger(value) && value >= 0 && value <= 255;
    }

    // 严格目录行校验：必填字段类型化且非空、ID/类型值唯一；任何畸形/重复行使整个目录不可用，
    // 不挑选部分行伪造可用目录（云端按缺失失败关闭）
    function _validTriggerTypes(list) {
        if (!Array.isArray(list) || !list.length || list.length > 16) return false;
        var seen = {};
        for (var i = 0; i < list.length; i++) {
            var row = list[i];
            if (!row || !_isUint8(row.type) || !_isNonBlankString(row.name, 64)) return false;
            if (seen[row.type]) return false;
            seen[row.type] = true;
        }
        return true;
    }

    function _validEvents(list) {
        if (!Array.isArray(list) || !list.length || list.length > 128) return false;
        var seen = {};
        for (var i = 0; i < list.length; i++) {
            var row = list[i];
            if (!row || !_isNonBlankString(row.id, 64) || !_isNonBlankString(row.name, 128)
                    || !_isUint8(row.type)) return false;
            if (seen[row.id]) return false;
            seen[row.id] = true;
        }
        return true;
    }

    function _validActionTypes(payload) {
        if (!payload || payload.schemaVersion !== EXEC_CATALOG_SCHEMA_VERSION
                || !_isNonBlankString(payload.firmwareVersion, 64)) return false;
        var list = payload.actions;
        if (!Array.isArray(list) || !list.length || list.length > 64) return false;
        var seen = {};
        for (var i = 0; i < list.length; i++) {
            var row = list[i];
            if (!row || !_isUint8(row.value) || !_isNonBlankString(row.name, 64)
                    || typeof row.requiresPeripheralTarget !== 'boolean') return false;
            if (row.targetTypes !== undefined && row.targetTypes !== null) {
                if (!Array.isArray(row.targetTypes)) return false;
                for (var j = 0; j < row.targetTypes.length; j++) {
                    if (!_isUint8(row.targetTypes[j])) return false;
                }
            }
            if (seen[row.value]) return false;
            seen[row.value] = true;
        }
        return true;
    }

    /**
     * 读取设备执行目录（触发类型 + 静态事件 + 生产动作类型目录）。
     * 三个子目录都必须通过严格形状校验（类型化必填字段、唯一 ID）；任一端点失败、
     * 畸形或超界即视为目录整体不可用（返回 null，云端规则工具失败关闭），
     * 绝不截断或编造部分目录。
     */
    function _fetchExecutionCatalog() {
        return Promise.all([
            _deviceGet(EXEC_TRIGGER_TYPES_PATH).catch(function () { return null; }),
            _deviceGet(EXEC_EVENTS_PATH).catch(function () { return null; }),
            _deviceGet(EXEC_ACTION_TYPES_PATH).catch(function () { return null; })
        ]).then(function (results) {
            if (!_validTriggerTypes(results[0]) || !_validEvents(results[1])
                    || !_validActionTypes(results[2])) {
                return null;
            }
            return {
                schemaVersion: EXEC_CATALOG_SCHEMA_VERSION,
                firmwareVersion: results[2].firmwareVersion,
                triggerTypes: results[0],
                events: results[1],
                actions: results[2].actions
            };
        });
    }

    /**
     * 组合系统编译能力与外设类型目录（/api/peripherals/types，云端外设配置工具的 schema 来源）。
     * 目录读取失败时返回不含目录的能力并显式标记；组合 JSON 按 UTF-8 字节数（与云端存储口径一致，
     * 不用 UTF-16 长度）超过有界上限时放弃目录而不是截断伪造，云端外设工具在目录缺失时保持失败关闭。
     * 执行目录（trigger/event，云端规则工具的 schema 来源）同样纳入组合；超限时先放弃执行目录、
     * 再放弃外设目录，两级都显式标记。
     */
    function _utf8Bytes(json) {
        return (typeof TextEncoder === 'function')
            ? new TextEncoder().encode(json).length
            : unescape(encodeURIComponent(json)).length;
    }

    function _buildCapabilities(sysCaps, catalog, execution) {
        if (!sysCaps && !catalog && !execution) return null;
        var combined = {};
        if (sysCaps && typeof sysCaps === 'object') {
            for (var key in sysCaps) {
                if (Object.prototype.hasOwnProperty.call(sysCaps, key)) combined[key] = sysCaps[key];
            }
        }
        if (catalog && typeof catalog === 'object') {
            combined.peripheralCatalog = catalog;
        } else {
            combined.peripheralCatalogAvailable = false;
        }
        if (execution && typeof execution === 'object') {
            combined.executionCatalog = execution;
        } else {
            combined.executionCatalogAvailable = false;
        }
        try {
            if (_utf8Bytes(JSON.stringify(combined)) > MAX_CAPABILITIES_BYTES && combined.executionCatalog) {
                delete combined.executionCatalog;
                combined.executionCatalogAvailable = false;
                combined.executionCatalogOversize = true;
            }
            if (_utf8Bytes(JSON.stringify(combined)) > MAX_CAPABILITIES_BYTES) {
                delete combined.peripheralCatalog;
                combined.peripheralCatalogAvailable = false;
                combined.peripheralCatalogOversize = true;
            }
        } catch (e) {
            delete combined.peripheralCatalog;
            delete combined.executionCatalog;
            combined.peripheralCatalogAvailable = false;
            combined.executionCatalogAvailable = false;
        }
        return combined;
    }

    function pair(cloudBase, code) {
        if (!cloudBase || !code || typeof cloudBase !== 'string' || typeof code !== 'string') {
            return Promise.resolve({ ok: false, error: 'INVALID_ARGUMENT' });
        }
        var base = cloudBase.replace(/\/+$/, '');
        return Promise.all([
            _deviceGet('/api/device/info'),
            _deviceGet('/api/system/capabilities'),
            _deviceGet('/api/system/info?brief=1').catch(function () { return null; }),
            // 外设类型目录（/api/peripherals/types，原始 body 含 profile/hardware）：
            // 云端外设配置工具的 schema 来源；读取失败时继续配对但不携带目录，云端失败关闭
            _deviceGetRaw('/api/peripherals/types').catch(function () { return null; }),
            // 执行目录（trigger-types + events/static）：云端外设执行规则工具的 schema 来源；
            // 读取失败时继续配对但不携带目录，云端规则工具失败关闭
            _fetchExecutionCatalog()
        ]).then(function (results) {
            var info = results[0] || {};
            var sys = results[2] || {};
            var chip = info.chip || {};
            var device = {
                deviceId: info.deviceId || '',
                deviceName: info.deviceName || '',
                macAddress: chip.macAddress || '',
                chipModel: chip.model || '',
                firmwareVersion: (sys.device && sys.device.firmwareVersion) || '',
                schemaVersion: SCHEMA_VERSION,
                capabilities: _buildCapabilities(results[1], results[3], results[4])
            };
            if (!device.deviceId || !device.macAddress) {
                return { ok: false, error: 'DEVICE_API_FAILED' };
            }
            return _cloudPost(base + '/connector/v1/pair', {
                code: code,
                origin: window.location.origin,
                device: device
            }, null, null, false).then(function (resp) {
                if (resp.status === 200 && resp.body && resp.body.connectorToken) {
                    session = {
                        cloudBase: base,
                        token: resp.body.connectorToken,
                        tokenExpiresAt: resp.body.tokenExpiresAt,
                        sessionExpiresAt: resp.body.sessionExpiresAt,
                        deviceId: resp.body.deviceId || device.deviceId
                    };
                    _save();
                    return { ok: true, deviceId: session.deviceId };
                }
                return { ok: false, error: (resp.body && resp.body.code) || 'PAIR_FAILED' };
            });
        }).catch(function () {
            return { ok: false, error: 'DEVICE_API_FAILED' };
        });
    }

    // ---------- 任务执行（固定白名单；写类型仅 Task 4 配置事务） ----------

    var JOB_HANDLERS = {
        'device.info.read': function () {
            return _deviceGet('/api/device/info');
        },
        'device.capabilities.read': function () {
            // 能力刷新同样携带外设类型目录（原始 body，有界）与执行目录，
            // 供云端外设配置与外设执行规则校验使用；顶层附带当前固件版本，
            // 云端据此把刷新结果写回注册并做目录新鲜度绑定
            return Promise.all([
                _deviceGet('/api/system/capabilities'),
                _deviceGetRaw('/api/peripherals/types').catch(function () { return null; }),
                _fetchExecutionCatalog(),
                _deviceGet('/api/system/info?brief=1').catch(function () { return null; })
            ]).then(function (results) {
                var combined = _buildCapabilities(results[0], results[1], results[2]);
                if (combined && typeof combined === 'object'
                        && !Object.prototype.hasOwnProperty.call(combined, 'firmwareVersion')) {
                    var fw = results[3] && results[3].device && results[3].device.firmwareVersion;
                    if (typeof fw === 'string' && fw) combined.firmwareVersion = fw;
                }
                return combined;
            });
        },
        'device.config.section.read': function (job) {
            var path = SECTION_PATHS[job.section];
            if (!path) return Promise.reject(new Error('INVALID_ARGUMENT'));
            return _deviceGet(path);
        },
        'device.config.snapshot.read': function () {
            return _deviceGet('/api/mcp/config/snapshot');
        },
        'device.config.preflight': function (job) {
            return _devicePost('/api/mcp/config/preflight', {
                baseRevision: job.baseRevision, operations: job.operations
            });
        },
        'device.config.apply': function (job) {
            return _devicePost('/api/mcp/config/apply', {
                draftId: job.draftId, baseRevision: job.baseRevision, approvalHash: job.approvalHash,
                idempotencyKey: job.deviceIdempotencyKey, operations: job.operations
            });
        },
        'device.config.operation.read': function (job) {
            return _deviceGet('/api/mcp/config/operations/' + encodeURIComponent(job.operationId));
        },
        'device.config.readback': function (job) {
            return _devicePost('/api/mcp/config/readback', { operationId: job.operationId });
        },
        'device.config.rollback': function (job) {
            return _devicePost('/api/mcp/config/rollback', {
                operationId: job.operationId, currentRevision: job.currentRevision,
                approvalHash: job.approvalHash, idempotencyKey: job.deviceIdempotencyKey
            });
        }
    };

    function _validateJob(job) {
        if (!job || typeof job !== 'object') return 'INVALID_ARGUMENT';
        if (!job.jobId || typeof job.jobId !== 'string') return 'INVALID_ARGUMENT';
        if (!job.idempotencyKey || typeof job.idempotencyKey !== 'string') return 'INVALID_ARGUMENT';
        if (typeof JOB_HANDLERS[job.type] !== 'function') return 'INVALID_ARGUMENT';
        for (var i = 0; i < FORBIDDEN_JOB_KEYS.length; i++) {
            if (Object.prototype.hasOwnProperty.call(job, FORBIDDEN_JOB_KEYS[i])) return 'INVALID_ARGUMENT';
        }
        for (var key in job) {
            if (Object.prototype.hasOwnProperty.call(job, key) && ALLOWED_JOB_KEYS.indexOf(key) < 0) {
                return 'INVALID_ARGUMENT';
            }
        }
        if (job.type === 'device.config.section.read' && !SECTION_PATHS[job.section]) return 'INVALID_ARGUMENT';
        if (['device.config.operation.read', 'device.config.readback', 'device.config.rollback'].indexOf(job.type) >= 0
                && (typeof job.operationId !== 'string' || !/^[A-Za-z0-9-]{1,128}$/.test(job.operationId))) {
            return 'INVALID_ARGUMENT';
        }
        if (['device.config.preflight', 'device.config.apply'].indexOf(job.type) >= 0
                && !Array.isArray(job.operations)) return 'INVALID_ARGUMENT';
        return null;
    }

    function _executeJob(job) {
        if (!job || typeof job !== 'object') return Promise.resolve(false);
        var invalid = _validateJob(job);
        if (invalid) {
            return _submitResult(job.jobId, job.idempotencyKey, 'FAILED', null, invalid);
        }
        if (job.deadlineAt && Date.now() > job.deadlineAt) {
            return _submitResult(job.jobId, job.idempotencyKey, 'UNKNOWN', null, 'DEADLINE_EXCEEDED');
        }
        var writeJob = UNCERTAIN_RESULT_JOB_TYPES.indexOf(job.type) >= 0;
        return Promise.resolve()
            .then(function () { return JOB_HANDLERS[job.type](job); })
            .then(function (data) {
                var json;
                try {
                    json = JSON.stringify(data === undefined ? null : data);
                } catch (e) {
                    return _submitResult(job.jobId, job.idempotencyKey, writeJob ? 'UNKNOWN' : 'FAILED', null,
                        writeJob ? 'OUTCOME_UNKNOWN' : 'INVALID_ARGUMENT');
                }
                // 超限结果明确失败，不截断伪造成功；写后无法证明结果时保持 UNKNOWN。
                if (json.length > MAX_RESULT_BYTES) {
                    return _submitResult(job.jobId, job.idempotencyKey, writeJob ? 'UNKNOWN' : 'FAILED', null,
                        writeJob ? 'OUTCOME_UNKNOWN' : 'RESULT_TOO_LARGE');
                }
                return _submitResult(job.jobId, job.idempotencyKey, 'SUCCEEDED', data, null);
            })
            .catch(function (err) {
                var code = (err && err.message === 'INVALID_ARGUMENT') ? 'INVALID_ARGUMENT' :
                    (err && err.message) || 'DEVICE_API_FAILED';
                if (writeJob && !(err && err.known)) {
                    return _submitResult(job.jobId, job.idempotencyKey, 'UNKNOWN', null, 'OUTCOME_UNKNOWN');
                }
                return _submitResult(job.jobId, job.idempotencyKey, 'FAILED', null, code);
            });
    }

    function _submitResult(jobId, idempotencyKey, status, result, error) {
        if (!session || !jobId) return Promise.resolve(false);
        return _cloudPost(session.cloudBase + '/connector/v1/jobs/' + encodeURIComponent(jobId) + '/result', {
            status: status,
            result: result === undefined ? null : result,
            error: error || null,
            idempotencyKey: idempotencyKey
        }, session.token, null, false).then(function (resp) {
            return resp.status === 200;
        }).catch(function () {
            return false;
        });
    }

    // ---------- 长轮询 ----------

    function _pollLoop(pollWaitMs) {
        if (!running || !session) return;
        if (Date.now() >= session.sessionExpiresAt) {
            _expireSession();
            return;
        }
        var waitMs = Math.min(MAX_POLL_WAIT_MS, pollWaitMs || MAX_POLL_WAIT_MS);
        pollController = new AbortController();
        var controller = pollController;
        var tokenAtStart = session.token;
        _cloudPost(session.cloudBase + '/connector/v1/jobs/poll', { waitMs: waitMs }, session.token,
            controller.signal, false)
            .then(function (resp) {
                if (!running || controller !== pollController) return 'stale';
                if (resp.status === 401) {
                    // 轮询期间 token 被续期轮换：直接用新 token 继续；否则会话已失效
                    if (session && session.token !== tokenAtStart) return 'ok';
                    _expireSession();
                    return 'stopped';
                }
                if (!resp || resp.status !== 200) return 'failed';
                consecutiveErrors = 0;
                var jobs = (resp.body && resp.body.jobs) || [];
                return jobs.slice(0, 8).reduce(function (p, job) {
                    return p.then(function () { return _executeJob(job); });
                }, Promise.resolve()).then(function () { return 'ok'; });
            })
            .then(function (outcome) {
                if (!running) return;
                if (outcome === 'failed') {
                    _registerFailure(pollWaitMs);
                    return;
                }
                if (outcome === 'stale' || outcome === 'stopped') return;
                _pollLoop(pollWaitMs);
            })
            .catch(function (err) {
                if (err && err.name === 'AbortError') return;
                if (!running) return;
                _registerFailure(pollWaitMs);
            });
    }

    // 有界失败计数：连续失败达到上限后停机并上报最后状态，不无限轮询
    function _registerFailure(pollWaitMs) {
        consecutiveErrors++;
        if (consecutiveErrors >= MAX_CONSECUTIVE_FAILURES) {
            _haltUnreachable();
            return;
        }
        var delay = Math.min(POLL_RETRY_MAX_MS, POLL_RETRY_BASE_MS * consecutiveErrors);
        retryTimer = setTimeout(function () {
            retryTimer = null;
            if (running) _pollLoop(pollWaitMs);
        }, delay);
    }

    function _haltUnreachable() {
        lastState = 'unreachable';
        running = false;
        wantResume = true;
        _clearAllTimers();
        if (pollController) {
            pollController.abort();
            pollController = null;
        }
        if (session) {
            // 停机前上报最后状态（有界一次）；会话保留，恢复在线/页面回显时可继续
            _cloudPost(session.cloudBase + '/connector/v1/heartbeat', { state: 'unreachable' }, session.token,
                null, false).catch(function () { /* 不可达场景忽略失败 */ });
        }
    }

    // ---------- 心跳与续期 ----------

    function _scheduleHeartbeat() {
        _clearTimer('heartbeat');
        heartbeatTimer = setTimeout(function () {
            heartbeatTimer = null;
            if (!running || !session) return;
            _cloudPost(session.cloudBase + '/connector/v1/heartbeat', { state: lastState }, session.token, null, false)
                .then(function (resp) {
                    if (resp && resp.status === 401) {
                        _expireSession();
                        return;
                    }
                    if (!running) return;
                    if (!resp || resp.status !== 200) {
                        _registerHeartbeatFailure();
                        return;
                    }
                    _scheduleHeartbeat();
                })
                .catch(function () {
                    if (running) _registerHeartbeatFailure();
                });
        }, HEARTBEAT_INTERVAL_MS);
    }

    // 心跳失败计入同一有界上限（只有轮询成功才清零），失效会话不得无限心跳
    function _registerHeartbeatFailure() {
        consecutiveErrors++;
        if (consecutiveErrors >= MAX_CONSECUTIVE_FAILURES) {
            _haltUnreachable();
            return;
        }
        _scheduleHeartbeat();
    }

    function _scheduleRefresh() {
        _clearTimer('refresh');
        if (!session) return;
        var delay = session.tokenExpiresAt - TOKEN_REFRESH_MARGIN_MS - Date.now();
        if (delay < 0) delay = 0;
        refreshTimer = setTimeout(function () {
            refreshTimer = null;
            if (!running || !session) return;
            if (Date.now() >= session.sessionExpiresAt) {
                _expireSession();
                return;
            }
            _cloudPost(session.cloudBase + '/connector/v1/token/refresh', {}, session.token, null, false)
                .then(function (resp) {
                    if (!session) return;
                    if (resp && resp.status === 200 && resp.body && resp.body.connectorToken) {
                        session.token = resp.body.connectorToken;
                        session.tokenExpiresAt = resp.body.tokenExpiresAt;
                        session.sessionExpiresAt = resp.body.sessionExpiresAt;
                        _save();
                    }
                    else if (resp && resp.status === 401) {
                        _expireSession();
                        return;
                    }
                    if (running) _scheduleRefresh();
                })
                .catch(function () {
                    if (running) _scheduleRefresh();
                });
        }, delay);
    }

    function _scheduleSessionCap() {
        _clearTimer('cap');
        if (!session) return;
        var delay = session.sessionExpiresAt - Date.now();
        if (delay < 0) delay = 0;
        capTimer = setTimeout(function () {
            capTimer = null;
            _expireSession();
        }, delay);
    }

    function _clearTimer(which) {
        if (which === 'heartbeat' && heartbeatTimer) {
            clearTimeout(heartbeatTimer);
            heartbeatTimer = null;
        }
        if (which === 'refresh' && refreshTimer) {
            clearTimeout(refreshTimer);
            refreshTimer = null;
        }
        if (which === 'cap' && capTimer) {
            clearTimeout(capTimer);
            capTimer = null;
        }
    }

    function _clearAllTimers() {
        _clearTimer('heartbeat');
        _clearTimer('refresh');
        _clearTimer('cap');
        if (retryTimer) {
            clearTimeout(retryTimer);
            retryTimer = null;
        }
    }

    // ---------- 生命周期 ----------

    function _expireSession() {
        running = false;
        wantResume = false;
        _clearAllTimers();
        if (pollController) {
            pollController.abort();
            pollController = null;
        }
        _clearSession();
    }

    function _pause(state, keepalive) {
        if (!running) return;
        lastState = state;
        running = false;
        wantResume = true;
        _clearAllTimers();
        if (pollController) {
            pollController.abort();
            pollController = null;
        }
        if (session) {
            // 上报最后状态后暂停；keepalive 保证离页场景也能发出
            _cloudPost(session.cloudBase + '/connector/v1/heartbeat', { state: state }, session.token, null, keepalive)
                .catch(function () { /* 离线场景忽略失败 */ });
        }
    }

    function _resume() {
        if (!wantResume || running || !session) return;
        wantResume = false;
        lastState = 'online';
        start();
    }

    function _bindListeners() {
        if (listenersBound) return;
        listenersBound = true;
        window.addEventListener('pagehide', function () { _pause('offline', true); });
        // bfcache 恢复：页面从缓存回显时继续轮询（pagehide 时已记录 wantResume）
        window.addEventListener('pageshow', function (event) {
            if (event && event.persisted) _resume();
        });
        window.addEventListener('offline', function () { _pause('offline', false); });
        window.addEventListener('online', function () { _resume(); });
        document.addEventListener('visibilitychange', function () {
            if (document.hidden) _pause('hidden', false);
            else _resume();
        });
    }

    function start(options) {
        _load();
        if (!session || !session.token || !session.cloudBase) return false;
        if (running) return true;
        running = true;
        wantResume = false;
        consecutiveErrors = 0;
        lastState = 'online';
        _bindListeners();
        _scheduleHeartbeat();
        _scheduleRefresh();
        _scheduleSessionCap();
        var pollWaitMs = options && typeof options.pollWaitMs === 'number' ? options.pollWaitMs : MAX_POLL_WAIT_MS;
        _pollLoop(pollWaitMs);
        return true;
    }

    function stop() {
        running = false;
        wantResume = false;
        _clearAllTimers();
        if (pollController) {
            pollController.abort();
            pollController = null;
        }
    }

    function revoke() {
        _load();
        if (!session) return Promise.resolve(false);
        var base = session.cloudBase;
        var token = session.token;
        stop();
        _clearSession();
        return _cloudPost(base + '/connector/v1/revoke', {}, token, null, false)
            .then(function (resp) { return resp.status === 200; })
            .catch(function () { return false; });
    }

    function status() {
        _load();
        return {
            paired: !!session,
            running: running,
            deviceId: session ? session.deviceId : null,
            tokenExpiresAt: session ? session.tokenExpiresAt : null,
            sessionExpiresAt: session ? session.sessionExpiresAt : null,
            state: lastState,
            consecutiveErrors: consecutiveErrors
        };
    }

    window.FastBeeAiMcpConnector = {
        pair: pair,
        start: start,
        stop: stop,
        revoke: revoke,
        status: status
    };
})();
