// DOM + mock API checks. Radio, LCD, flash persistence and CSS layout need hardware/browser checks.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const {JSDOM, VirtualConsole} = require('jsdom');
const delay = milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds));

async function main() {
  const errors = [];
  const console = new VirtualConsole();
  console.on('jsdomError', error => errors.push(error.message));
  let status = {
    connected:false, state:'idle', message:'와이파이를 선택해주세요.', ssid:'',
    ip:'', use24Hour:true, brightness:180, epoch:0
  };
  let scans = 0, submissions = 0, finishes = 0, expectPassword = 'wrong-pass';
  const hostileSsid = '<img src=x onerror=alert(1)>';
  const html = fs.readFileSync(path.join(__dirname, '../web/index.html'), 'utf8')
    .replace('__TOKEN__', 'test-token');
  const dom = new JSDOM(html, {
    url:'http://192.168.4.1/', runScripts:'dangerously', virtualConsole:console,
    beforeParse(window) {
      window.TextEncoder = TextEncoder;
      window.AbortController = AbortController;
      window.fetch = async (url, options) => {
        const fields = new URLSearchParams(options.body || '');
        let body = {}, code = 200;
        if (options.method === 'POST') assert.equal(fields.get('token'), 'test-token');
        if (url === '/api/status') body = {...status};
        else if (url === '/api/scan') {
          if (++scans === 1) {code = 202; body = {message:'검색 중'};}
          else body = {networks:[
            {ssid:'Dowon Studio', rssi:-50, open:false},
            {ssid:hostileSsid, rssi:-70, open:false},
            {ssid:'Guest', rssi:-60, open:true}
          ]};
        } else if (url === '/api/wifi') {
          submissions++;
          assert.equal(fields.get('password'), expectPassword);
          assert.equal(fields.get('ssid'), 'Dowon Studio');
          status = {...status, connected:false, state:'connecting', message:'연결 중'};
          code = 202; body = {message:'연결 중'};
        } else if (url === '/api/clock') {
          status.use24Hour = fields.get('format') === '24';
          status.brightness = Number(fields.get('brightness'));
          body = {message:'시계 설정을 저장했어요.'};
        } else if (url === '/api/finish') {
          finishes++; body = {message:'설정을 마쳤어요.'};
        } else throw new Error('Unexpected endpoint ' + url);
        return {ok:code < 400, status:code, json:async () => body};
      };
    }
  });
  const $ = id => dom.window.document.getElementById(id);
  const waitFor = async predicate => {
    for (let i = 0; i < 100; i++) {if (predicate()) return; await delay(50);}
    throw new Error('Timed out waiting for UI');
  };
  try {
    await waitFor(() => $('status').textContent.includes('선택'));
    assert.equal($('time').textContent, '--:--');
    assert.equal($('finish').disabled, true);
    $('scan').click();
    await waitFor(() => dom.window.document.querySelectorAll('.network').length === 3);
    assert.equal($('networks').querySelectorAll('img').length, 0);
    assert.ok($('networks').textContent.includes(hostileSsid));
    $('networks').children[2].click();
    assert.equal($('open').checked, true);
    assert.equal($('password').disabled, true);
    $('networks').children[0].click();
    assert.equal($('password').disabled, false);
    $('password').value = 'short'; $('connect').click();
    assert.match($('wifiMessage').textContent, /8~63/);
    assert.equal(submissions, 0);
    $('ssid').value = '가'.repeat(11); $('password').value = 'good-password'; $('connect').click();
    assert.equal(submissions, 0, 'UTF-8 SSID byte limit');
    $('ssid').value = 'Dowon Studio'; $('password').value = expectPassword;
    $('showPassword').click(); assert.equal($('password').type, 'text');
    $('connect').click(); await waitFor(() => submissions === 1);
    await delay(20);
    assert.equal($('connect').disabled, true);
    status = {...status, state:'failed', message:'비밀번호를 확인해주세요.'};
    await waitFor(() => $('status').textContent.includes('비밀번호를 확인'));
    assert.equal($('connect').disabled, false);
    assert.equal($('finish').disabled, true);
    expectPassword = 'good-password'; $('password').value = expectPassword;
    $('connect').click(); await waitFor(() => submissions === 2);
    await delay(20);
    status = {...status, connected:true, state:'connected', ssid:'Dowon Studio', message:'저장했어요.', epoch:1794652200};
    await waitFor(() => !$('finish').disabled);
    assert.notEqual($('time').textContent, '--:--');
    assert.equal($('password').value, '');
    $('format').value = '12'; $('brightness').value = '50'; $('saveClock').click();
    await waitFor(() => $('clockMessage').textContent.includes('저장'));
    assert.equal(status.use24Hour, false);
    assert.equal(status.brightness, 50);
    $('finish').click();
    await waitFor(() => $('status').textContent.includes('설정 완료'));
    await delay(2100);
    assert.equal(finishes, 1);
    assert.equal($('scan').disabled, true);
    assert.equal($('connect').disabled, true);
    assert.deepEqual(errors, []);
    process.stdout.write('PASS: initial portal, async scan, SSID escaping, open-network toggle, byte validation, failed/retried connection, clock settings, finished state.\n');
  } finally {dom.window.close();}
}
main().catch(error => {console.error(error); process.exitCode = 1;});
