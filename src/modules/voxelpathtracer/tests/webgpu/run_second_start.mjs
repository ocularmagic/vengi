import { spawn } from 'child_process';
import http from 'http';

const chrome = 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
const url = 'http://127.0.0.1:8765/src/modules/voxelpathtracer/tests/webgpu/PathTracerTraversalWebGPU.html';
const port = 9333;

function getJson(path) {
	return new Promise((resolve, reject) => {
		http.get({ host: '127.0.0.1', port, path }, res => {
			let data = '';
			res.on('data', chunk => { data += chunk; });
			res.on('end', () => {
				try {
					resolve(JSON.parse(data));
				} catch (error) {
					reject(new Error(data));
				}
			});
		}).on('error', reject);
	});
}

async function waitForTargets() {
	for (let i = 0; i < 40; ++i) {
		try {
			const targets = await getJson('/json/list');
			if (Array.isArray(targets) && targets.length) {
				return targets;
			}
		} catch (error) {
		}
		await new Promise(resolve => setTimeout(resolve, 250));
	}
	throw new Error('no chrome debug targets');
}

const proc = spawn(chrome, [
	'--headless=new',
	'--disable-gpu-sandbox',
	'--enable-unsafe-webgpu',
	'--enable-webgpu-developer-features',
	`--remote-debugging-port=${port}`,
	'--user-data-dir=C:\\Users\\james\\AppData\\Local\\Temp\\vengi-webgpu-cdp2-' + Date.now() + '',
	'about:blank',
], { stdio: 'ignore' });

try {
	const targets = await waitForTargets();
	const page = targets.find(target => target.type === 'page') || targets[0];
	console.error('targets=' + JSON.stringify(targets.map(t => ({type: t.type, url: t.url, title: t.title}))));
	const ws = new WebSocket(page.webSocketDebuggerUrl);
	await new Promise((resolve, reject) => {
		ws.addEventListener('open', resolve, { once: true });
		ws.addEventListener('error', reject, { once: true });
	});
	let nextId = 0;
	const pending = new Map();
	ws.addEventListener('message', event => {
		const reply = JSON.parse(event.data);
		if (reply.id && pending.has(reply.id)) {
			pending.get(reply.id)(reply);
			pending.delete(reply.id);
		}
	});
	const send = (method, params) => new Promise(resolve => {
		const id = ++nextId;
		pending.set(id, resolve);
		ws.send(JSON.stringify({ id, method, params }));
	});
	await send('Page.enable');
	await send('Runtime.enable');
	ws.addEventListener('message', event => {
		const msg = JSON.parse(event.data);
		if (msg.method === 'Runtime.consoleAPICalled') {
			const args = msg.params.args.map(a => (a.value !== undefined ? a.value : a.description)).join(' ');
			console.error('BROWSER[' + msg.params.type + ']', args);
		}
	});
	await send('Page.navigate', { url });
	await new Promise(resolve => setTimeout(resolve, 1000));
	const deadline = Date.now() + 120000;
	let last = '';
	while (Date.now() < deadline) {
		const reply = await send('Runtime.evaluate', {
			expression: "(() => { const el = document.getElementById('result'); return el ? (el.dataset.state + '|' + el.textContent) : ('missing:' + document.title + ':' + location.href); })()",
			returnByValue: true,
		});
		last = reply.result && reply.result.result ? reply.result.result.value : JSON.stringify(reply);
		if (typeof last === 'string' && (last.startsWith('passed|') || last.startsWith('failed|'))) {
			console.log(last);
			ws.close();
			proc.kill();
			process.exit(last.startsWith('passed|') ? 0 : 1);
		}
		await new Promise(resolve => setTimeout(resolve, 1000));
	}
	console.log('timeout:' + last);
	ws.close();
	proc.kill();
	process.exit(2);
} catch (error) {
	console.error(error);
	proc.kill();
	process.exit(1);
}
