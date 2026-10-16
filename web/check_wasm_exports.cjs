#!/usr/bin/env node

const fs = require('node:fs');
const path = require('node:path');

async function main() {
  const bindings = fs.readFileSync(
    path.join(__dirname, '../lib/src/bindings/js_extension.dart'), 'utf8');
  const required = [...new Set([...bindings.matchAll(
    /@JS\('Module_soloud\.(_[^']+)'\)/g,
  )].map(match => match[1]))];
  const flavors = process.argv.slice(2);
  if (flavors.length === 0) {
    flavors.push('libflutter_soloud_plugin', 'libflutter_soloud_plugin_mt');
  }
  for (const flavor of flavors) {
    const factory = require(path.join(__dirname, `${flavor}.js`));
    const module = await factory({
      wasmBinary: fs.readFileSync(path.join(__dirname, `${flavor}.wasm`)),
    });
    const missing = required.filter(name => typeof module[name] !== 'function');
    if (missing.length > 0) {
      throw new Error(`${flavor} is missing Dart binding exports: ${missing.join(', ')}`);
    }
    console.log(`${flavor}: ${required.length} Dart binding exports verified`);
  }
}

main().then(() => process.exit(0)).catch(error => {
  console.error(error.message);
  process.exit(1);
});
