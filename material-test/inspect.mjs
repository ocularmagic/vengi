// Inspect the live GPU sampleOutputs buffer after some samples accumulate.
// node inspect.mjs <port> <tag>
import http from 'http';
import fs from 'fs';
import path from 'path';
import os from 'os';
import { spawn } from 'child_process';

const PORT = process.argv[2] || '8781';
const TAG = process.argv[3] || 'insp';
const OUT = path.join(os.tmpdir(), 'vengi-cdp');
const DBG = parseInt(process.env.VENGI_DBG_PORT || '9360', 10);
const CHROME = 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const sleep = ms => new Promise(r => setTimeout(r, ms));
const getJSON = u => new Promise((res, rej) => http.get(u, r => { let d = ''; r.on('data', c => d += c); r.on('end', () => { try { res(JSON.parse(d)); } catch (e) { rej(e); } }); }).on('error', rej));

const chrome = spawn(CHROME, ['--headless=new', '--no-first-run', '--enable-unsafe-webgpu',
  '--enable-webgpu-developer-features', '--disable-gpu-sandbox', '--window-size=1600,1000',
  `--remote-debugging-port=${DBG}`, `--user-data-dir=${path.join(os.tmpdir(), 'vp-' + TAG)}`,
  `http://127.0.0.1:${PORT}/vengi-voxedit.html`], { stdio: ['ignore', 'ignore', 'pipe'] });

let target = null;
for (let i = 0; i < 80; i++) { await sleep(500); try { const l = await getJSON(`http://127.0.0.1:${DBG}/json/list`); target = l.find(t => t.type === 'page' && t.url.includes('vengi-voxedit')); if (target) break; } catch (e) {} }
if (!target) { console.log('NO_TARGET'); chrome.kill(); process.exit(1); }
const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((res, rej) => { ws.addEventListener('open', res, { once: true }); ws.addEventListener('error', rej, { once: true }); });
let id = 0; const pending = new Map(); const logs = [];
ws.addEventListener('message', e => { const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); return; }
  if (m.method === 'Runtime.consoleAPICalled') logs.push((m.params.args || []).map(a => a.value !== undefined ? String(a.value) : (a.description || a.type)).join(' ')); });
const send = (mm, p = {}) => { const my = ++id; return new Promise((res, rej) => { pending.set(my, x => x.error ? rej(new Error(mm + JSON.stringify(x.error))) : res(x.result)); ws.send(JSON.stringify({ id: my, method: mm, params: p })); }); };
async function ev(expr, aw = true) { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: aw, returnByValue: true, timeout: 180000 }); if (r.exceptionDetails) return { __error: r.exceptionDetails.text + ' ' + (r.exceptionDetails.exception?.description || '') }; return r.result.value; }
await send('Runtime.enable'); await send('Page.enable');
const waitFS = async () => { for (let i = 0; i < 80; i++) { if (await ev("(typeof FS!=='undefined')?1:0")) return 1; await sleep(500); } };
async function click(x, y) { await send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y, buttons: 0 }); await sleep(150);
  await send('Input.dispatchMouseEvent', { type: 'mousePressed', x, y, button: 'left', buttons: 1, clickCount: 1 }); await sleep(120);
  await send('Input.dispatchMouseEvent', { type: 'mouseReleased', x, y, button: 'left', buttons: 0, clickCount: 1 }); await sleep(500); }

await waitFS();
await ev(`(async()=>{FS.mkdirTree('/libsdl/vengi/voxedit');
  for(const n of ['material-test.vengi','abandoned_garage_2k.exr']){const r=await fetch('/'+n);FS.writeFile('/libsdl/vengi/voxedit/'+n,new Uint8Array(await r.arrayBuffer()));}
  FS.writeFile('/libsdl/vengi/voxedit/autoexec.cfg','load "/libsdl/vengi/voxedit/material-test.vengi"\\n');
  await new Promise(r=>FS.syncfs(false,r));return 'ok';})()`);
