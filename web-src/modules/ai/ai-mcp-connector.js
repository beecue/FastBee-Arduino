/**
 * FastBee AI MCP 浏览器连接器（Task 3）
 *
 * 在已登录的设备 Web 页面内运行，作为云端 MCP 服务与设备同源 API 之间的有界转发通道：
 * - 配对码/连接器 token 只存 sessionStorage，不写设备 Flash、URL、日志或模型上下文；
 * - 只执行固定类型白名单任务，拒绝任何携带 URL/方法的任务；
 * - 设备侧只读调用（GET），使用页面现有同源认证，不允许写操作；
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
    var ALLOWED_JOB_KEYS = ['jobId', 'type', 'section', 'idempotencyKey', 'deadlineAt', 'leaseExpiresAt'];
    var FORBIDDEN_JOB_KEYS = ['url', 'method', 'headers', 'body', 'path'];

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

    function pair(cloudBase, code) {
        if (!cloudBase || !code || typeof cloudBase !== 'string' || typeof code !== 'string') {
            return Promise.resolve({ ok: false, error: 'INVALID_ARGUMENT' });
        }
        var base = cloudBase.replace(/\/+$/, '');
        return Promise.all([
            _deviceGet('/api/device/info'),
            _deviceGet('/api/system/capabilities'),
            _deviceGet('/api/system/info?brief=1').catch(function () { return null; })
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
                capabilities: results[1] || null
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

    // ---------- 任务执行（固定白名单，只读） ----------

    var JOB_HANDLERS = {
        'device.info.read': function () {
            return _deviceGet('/api/device/info');
        },
        'device.capabilities.read': function () {
            return _deviceGet('/api/system/capabilities');
        },
        'device.config.section.read': function (job) {
            var path = SECTION_PATHS[job.section];
            if (!path) {
                return Promise.reject(new Error('INVALID_ARGUMENT'));
            }
            return _deviceGet(path);
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
        return Promise.resolve()
            .then(function () { return JOB_HANDLERS[job.type](job); })
            .then(function (data) {
                var json;
                try {
                    json = JSON.stringify(data === undefined ? null : data);
                } catch (e) {
                    return _submitResult(job.jobId, job.idempotencyKey, 'FAILED', null, 'INVALID_ARGUMENT');
                }
                // 超限结果明确失败，不截断伪造成功
                if (json.length > MAX_RESULT_BYTES) {
                    return _submitResult(job.jobId, job.idempotencyKey, 'FAILED', null, 'RESULT_TOO_LARGE');
                }
                return _submitResult(job.jobId, job.idempotencyKey, 'SUCCEEDED', data, null);
            })
            .catch(function (err) {
                var code = (err && err.message === 'INVALID_ARGUMENT') ? 'INVALID_ARGUMENT' : 'DEVICE_API_FAILED';
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
