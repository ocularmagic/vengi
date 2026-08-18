// Full headless drive: load scene, activate Render tab, click Start path
// tracer, then poll pixels/logs while the render runs.
// node drive3.mjs <port> <tag>
import http from 'http';
import fs from 'fs';
import path from 'path';
import os from 'os';
import { spawn } from 'child_process';

const PORT = process.argv[2] || '8781';
const TAG = process.argv[3] || 'run3';
const POLLS = parseInt(process.env.VENGI_POLLS || '12', 10);
const POLL_MS = parseInt(process.env.VENGI_POLL_MS || '5000', 10);
const OUT = path.join(os.tmpdir(), 'vengi-cdp');
fs.mkdirSync(OUT, { recursive: true });
const DBG = parseInt(process.env.VENGI_DBG_PORT || '9347', 10);
const CHROME = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const PROFILE = path.join(os.tmpdir(), 'vengi-cdp-profile-' + TAG);
const SCENE = process.env.VENGI_SCENE || 'material-test.vengi';

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
const send = (m, p = {}) => { const my = ++id; return new Promise((res, rej) => { pending.set(my, x => x.error ? rej(new Error(m + ':' + JSON.stringify(x.error))) : res(x.result)); ws.send(JSON.stringify({ id: my, method: m, params: p })); }); };
async function ev(expr, awaitPromise = true) {
  const r = await send('Runtime.evaluate', { expression: expr, awaitPromise, returnByValue: true, timeout: 180000 });
  if (r.exceptionDetails) return { __error: r.exceptionDetails.text };
  return r.result.value;
}
await send('Runtime.enable'); await send('Page.enable');
const waitFS = async () => { for (let i = 0; i < 80; i++) { if (await ev("(typeof FS!=='undefined')?1:0")) return 1; await sleep(500); } return 0; };
async function click(x, y) {
  await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y, buttons: 0 }); await sleep(150);
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount: 1 }); await sleep(120);
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', buttons: 0, clickCount: 1 }); await sleep(500);
}
async function shot(n) { const s = await send('Page.captureScreenshot', { format: 'png' }); const p = path.join(OUT, `${TAG}-${n}.png`); fs.writeFileSync(p, Buffer.from(s.data, 'base64')); return p; }
// sample only the Render panel area of the canvas (CSS 320..1200 x 75..640)
async function panelPixels() {
  return await ev(`(()=>{const c=document.querySelector('canvas');if(!c)return 'nocanvas';
    const sx=c.width/window.innerWidth, sy=c.height/window.innerHeight;
    const x0=Math.round(325*sx),y0=Math.round(80*sy),w=Math.round(870*sx),h=Math.round(550*sy);
    const g=document.createElement('canvas');g.width=c.width;g.height=c.height;
    g.getContext('2d').drawImage(c,0,0);
    const d=g.getContext('2d').getImageData(x0,y0,w,h).data;
    let nb=0,mx=0,sum=0;const n=d.length/4;
    for(let i=0;i<d.length;i+=4){const v=Math.max(d[i],d[i+1],d[i+2]);sum+=v;if(v>8)nb++;if(v>mx)mx=v;}
    return JSON.stringify({region:[x0,y0,w,h],nonblack:nb,total:n,pct:+(100*nb/n).toFixed(1),maxv:mx,avg:+(sum/n).toFixed(1)});})()`, false);
}

await waitFS();
await ev(`(async()=>{ FS.mkdirTree('/libsdl/vengi/voxedit');
  for (const n of ['${SCENE}','abandoned_garage_2k.exr']) { const r=await fetch('/'+n); FS.writeFile('/libsdl/vengi/voxedit/'+n, new Uint8Array(await r.arrayBuffer())); }
  FS.writeFile('/libsdl/vengi/voxedit/autoexec.cfg','load "/libsdl/vengi/voxedit/${SCENE}"\\n');
  await new Promise(r=>FS.syncfs(false,r)); return 'ok';})()`);
logs.length = 0;
await send('Page.navigate', { url: `http://127.0.0.1:${PORT}/vengi-voxedit.html` });
await sleep(3000); await waitFS(); await sleep(7000);

await click(603, 36);            // Render tab
await sleep(1000);
console.log('--- clicking Start path tracer ---');
await click(454, 61);            // Start path tracer
await sleep(1500);

for (let i = 0; i < POLLS; i++) {
  await sleep(POLL_MS);
  const px = await panelPixels();
  console.log(`t=${((i + 1) * POLL_MS / 1000).toFixed(0)}s`, px);
  const newLogs = logs.splice(0);
  if (newLogs.length) console.log('  LOG: ' + newLogs.join('\n  LOG: '));
}
console.log('SHOT:', await shot('render'));
if (exceptions.length) console.log('EXC:\n' + exceptions.join('\n'));
fs.writeFileSync(path.join(OUT, `${TAG}-logs.txt`), logs.join('\n'));
ws.close(); chrome.kill(); process.exit(0);
