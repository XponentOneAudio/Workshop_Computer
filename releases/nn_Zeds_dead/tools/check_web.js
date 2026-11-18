// ============================================================================
//  check_web.js — prove the web editor's maths matches the firmware's
// ============================================================================
//
//  The editor (web/index.html) contains a JavaScript copy of zplane.h and
//  bank.h between the markers "BEGIN ZPLANE CORE" and "END ZPLANE CORE".
//  This script runs that copy in Node and compares it with reference numbers
//  printed by the C++ host test:
//
//    * every factory slot must encode to EXACTLY the same SysEx bytes
//      (so the presets and the float encoding are bit-for-bit identical), and
//    * the designed frequency responses must agree to within 0.05 dB
//      (C++ uses 32-bit floats, JavaScript 64-bit, so tiny differences are
//      expected).
//
//  Usage (from this tools/ directory):
//      g++ -O2 -std=c++17 -I.. host_test.cpp -o host_test
//      ./host_test vectors > vectors.txt
//      node check_web.js vectors.txt
// ============================================================================
"use strict";
const fs = require("fs");
const path = require("path");

const html = fs.readFileSync(path.join(__dirname, "..", "web", "index.html"), "utf8");
const m = html.match(/\/\/ ===== BEGIN ZPLANE CORE =====([\s\S]*?)\/\/ ===== END ZPLANE CORE =====/);
if (!m) { console.error("Could not find the ZPLANE CORE block in web/index.html"); process.exit(1); }
const ZP = new Function(m[1] + "\nreturn ZP;")();

const lines = fs.readFileSync(process.argv[2] || "vectors.txt", "utf8").trim().split("\n");
const banks = [ZP.factoryBank(0), ZP.factoryBank(1)];
let failures = 0, slots = 0, resps = 0, worst = 0;

for (const line of lines) {
  const parts = line.trim().split(/\s+/);
  if (parts[0] === "SLOT") {
    const b = +parts[1], s = +parts[2];
    const js = ZP.encodeSlot(banks[b].slots[s]).map(v => v.toString(16).padStart(2, "0")).join("");
    if (js !== parts[3]) { failures++; console.log(`FAIL: slot ${b}/${s} encodes differently`); }
    // And decoding the C++ bytes gives back the same slot.
    const bytes = parts[3].match(/../g).map(h => parseInt(h, 16));
    const back = ZP.encodeSlot(ZP.decodeSlot(bytes, 0)).map(v => v.toString(16).padStart(2, "0")).join("");
    if (back !== parts[3]) { failures++; console.log(`FAIL: slot ${b}/${s} does not survive decode`); }
    slots++;
  } else if (parts[0] === "RESP") {
    const [b, morph, res, cut] = parts.slice(1, 5).map(Number);
    const want = parts.slice(5).map(Number);
    const hzs = [50, 200, 700, 1000, 1500, 4000, 12000];
    const d = ZP.designMorph(banks[b], cut, morph, res);
    hzs.forEach((hz, i) => {
      const got = ZP.responseDb(d, hz), err = Math.abs(got - want[i]);
      // Compare only where the response is audible-ish; deep in a notch the
      // float32 C++ value is mostly rounding noise.
      if (want[i] > -80 && err > worst) worst = err;
      if (want[i] > -80 && err > 0.05) { failures++; console.log(`FAIL: bank ${b} morph ${morph} res ${res} cutoff ${cut} @${hz} Hz: C++ ${want[i]} JS ${got.toFixed(4)}`); }
    });
    resps++;
  }
}
console.log(`[web] ${slots} slots byte-identical check, ${resps} responses compared, worst difference ${worst.toFixed(4)} dB`);
console.log(failures ? `${failures} CHECK(S) FAILED` : "WEB EDITOR MATCHES FIRMWARE");
process.exit(failures ? 1 : 0);
