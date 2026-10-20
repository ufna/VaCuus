import { h, render } from 'preact';

const log = (message) => globalThis.EPLOG.push(message);
globalThis.EPLOG = [];
const result = globalThis.EPRUN = {};

try {
	render(h('div', { id: 'a' }, h('span', { id: 'b' })), document.getElementById('mount'));
	result.mount = document.getElementById('mount').innerRML;
	render(h('div', { id: 'f' }), document.getElementById('prefilled'));
	result.adopt = document.getElementById('prefilled').innerRML;
	const row = (key) => h('div', { key, id: 'row' + key });
	const order = () => document.getElementById('list').children.map((child) => child.id).join(',');
	const mount = document.getElementById('mount2');
	render(h('div', { id: 'list' }, [1, 2, 3, 4].map(row)), mount);
	result.mounted = order();
	render(h('div', { id: 'list' }, [4, 3, 2, 1].map(row)), mount);
	result.reversed = order();
} catch (error) {
	log('FIXTURE THREW: ' + error);
	result.error = String(error);
}
