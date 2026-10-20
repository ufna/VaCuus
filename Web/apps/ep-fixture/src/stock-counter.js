import { h, render } from 'preact';
import { useState } from 'preact/hooks';

globalThis.EPLOG = [];
const log = (message) => globalThis.EPLOG.push(message);
const documentPrototype = Object.getPrototypeOf(document);
const createTextNode = documentPrototype.createTextNode;
documentPrototype.createTextNode = function (text) {
	log('createTextNode(' + JSON.stringify(text) + ')');
	return createTextNode.call(this, text);
};
const elementPrototype = Object.getPrototypeOf(document.createElement('div'));
const data = Object.getOwnPropertyDescriptor(elementPrototype, 'data');
Object.defineProperty(elementPrototype, 'data', {
	configurable: true,
	get() { return data.get.call(this); },
	set(value) {
		log('data=' + JSON.stringify(value));
		data.set.call(this, value);
	},
});

function Counter() {
	const [count, setCount] = useState(0);
	globalThis.bump = () => setCount((value) => value + 1);
	return h('div', { id: 'ctr' }, 'count:', count);
}

try {
	render(h(Counter, null), document.getElementById('mount'));
} catch (error) {
	log('FIXTURE THREW: ' + error);
}
