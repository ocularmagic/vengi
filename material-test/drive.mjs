// Self-contained headless CDP driver for the wasm voxedit path tracer.
// node drive.mjs <port> <outTag>
import http from 'http';
import fs from 'fs';
import path from 'path';
import os from 'os';
import { spawn } from 'child_process';

const PORT = process.argv[2] || '8781';
const TAG = process.argv[3] || 'run1';
const WAIT_S = parseInt(process.env.VENGI_WAIT_S || '45', 10);
const OUT = path.join(os.tmpdir(), 'vengi-cdp');
fs.mkdirSync(OUT, { recursive: true });
const DBG = parseInt(process.env.VENGI_DBG_PORT || '9345', 10);
const CHROME = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const PROFILE = path.join(os.tmpdir(), 'vengi-cdp-profile-' + TAG);

const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const getJSON = (url) => new Promise((res, rej) => {
  http.get(url, r => { let d = ''; r.on('data', c => d += c); r.on('end', () => { try { res(JSON.parse(d)); } catch (e) { rej(e); } }); }).on('error', rej);
});

const chrome = spawn(CHROME, [
  '--headless=new', '--no-first-run', '--no-default-browser-check',
  '--enable-unsafe-webgpu', '--enable-webgpu-developer-features',
  '--disable-gpu-sandbox', '--window-size=1600,1000',
  `--remote-debugging-port=${DBG}`, `--user-data-dir=${PROFILE}`,
  `http://127.0.0.1:${PORT}/vengi-voxedit.html`,
], { stdio: ['ignore', 'ignore', 'pipe'] });
let cerr = ''; chrome.stderr.on('data', d => cerr += d.toString());

let target = null;
for (let i = 0; i < 80; i++) {
  await sleep(500);
  try {
    const list = await getJSON(`http://127.0.0.1:${DBG}/json/list`);
    target = list.find(t => t.type === 'page' && t.url.includes('vengi-voxedit'));
    if (target) break;
  } catch (e) {}
}
if (!target) { console.log('NO_TARGET\n' + cerr.slice(-2000)); chrome.kill(); process.exit(1); }

const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((res, rej) => { ws.addEventListener('open', res, { once: true }); ws.addEventListener('error', rej, { once: true }); });
let id = 0; const pending = new Map();
const logs = []; const exceptions = [];
ws.addEventListener('message', ev => {
  const m = JSON.parse(ev.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
  if (m.method === 'Runtime.consoleAPICalled') {
    logs.push((m.params.args || []).map(a => a.value !== undefined ? String(a.value) : (a.description || a.type)).join(' '));
  }
  if (m.method === 'Runtime.exceptionThrown') {
    exceptions.push(m.params.exceptionDetails.text + ' ' + (m.params.exceptionDetails.exception?.description || ''));
  }
});
function send(method, params = {}) {
  const my = ++id;
  return new Promise((res, rej) => { pending.set(my, m => m.error ? rej(new Error(method + ':' + JSON.stringify(m.error))) : res(m.result)); ws.send(JSON.stringify({ id: my, method, params })); });
}
async function ev(expr, awaitPromise = true) {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise, returnByValue: true, timeout: 180000 });
  if (r.exceptionDetails) return { __error: r.exceptionDetails.text + ' ' + (r.exceptionDetails.exception?.description || '') };
  return r.result.value;
}
await send('Runtime.enable'); await send('Page.enable'); await send('Log.enable');

console.log('== boot ==');
console.log('gpu:', await ev('typeof navigator.gpu'));
// wait for the wasm module + FS
for (let i = 0; i < 60; i++) {
  const ok = await ev("(typeof Module!=='undefined' && Module.FS ? 'FS' : (typeof FS!=='undefined' ? 'GFS' : 'no'))");
  if (ok === 'FS' || ok === 'GFS') { console.log('fs ready:', ok, 'after', i * 0.5, 's'); break; }
  await sleep(500);
}
console.log('canvas:', JSON.stringify(await ev("(()=>{const c=document.querySelector('canvas');return c?[c.width,c.height]:null})()")));
const adapterInfo = await ev(`(async()=>{const a=await navigator.gpu.requestAdapter();if(!a)return 'no-adapter';const i=a.info||{};return JSON.stringify({vendor:i.vendor,arch:i.architecture,desc:i.description});})()`);
console.log('adapter:', adapterInfo);

// stage files into the emscripten FS from HTTP (no giant base64)
const stage = `(async()=>{
  const F = (typeof FS!=='undefined')?FS:Module.FS;
  F.mkdirTree('/libsdl/vengi/voxedit');
  async function put(name){
    const r = await fetch('/'+name);
    const b = new Uint8Array(await r.arrayBuffer());
    F.writeFile('/libsdl/vengi/voxedit/'+name, b);
    return name+':'+b.length;
  }
  const a = await put('material-test.vengi');
  const b = await put('abandoned_garage_2k.exr');
  F.writeFile('/libsdl/vengi/voxedit/autoexec.cfg',
    've_showrender 1\\nload "/libsdl/vengi/voxedit/material-test.vengi"\\ntrace_start\\n');
  await new Promise(r=>F.syncfs(false, r));
  return a+' '+b;
})()`;
console.log('staged:', JSON.stringify(await ev(stage)));

console.log('== reload to run autoexec ==');
logs.length = 0;
await send('Page.navigate', { url: `http://127.0.0.1:${PORT}/vengi-voxedit.html` });
await sleep(3000);
for (let i = 0; i < 60; i++) {
  const ok = await ev("(typeof FS!=='undefined'||(typeof Module!=='undefined'&&Module.FS))?1:0");
  if (ok) break; await sleep(500);
}
await sleep(WAIT_S * 1000);

const shot = await send('Page.captureScreenshot', { format: 'png' });
const shotPath = path.join(OUT, `shot-${TAG}.png`);
fs.writeFileSync(shotPath, Buffer.from(shot.data, 'base64'));

// sample canvas pixels
const px = await ev(`(()=>{const c=document.querySelector('canvas');if(!c)return 'nocanvas';
  const g=document.createElement('canvas');g.width=c.width;g.height=c.height;
  const x=g.getContext('2d');x.drawImage(c,0,0);
  const d=x.getImageData(0,0,c.width,c.height).data;
  let nonblack=0,maxv=0;const hist={};
  for(let i=0;i<d.length;i+=4){const v=Math.max(d[i],d[i+1],d[i+2]);if(v>8)nonblack++;if(v>maxv)maxv=v;}
  return JSON.stringify({w:c.width,h:c.height,nonblack,total:d.length/4,maxv});})()`, false);
console.log('canvas pixels:', px);

console.log('== console (' + logs.length + ' lines) ==');
const interesting = logs.filter(l => /DDA start|HDRI|WebGPU|path tracer|Load file|trace|error|Error|warn|fail|Fail|black|sample/i.test(l));
console.log(interesting.slice(-120).join('\n'));
fs.writeFileSync(path.join(OUT, `logs-${TAG}.txt`), logs.join('\n'));
if (exceptions.length) console.log('== exceptions ==\n' + exceptions.join('\n'));
console.log('SHOT:', shotPath);
console.log('LOGS:', path.join(OUT, `logs-${TAG}.txt`));
ws.close(); chrome.kill();
process.exit(0);
