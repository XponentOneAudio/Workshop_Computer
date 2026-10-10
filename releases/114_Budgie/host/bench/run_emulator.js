// Run the benchmark in the rp2040js emulator and print its UART output.
//
//   npm install rp2040js@1.1.1
//   curl -O https://raw.githubusercontent.com/wokwi/rp2040js/main/demo/bootrom.ts
//   node run_emulator.js budgie_bench.uf2 bootrom.ts
//
// rp2040js counts Cortex-M0+ cycles per instruction, so the numbers are a
// good guide but not a substitute for the BUDGIE_PROFILE build on hardware.

const fs = require('fs');
const { Simulator } = require('rp2040js');

const [uf2Path, bootromPath] = process.argv.slice(2);
const words = fs.readFileSync(bootromPath, 'utf8').match(/0x[0-9a-fA-F]+/g).map(Number);
const sim = new Simulator();
const mcu = sim.rp2040;
mcu.loadBootrom(new Uint32Array(words));

const buf = fs.readFileSync(uf2Path);
for (let o = 0; o + 512 <= buf.length; o += 512) {
  const addr = buf.readUInt32LE(o + 12), size = buf.readUInt32LE(o + 16);
  mcu.flash.set(buf.subarray(o + 32, o + 32 + size), addr - 0x10000000);
}

let out = '';
mcu.uart[0].onByte = (v) => {
  const ch = String.fromCharCode(v);
  process.stdout.write(ch);
  out += ch;
  if (out.includes('BENCH DONE')) process.exit(0);
};
mcu.core.PC = 0x10000000;
sim.execute();
setTimeout(() => { console.log('\nTIMEOUT'); process.exit(1); }, 600000);
