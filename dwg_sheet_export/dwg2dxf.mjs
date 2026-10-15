// DWG -> DXF conversion using LibreDWG (WebAssembly build).
// Usage: node dwg2dxf.mjs <input.dwg> <output.dxf>
import { LibreDwg } from '@mlightcad/libredwg-web';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const [, , inPath, outPath] = process.argv;
if (!inPath || !outPath) {
  console.error('usage: node dwg2dxf.mjs <input.dwg> <output.dxf>');
  process.exit(2);
}
const here = path.dirname(fileURLToPath(import.meta.url));
const wasmDir = path.join(here, 'node_modules', '@mlightcad', 'libredwg-web', 'wasm') + path.sep;

const buf = fs.readFileSync(inPath);
const ab = buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.byteLength);
const lib = await LibreDwg.create(wasmDir);
const dxf = lib.dwg_write_dxf(ab);
if (!dxf) {
  console.error('LibreDWG could not convert this file to DXF');
  process.exit(1);
}
fs.writeFileSync(outPath, dxf);
console.log(`DXF written: ${outPath} (${dxf.length} bytes)`);
