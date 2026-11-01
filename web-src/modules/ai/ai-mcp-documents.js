(function () {
    'use strict';

    var MAX_FILE_BYTES = 20971520;
    var MAX_CONTEXT_BYTES = 12288;

    function utf8Size(text) {
        return typeof TextEncoder === 'function' ? new TextEncoder().encode(text).length : unescape(encodeURIComponent(text)).length;
    }
    function sha256Fallback(bytes) {
        var K = [0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];
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
                    w[t] = (w[t - 16] + s0 + w[t - 7] + s1) >>> 0;
                }
            }
            var a=H[0],b=H[1],c=H[2],d=H[3],e=H[4],f=H[5],g=H[6],h=H[7];
            for (var i = 0; i < 64; i++) {
                var S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
                var temp1 = (h + S1 + ((e & f) ^ (~e & g)) + K[i] + w[i]) >>> 0;
                var S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
                var temp2 = (S0 + ((a & b) ^ (a & c) ^ (b & c))) >>> 0;
                h=g;g=f;f=e;e=(d + temp1) >>> 0;d=c;c=b;b=a;a=(temp1 + temp2) >>> 0;
            }
            H[0]=(H[0]+a)>>>0;H[1]=(H[1]+b)>>>0;H[2]=(H[2]+c)>>>0;H[3]=(H[3]+d)>>>0;
            H[4]=(H[4]+e)>>>0;H[5]=(H[5]+f)>>>0;H[6]=(H[6]+g)>>>0;H[7]=(H[7]+h)>>>0;
        }
        return H.map(function (value) { return value.toString(16).padStart(8, '0'); }).join('');
    }
    function sha256(file) {
        return file.arrayBuffer().then(function (buffer) {
            if (window.crypto && crypto.subtle) return crypto.subtle.digest('SHA-256', buffer).then(function (digest) {
                return Array.prototype.map.call(new Uint8Array(digest), function (byte) {
                    return byte.toString(16).padStart(2, '0');
                }).join('');
            });
            return sha256Fallback(new Uint8Array(buffer));
        });
    }
    function upload(file, options) {
        var controller = new AbortController();
        var timeoutMs = options && Number.isInteger(options.timeoutMs) ?
            Math.min(120000, Math.max(1, options.timeoutMs)) : 60000;
        var onPageHide = function () { controller.abort(); };
        var timer = setTimeout(onPageHide, timeoutMs);
        window.addEventListener('pagehide', onPageHide, { once: true });
        return uploadConfirmed(file, options, controller.signal).catch(function (error) {
            throw new Error(FastBeeAiMcpTools.normalizeError(error, controller.signal));
        }).finally(function () {
            clearTimeout(timer);
            window.removeEventListener('pagehide', onPageHide);
        });
    }
    function uploadConfirmed(file, options, signal) {
        if (!file || !file.size || file.size > MAX_FILE_BYTES || !options || options.humanConfirmed !== true) {
            return Promise.reject(new Error(file && file.size > MAX_FILE_BYTES ? 'DOCUMENT_TOO_LARGE' : 'AI_CONFIRMATION_REQUIRED'));
        }
        if (!/\.(pdf|doc|docx|png|jpe?g|txt|md|csv|json|ya?ml|xml|html|xlsx)$/i.test(file.name || '')) {
            return Promise.reject(new Error('DOCUMENT_TYPE_UNSUPPORTED'));
        }
        return sha256(file).then(function (digest) {
            FastBeeAiMcpTools.checkSignal(signal);
            return FastBeeAiMcpTools.callTrustedTool('request_document_upload', {
                fileName: file.name, size: file.size, contentType: file.type || 'application/octet-stream', sha256: digest
            }, signal).then(function (ticket) {
                FastBeeAiMcpTools.checkSignal(signal);
                var id = ticket && (ticket.uploadTicket || ticket.ticket);
                if (typeof id !== 'string' || !/^[A-Za-z0-9_-]{8,128}$/.test(id)) throw new Error('MCP_INVALID_RESPONSE');
                return { ticket: id, tools: FastBeeAiMcpTools };
            });
        }).then(function (pending) {
            FastBeeAiMcpTools.checkSignal(signal);
            var uploadBase = pending.tools.baseUrl();
            if (!/^https:\/\//.test(uploadBase || '')) throw new Error('AI_INVALID_MCP_CONFIG');
            return window.fetch(uploadBase.replace(/\/+$/, '') + '/upload/v1/' + encodeURIComponent(pending.ticket), {
                method: 'POST', credentials: 'omit', headers: { 'Content-Type': 'application/octet-stream' }, body: file,
                signal: signal
            }).then(function (response) {
                return FastBeeAiMcpTools.readJson(response, 65536, signal).then(function (body) {
                    return FastBeeAiMcpTools.callTrustedTool('get_document_status', { documentId: body.documentId }, signal);
                });
            });
        });
    }
    function boundedCall(name, args) {
        var controller = new AbortController();
        var onPageHide = function () { controller.abort(); };
        var timer = setTimeout(onPageHide, 20000);
        window.addEventListener('pagehide', onPageHide, { once: true });
        return FastBeeAiMcpTools.callTrustedTool(name, args, controller.signal).catch(function (error) {
            throw new Error(FastBeeAiMcpTools.normalizeError(error, controller.signal));
        }).finally(function () {
            clearTimeout(timer);
            window.removeEventListener('pagehide', onPageHide);
        });
    }
    function search(query) {
        if (typeof query !== 'string' || !query || query.length > 256) return Promise.reject(new Error('AI_INVALID_TOOL_ARGUMENTS'));
        return boundedCall('search_documents', { query: query, pageSize: 10 });
    }
    function facts(documentId, kind) {
        var args = { documentId: documentId, pageSize: 50 };
        if (kind) args.kind = kind;
        return boundedCall('extract_configuration_facts', args);
    }
    function buildDocumentContext(result) {
        var text = '<untrusted_document_context>\n' + JSON.stringify({ untrusted: true, documents: result }) +
            '\n</untrusted_document_context>';
        if (utf8Size(text) > MAX_CONTEXT_BYTES) throw new Error('DOCUMENT_CONTEXT_TOO_LARGE');
        return text;
    }

    window.FastBeeAiMcpDocuments = { upload: upload, search: search, facts: facts, buildDocumentContext: buildDocumentContext };
})();
