// SPDX-License-Identifier: GPL-3.0-or-later
// 执行固件内嵌的真实网页脚本；DOM/网络由主机替身提供。
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(process.argv[2], 'utf8');
const scripts = [...source.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(match => match[1]);
assert.equal(scripts.length, 2);
for (const script of scripts) new vm.Script(script);
function environment() {
    const elements = new Map(), requests = [], intervals = [], documentEvents = {}, windowEvents = {};
    const document = {
        hidden: false,
        getElementById(id) {
            if (!elements.has(id)) elements.set(id, {
                style: {}, classList: { add() {}, remove() {}, toggle() {} }, events: {},
                addEventListener(name, callback) { this.events[name] = callback; },
                getBoundingClientRect() { return { left: 0, top: 0, width: 300, height: 300 }; }
            });
            return elements.get(id);
        },
        addEventListener(name, callback) { documentEvents[name] = callback; }
    };
    const context = vm.createContext({
        document, window: { addEventListener(name, callback) { windowEvents[name] = callback; }, location: {} },
        console: { log() {}, error() {} }, AbortController,
        setTimeout() { return 1; }, clearTimeout() {},
        setInterval(callback) { intervals.push(callback); },
        fetch(url, options = {}) { return new Promise((resolve, reject) => requests.push({ url, options, resolve, reject })); },
        FormData: class { entries() { return [['forward_ms', '1200'], ['def_speed_pct', '60']][Symbol.iterator](); } },
        alert() {}
    });
    return { context, requests, elements, intervals, document, documentEvents, windowEvents };
}
const flush = async () => { for (let i = 0; i < 15; i++) await Promise.resolve(); };
const ok = (request, json = { status: 'ok', accepted: true }) => request.resolve({ ok: true, json: async () => json });
const body = request => JSON.parse(request.options.body);
async function main() {
    let env = environment();
    vm.runInContext(scripts[0], env.context);
    vm.runInContext('isDragging = true; sendControl(4, 80);', env.context);
    vm.runInContext('stopCar();', env.context);
    assert.equal(env.requests.length, 1); // 停止等待当前请求，不能被丢弃或并发乱序
    ok(env.requests[0]); await flush();
    assert.equal(env.requests.length, 2);
    assert.deepEqual(body(env.requests[1]), { direction: 0, speed: 0 });
    ok(env.requests[1]); await flush();
    assert.equal(vm.runInContext('isDragging', env.context), false);
    vm.runInContext('isDragging = true; sendControl(4, 80);', env.context);
    ok(env.requests[2]); await flush();
    env.intervals[0]();
    assert.deepEqual(body(env.requests[3]), { direction: 4, speed: 80 });
    ok(env.requests[3]); await flush();
    vm.runInContext('executeAction("dance");', env.context);
    assert.equal(env.requests[4].url, '/api/control');
    ok(env.requests[4]); await flush();
    assert.equal(env.requests[5].url, '/api/motor/action');
    assert.deepEqual(body(env.requests[5]), { action: 'dance' });
    ok(env.requests[5]); await flush();
    env.windowEvents.blur();
    assert.deepEqual(body(env.requests[6]), { direction: 0, speed: 0 });
    ok(env.requests[6]); await flush();
    env.document.hidden = true;
    env.documentEvents.visibilitychange();
    assert.deepEqual(body(env.requests[7]), { direction: 0, speed: 0 });
    ok(env.requests[7]); await flush();
    assert.equal(typeof env.elements.get('joystick-container').events.touchcancel, 'function');
    env.windowEvents.pagehide();
    assert.equal(env.requests[8].options.keepalive, true);
    ok(env.requests[8]); await flush();
    vm.runInContext('isDragging = true; sendControl(4, 80);', env.context);
    env.requests[9].resolve({ ok: false }); await flush();
    assert.equal(vm.runInContext('isDragging', env.context), false);
    assert.deepEqual(body(env.requests[10]), { direction: 0, speed: 0 });
    ok(env.requests[10]); await flush();
    const requestCount = env.requests.length;
    env.intervals[0]();
    assert.equal(env.requests.length, requestCount); // 网络失败不能持续续期运动

    env = environment();
    vm.runInContext(scripts[0], env.context);
    vm.runInContext('isDragging = true;', env.context);
    env.elements.get('joystick-container').events.touchcancel();
    assert.equal(vm.runInContext('isDragging', env.context), false);
    assert.deepEqual(body(env.requests[0]), { direction: 0, speed: 0 });
    ok(env.requests[0]); await flush();

    // 等待停车时收到新的停止，不能在响应到达后补发旧的舞蹈。
    env = environment();
    vm.runInContext(scripts[0], env.context);
    vm.runInContext('executeAction("dance"); stopCar();', env.context);
    ok(env.requests[0]); await flush();
    assert.deepEqual(body(env.requests[1]), { direction: 0, speed: 0 });
    ok(env.requests[1]); await flush();
    assert.equal(env.requests.length, 2);
    assert.ok(env.requests.every(request => request.url === '/api/control'));
    // 停车请求失败后也不能继续发送动作按钮请求。
    env = environment();
    vm.runInContext(scripts[0], env.context);
    vm.runInContext('executeAction("dance");', env.context);
    env.requests[0].resolve({ ok: false }); await flush();
    assert.equal(env.requests.length, 2);
    assert.ok(env.requests.every(request => request.url === '/api/control'));
    ok(env.requests[1]); await flush();

    env = environment();
    vm.runInContext(scripts[1], env.context);
    vm.runInContext("window.onload();", env.context);
    ok(env.requests[0], { forward_ms: 1500, backward_ms: 1500, left_turn_ms: 600, right_turn_ms: 600, spin_ms: 2500, def_speed_pct: 80 });
    await flush();
    const form = [...env.elements.values()].find(element => element.events.submit);
    assert.ok(form);
    form.events.submit.call(form, { preventDefault() {} });
    assert.deepEqual(body(env.requests[1]), { forward_ms: 1200, def_speed_pct: 60 });
    ok(env.requests[1], { status: 'success' }); await flush();
    process.stdout.write('PASS: embedded browser scripts, queued stop, repeated heartbeat, action ordering, blur/hide/cancel, network failure, numeric config\n');
}
main().catch(error => { console.error(error); process.exitCode = 1; });