await send('Page.navigate', { url: `http://127.0.0.1:${PORT}/vengi-voxedit.html` });
await sleep(3000); await waitFS(); await sleep(7000);
await click(603, 36); await sleep(800); await click(454, 61);
await sleep(20000);

// Read the denoise chain buffers at one pixel to see where the black starts.
const dump = await ev(`(async()=>{
  const rt = Module.vengiPathTracerWebGPU;
  const rec = [...rt.backends.values()][0];
  const dev = rec.device;
  const w=1280, px=640*w+640;
  async function readVec4(name){
    if(!rec[name]) return name+':missing';
    const rb=dev.createBuffer({size:256,usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});
    const e=dev.createCommandEncoder();
    e.copyBufferToBuffer(rec[name], px*16, rb, 0, 256);
    dev.queue.submit([e.finish()]);
    await rb.mapAsync(GPUMapMode.READ);
    const f=new Float32Array(rb.getMappedRange().slice(0)); rb.unmap(); rb.destroy();
    return name+':['+[f[0],f[1],f[2],f[3]].map(v=>v.toFixed(5))+']';
  }
  async function readU32(name){
    if(!rec[name]) return name+':missing';
    const rb=dev.createBuffer({size:256,usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});
    const e=dev.createCommandEncoder();
    e.copyBufferToBuffer(rec[name], px*4, rb, 0, 256);
    dev.queue.submit([e.finish()]);
    await rb.mapAsync(GPUMapMode.READ);
    const u=new Uint32Array(rb.getMappedRange().slice(0)); rb.unmap(); rb.destroy();
    const v=u[0];
    return name+':rgba('+(v&255)+','+((v>>8)&255)+','+((v>>16)&255)+','+((v>>>24)&255)+')';
  }
  const out=[];
  out.push(await readVec4('denoiseScratchA'));
  out.push(await readVec4('denoiseScratchB'));
  out.push(await readU32('finalImage'));
  // read the CURRENT denoiseParams uniform - proves which mode survived
  {
    const rb=dev.createBuffer({size:48,usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});
    const e=dev.createCommandEncoder();
    e.copyBufferToBuffer(rec.denoiseParams,0,rb,0,48);
    dev.queue.submit([e.finish()]);
    await rb.mapAsync(GPUMapMode.READ);
    const b=rb.getMappedRange().slice(0); rb.unmap(); rb.destroy();
    const dv=new DataView(b);
    out.push('denoiseParams: w='+dv.getUint32(0,true)+' h='+dv.getUint32(4,true)+
      ' exposure='+dv.getFloat32(16,true)+' mode='+dv.getUint32(32,true)+
      ' step='+dv.getUint32(36,true)+' filmic='+dv.getUint32(40,true)+' seed='+dv.getUint32(44,true));
  }
  // and the accumulated radiance at the same pixel for reference
  {
    const rb=dev.createBuffer({size:96,usage:GPUBufferUsage.COPY_DST|GPUBufferUsage.MAP_READ});
    const e=dev.createCommandEncoder();
    e.copyBufferToBuffer(rec.sampleOutputs, px*96, rb, 0, 96);
    dev.queue.submit([e.finish()]);
    await rb.mapAsync(GPUMapMode.READ);
    const f=new Float32Array(rb.getMappedRange().slice(0)); rb.unmap(); rb.destroy();
    const cnt=Math.max(f[21],1);
    out.push('sampleOutputs: rad=['+[f[0],f[1],f[2]].map(v=>v.toFixed(3))+'] count='+f[21]+
      ' mean=['+[f[0]/cnt,f[1]/cnt,f[2]/cnt].map(v=>v.toFixed(5))+'] alpha='+(f[3]/cnt).toFixed(3));
  }
  return out.join(String.fromCharCode(10));
})()`);
console.log('=== sampleOutputs dump ===');
console.log(dump);
console.log('=== logs ===');
console.log(logs.filter(l => /batch|dispatch|denoise|error|warn/i.test(l)).slice(-8).join('\n'));
ws.close(); chrome.kill(); process.exit(0);
