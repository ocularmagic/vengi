// Headless CDP driver that ALSO drives real mouse input into the ImGui canvas.
// node drive2.mjs <port> <tag>
import http from 'http';
import fs from 'fs';
import path from 'path';
import os from 'os';
import { spawn } from 'child_process';

const PORT = process.argv[2] || '8781';
const TAG = process.argv[3] || 'run2';
const WAIT_S = parseInt(process.env.VENGI_WAIT_S || '40', 10);
const OUT = path.join(os.tmpdir(), 'vengi-cdp');
fs.mkdirSync(OUT, { recursive: true });
const DBG = parseInt(process.env.VENGI_DBG_PORT || '9346', 10);
const CHROME = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const PROFILE = path.join(os.tmpdir(), 'vengi-cdp-profile-' + TAG);

const sleep = ms => new Promise(r => setTimeout(r, ms));
const getJSON = url => new Promise((res, rej) => {
  http.get(url, r => { let d = ''; r.on('data', c => d += c); r.on('end', () => { try { res(JSON.parse(d)); } catch (e) { rej(e); } }); }).on('error', rej);
});

const chrome = spawn(CHROME, [
  '--headless=new', '--no-first-run', '--no-default-browser-check',
  '--enable-unsafe-webgpu', '--enable-webgpu-developer-features',
  '--disable-gpu-sandbox', '--window-size=1600,1000',
  `--remote-debugging-port=${DBG}`, `--user-data-dir=${PROFILE}`,
  `http://127.0.0.1:${PORT}/vengi-voxedit.html`,
], { stdio: ['ignore', 'ignore', 'pipe'] });

let target = null;
for (let i = 0; i < 80; i++) {
  await sleep(500);
  try { const l = await getJSON(`http://127.0.0.1:${DBG}/json/list`); target = l.find(t => t.type === 'page' && t.url.includes('vengi-voxedit')); if (target) break; } catch (e) {}
}
if (!target) { console.log('NO_TARGET'); chrome.kill(); process.exit(1); }

const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((res, rej) => { ws.addEventListener('open', res, { once: true }); ws.addEventListener('error', rej, { once: true }); });
let id = 0; const pending = new Map(); const logs = []; const exceptions = [];
ws.addEventListener('message', e => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
  if (m.method === 'Runtime.consoleAPICalled') logs.push((m.params.args || []).map(a => a.value !== undefined ? String(a.value) : (a.description || a.type)).join(' '));
  if (m.method === 'Runtime.exceptionThrown') exceptions.push(m.params.exceptionDetails.text + ' ' + (m.params.exceptionDetails.exception?.description || ''));
});
const send = (method, params = {}) => { const my = ++id; return new Promise((res, rej) => { pending.set(my, m => m.error ? rej(new Error(method + ':' + JSON.stringify(m.error))) : res(m.result)); ws.send(JSON.stringify({ id: my, method, params })); }); };
async function ev(expr, awaitPromise = true) {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise, returnByValue: true, timeout: 180000 });
  if (r.exceptionDetails) return { __error: r.exceptionDetails.text };
  return r.result.value;
}
await send('Runtime.enable'); await send('Page.enable');

async function waitFS() { for (let i = 0; i < 80; i++) { if (await ev("(typeof FS!=='undefined')?1:0")) return true; await sleep(500); } return false; }
async function click(x, y) {
  await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y, buttons: 0 });
  await sleep(120);
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount: 1 });
  await sleep(100);
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', buttons: 0, clickCount: 1 });
  await sleep(400);
}
async function shot(name) {
  const s = await send('Page.captureScreenshot', { format: 'png' });
  const p = path.join(OUT, `${TAG}-${name}.png`);
  fs.writeFileSync(p, Buffer.from(s.data, 'base64'));
  return p;
}
async function pixels() {
  return await ev(`(()=>{const c=document.querySelector('canvas');if(!c)return 'nocanvas';
    const g=document.createElement('canvas');g.width=c.width;g.height=c.height;
    g.getContext('2d').drawImage(c,0,0);
    const d=g.getContext('2d').getImageData(0,0,c.width,c.height).data;
    let nb=0,mx=0;for(let i=0;i<d.length;i+=4){const v=Math.max(d[i],d[i+1],d[i+2]);if(v>8)nb++;if(v>mx)mx=v;}
    return JSON.stringify({w:c.width,h:c.height,nonblack:nb,maxv:mx});})()`, false);
}

await waitFS();
console.log('staging...');
console.log(await ev(`(async()=>{
  FS.mkdirTree('/libsdl/vengi/voxedit');
  for (const n of ['material-test.vengi','abandoned_garage_2k.exr']) {
    const r=await fetch('/'+n); FS.writeFile('/libsdl/vengi/voxedit/'+n, new Uint8Array(await r.arrayBuffer()));
  }
  FS.writeFile('/libsdl/vengi/voxedit/autoexec.cfg','load "/libsdl/vengi/voxedit/material-test.vengi"\\n');
  await new Promise(r=>FS.syncfs(false,r)); return 'ok';})()`));

console.log('reload...');
logs.length = 0;
await send('Page.navigate', { url: `http://127.0.0.1:${PORT}/vengi-voxedit.html` });
await sleep(3000); await waitFS(); await sleep(6000);
console.log('after load pixels:', await pixels());

// locate the "Render" viewport tab. From the earlier screenshot it sits near
// (603,36) in CSS pixels. Click it to make the Render panel the active tab.
const RENDER_TAB = [Number(process.env.VENGI_TAB_X || 603), Number(process.env.VENGI_TAB_Y || 36)];
console.log('clicking Render tab at', RENDER_TAB);
await click(RENDER_TAB[0], RENDER_TAB[1]);
await sleep(1500);
console.log('SHOT_TAB:', await shot('tab'));
console.log('pixels after tab:', await pixels());

console.log('== console so far ==');
console.log(logs.filter(l => /DDA|HDRI|WebGPU|trace|Load file|rror|arn/i.test(l)).slice(-40).join('\n'));
fs.writeFileSync(path.join(OUT, `${TAG}-logs-a.txt`), logs.join('\n'));
console.log('KEEPALIVE_DONE');
// keep the session alive for a follow-up driver? no - dump and exit
ws.close(); chrome.kill(); process.exit(0);
