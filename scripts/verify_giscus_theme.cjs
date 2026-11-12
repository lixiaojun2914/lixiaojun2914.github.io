// Unit test the generated theme-sync script with a minimal DOM stub.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../public/p/python/index.html'), 'utf8');
const inline = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)]
    .map(match => match[1]).find(script => script.includes('setGiscusTheme'));
assert.ok(inline, 'Giscus theme-sync script must be generated');
const listeners = new Map();
const messages = [];
const document = {
    documentElement: { dataset: { scheme: 'dark' } },
    querySelector: () => ({ contentWindow: { postMessage: (...args) => messages.push(args) } }),
};
const addEventListener = (name, fn) => listeners.set(name, fn);
vm.runInNewContext(inline, { document, addEventListener, window: { addEventListener } });
listeners.get('message')({ origin: 'https://example.com', data: { giscus: {} } });
assert.equal(messages.length, 0, 'Ignore messages from other origins');
listeners.get('message')({ origin: 'https://giscus.app', data: { unrelated: true } });
assert.equal(messages.length, 0, 'Ignore unrelated messages');
listeners.get('message')({ origin: 'https://giscus.app', data: { giscus: { resizeHeight: 100 } } });
assert.equal(messages.at(-1)[0].giscus.setConfig.theme, 'dark_dimmed');
assert.equal(messages.at(-1)[1], 'https://giscus.app');
document.documentElement.dataset.scheme = 'light';
listeners.get('onColorSchemeChange')();
assert.equal(messages.at(-1)[0].giscus.setConfig.theme, 'light');
console.log('PASS: Giscus theme follows light/dark changes and ignores unrelated messages');
