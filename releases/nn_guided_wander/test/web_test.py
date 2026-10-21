"""Browser test for the Guided Wander web editor (web/index.html).

Runs the page in headless Chromium with a stand-in for WebMIDI: a fake card
(answering HELLO with STATE and sending STATUS) and two fake 8mus, so the
editor's card protocol, 8mu pickup and pages, LEDs, several 8mus at once and
the hand-over of one 8mu between two editor windows can all be checked.

    python3 web_test.py            (needs: pip install playwright)
"""
import os
import sys
import pathlib
from playwright.sync_api import sync_playwright

HERE = pathlib.Path(__file__).resolve().parent
PAGE = (HERE.parent / "web" / "index.html").as_uri()
SHOTS = pathlib.Path(os.environ.get("SHOTS", HERE / "shots"))

FAKE_MIDI = r"""
(() => {
  const sent = { card: [], mu1: [], mu2: [] };
  const ins = {};
  const mkIn = (id, name, man) => (ins[id] = { id, name, manufacturer: man, state: 'connected', onmidimessage: null });
  const deliver = (id, bytes) => { const p = ins[id]; if (p && p.onmidimessage) p.onmidimessage({ data: new Uint8Array(bytes) }); };
  // The fake card: STATE on HELLO (a known set of values), STATUS on demand
  const cardValues = Array.from({ length: 64 }, (_, i) => i < 32 ? (i * 3) % 128 : 0);
  const sx = (cmd, p) => [0xF0, 0x7D, 0x47, cmd, ...p, 0xF7];
  const mkOut = (id, name, man) => ({ id, name, manufacturer: man, state: 'connected',
    send(d) { const a = Array.from(d); sent[id].push(a);
      if (id === 'card' && a[3] === 0x01) setTimeout(() => deliver('card', sx(0x02, [1, 0, ...cardValues])), 5);
      if (id === 'card' && a[3] === 0x04) for (let i = 0; i < 64; i++) cardValues[i] = a[5 + i];
    } });
  const inputs = new Map([['card', mkIn('card', 'Guided Wander', 'Music Thing')], ['mu1', mkIn('mu1', '8mu', 'Music Thing Modular')], ['mu2', mkIn('mu2', '8mu MIDI 1', 'Music Thing Modular')]]);
  const outputs = new Map([['card', mkOut('card', 'Guided Wander', 'Music Thing')], ['mu1', mkOut('mu1', '8mu', 'Music Thing Modular')], ['mu2', mkOut('mu2', '8mu MIDI 1', 'Music Thing Modular')]]);
  window.__midi = { sent, deliver, cardValues, sx };
  navigator.requestMIDIAccess = async () => ({ inputs, outputs, onstatechange: null });
})();
"""


class Checker:
    def __init__(self):
        self.n = 0
        self.fails = []

    def __call__(self, cond, what):
        self.n += 1
        if not cond:
            self.fails.append(what)
            print("FAIL:", what)


def status_bytes(v1, v2):
    """A STATUS payload: switch up, scale 9 (major pentatonic), clock x2."""
    def voice(note, octv, gate, ratchet, hits, mv):
        p = mv + 8192
        return [note | (octv << 3), gate | (ratchet << 3), hits, 1, 90, p >> 7, p & 127]
    step = 500
    return ([4, 0, 0, 9, 6, 0, 9, 50, 64, 30, (step >> 14) & 127, (step >> 7) & 127, step & 127]
            + voice(*v1) + voice(*v2))


def main():
    ok = Checker()
    SHOTS.mkdir(exist_ok=True)
    with sync_playwright() as pw:
        browser = pw.chromium.launch()
        ctx = browser.new_context(viewport={"width": 1280, "height": 1400})
        ctx.add_init_script(FAKE_MIDI)
        errors = []

        a = ctx.new_page()
        a.on("pageerror", lambda e: errors.append(str(e)))
        a.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
        a.goto(PAGE)
        a.wait_for_timeout(300)

        # No card yet: the page previews on its own
        ok(a.evaluate("state.live.voices[0].hits") > 0, "preview plays before connecting")
        ok(a.is_visible("#sim-row"), "preview scale selector shown with no card")

        a.click("#btn-connect")
        a.wait_for_timeout(200)
        st = a.inner_text("#status-text")
        ok("Guided Wander card" in st and "2 8mus" in st, "status shows card and two 8mus: " + st)
        ok("2 8mus: here" in a.inner_text("#mu-owner"), "owner button counts the 8mus")
        sent = a.evaluate("__midi.sent.card")
        ok(any(m[3] == 0x01 for m in sent), "HELLO sent to the card")
        ok(a.evaluate("state.values[0][5]") == 15, "STATE from the card read in")
        ok(a.evaluate("state.values[4].every(v => v === 0)"), "voice 2 pages zero from the card")

        # A STATUS: the readouts and voices follow it
        a.evaluate("__midi.deliver('card', __midi.sx(0x07, %s))" % status_bytes((2, 3, 4, 1, 7, 1400), (0, 5, 7, 6, 9, 3000)))
        a.wait_for_timeout(120)
        ok(a.inner_text("#ro-scale") == "Major pentatonic", "scale readout from STATUS")
        ok("×2" in a.inner_text("#ro-clock"), "clock ratio readout")
        ok(a.is_hidden("#sim-row"), "preview selector hidden with a card")
        ok("+1.400 V" in a.inner_text("#voice0 [data-k=pitch]"), "voice 1 pitch shown")
        ok("tied" in a.inner_text("#voice1 [data-k=gate]"), "voice 2 gate shown")
        ok("2× new" in a.inner_text("#voice1 [data-k=ratchet]"), "voice 2 ratchet shown")
        # A pentatonic scale: sliders 6-8 of the notes page are out of the scale
        ok(a.evaluate("document.querySelectorAll('.fader.unused').length") == 3, "three note faders unused in a 5-note scale")

        # 8mu button A: pressed and released twice, notes 2 then back to notes
        def press(page, mu, b):
            note = [36, 48, 60, 72][b]
            page.evaluate(f"__midi.deliver('{mu}', [0x90, {note}, 127])")
            page.evaluate(f"__midi.deliver('{mu}', [0x80, {note}, 0])")
            page.wait_for_timeout(30)

        press(a, "mu1", 0)
        ok(a.evaluate("state.page") == 4, "A again: second page (notes 2)")
        ok(any(m[3] == 0x05 and m[4] == 4 for m in a.evaluate("__midi.sent.card")), "PAGE 4 sent to the card")
        a.wait_for_timeout(50)
        ok(a.is_visible("#shared-note"), "shared note shown on an all-zero second page")
        a.screenshot(path=str(SHOTS / "shared_second_page.png"), full_page=True)
        # The page changes on release only
        a.evaluate("__midi.deliver('mu1', [0x90, 36, 127])")
        ok(a.evaluate("state.page") == 4, "no page change on press")
        a.evaluate("__midi.deliver('mu1', [0x80, 36, 0])")
        ok(a.evaluate("state.page") == 0, "A again: back to the first page")
        press(a, "mu1", 0)

        # Fader 4 on notes 2, from the second 8mu: at 0 it meets the stored 0 and follows
        a.evaluate("__midi.deliver('mu2', [0xB0, 37, 0])")
        a.evaluate("__midi.deliver('mu2', [0xB0, 37, 40])")
        a.wait_for_timeout(50)
        ok(a.evaluate("state.values[4][3]") == 40, "second 8mu's fader edits the page")
        ok(any(m[3] == 0x03 and m[4:7] == [4, 3, 40] for m in a.evaluate("__midi.sent.card")), "SET sent to the card")
        ok(a.is_hidden("#shared-note"), "a raised slider gives voice 2 its own page")
        ok(a.evaluate("ownPage(0)"), "ownPage true after a raise")

        # Pickup: on notes 1, fader 1 (stored 0 from the card) - fader at 100 does nothing until it meets it
        press(a, "mu1", 0)
        ok(a.evaluate("state.page") == 0, "back on notes 1")
        stored = a.evaluate("state.values[0][1]")
        a.evaluate("__midi.deliver('mu1', [0xB0, 35, 120])")
        a.evaluate("__midi.deliver('mu1', [0xB0, 35, 121])")
        ok(a.evaluate("state.values[0][1]") == stored, "fader waits to pick up (stored %d)" % stored)
        a.wait_for_timeout(50)
        ok(a.evaluate("getComputedStyle(document.querySelectorAll('.fader .ghost')[1]).display") == "block", "ghost line shows the waiting fader")
        a.evaluate("__midi.deliver('mu1', [0xB0, 35, 1])")      # crosses the stored value
        a.evaluate("__midi.deliver('mu1', [0xB0, 35, 5])")
        ok(a.evaluate("state.values[0][1]") == 5, "fader picks up once it crosses the value")

        # LEDs go to both 8mus, the probabilities first (just touched)
        a.wait_for_timeout(2200)    # past the blink-off pause
        a.evaluate("__midi.deliver('mu1', [0xB0, 35, 6])")
        a.wait_for_timeout(200)
        for mu in ("mu1", "mu2"):
            notes = [m for m in a.evaluate(f"__midi.sent.{mu}") if m[0] == 0x90]
            ok(len(notes) > 0, f"LED messages sent to {mu}")
        # (the card's STATUS keeps coming; without it the page would preview on its own after 3 s)
        status = "__midi.deliver('card', __midi.sx(0x07, %s))" % status_bytes((2, 3, 4, 1, 7, 1400), (0, 5, 7, 6, 9, 3000))
        a.evaluate(status)
        leds = a.evaluate("eightmuLeds(performance.now())")
        vals = a.evaluate("state.values[0].map((v, i) => i < noteCount() ? v : 0)")
        mx = max(vals)
        ok(leds[vals.index(mx)] == 127, "largest probability fully lit")
        ok(all((leds[i] == 0) == (vals[i] == 0) for i in range(8)), "zero probabilities dark, others lit: %s %s" % (leds, vals))
        # 3 s later: what's playing (voice 1's note 2, voice 2 sharing? no - notes 2 is its own now)
        a.evaluate("mu.touchedAt = performance.now() - 3100")
        a.evaluate(status)
        leds = a.evaluate("eightmuLeds(performance.now())")
        ok(leds[2] == 127 and sum(1 for x in leds if x) == 1, "after 3 s: the note playing: %s" % leds)
        # Gates page: a bar to the gate
        press(a, "mu1", 2)
        a.evaluate("mu.touchedAt = performance.now() - 3100")
        a.evaluate(status)
        leds = a.evaluate("eightmuLeds(performance.now())")
        ok(leds == [127, 127, 127, 127, 127, 0, 0, 0], "gate bar to 4/7: %s" % leds)
        a.evaluate("state.live.voices[0].gate = 0")
        leds = a.evaluate("eightmuLeds(performance.now())")
        ok(leds[0] == 22 and sum(leds[1:]) == 0, "a rest: first LED dim: %s" % leds)
        # Ratchets after a touch: the sliders flash on and off through the step
        press(a, "mu1", 3)
        seen_on, seen_off = set(), set()
        for u in [i / 40 for i in range(40)]:
            a.evaluate(f"state.live.progress = {u}")
            l = a.evaluate("eightmuLeds(performance.now())")
            for i, x in enumerate(l):
                (seen_on if x else seen_off).add(i)
        rv = a.evaluate("state.values[3]")
        ok(all((i in seen_on and i in seen_off) for i in range(8) if rv[i] > 0), "ratchet sliders flash in time")
        ok(all(i not in seen_on for i in range(8) if rv[i] == 0), "zero ratchets stay dark")
        # D again: ratchets 2, D again: ratchets 1
        press(a, "mu1", 3)
        ok(a.evaluate("state.page") == 7, "D twice: ratchets 2")
        press(a, "mu1", 3)
        ok(a.evaluate("state.page") == 3, "D three times: ratchets 1 again")

        # The tabs do the same as the buttons
        a.click(".tabpair:nth-child(2) .tab.second")
        ok(a.evaluate("state.page") == 5, "tab B2 selects octaves 2")
        a.click("#btn-copy1")
        ok(a.evaluate("JSON.stringify(state.values[5]) === JSON.stringify(state.values[1])"), "copy first page to second")
        a.click("#btn-zero")
        ok(a.evaluate("!ownPage(1)"), "zero: voice 2 shares octaves again")
        ok(a.evaluate("__midi.cardValues.slice(40, 48).every(v => v === 0)"), "card got the zeroed page")

        # Presets: load one with an independent voice 2, it goes to the card
        a.click("#presets li:nth-child(4) .pname")
        ok(a.evaluate("ownPage(0) && ownPage(1) && ownPage(2) && ownPage(3)"), "Bass and lead preset: voice 2 independent")
        ok(a.evaluate("__midi.cardValues[32]") == 40, "preset sent to the card")
        a.wait_for_timeout(200)
        a.screenshot(path=str(SHOTS / "editor.png"), full_page=True)

        # A second editor window: it takes the 8mu; the first lets go
        b = ctx.new_page()
        b.on("pageerror", lambda e: errors.append("B: " + str(e)))
        b.goto(PAGE)
        b.click("#btn-connect")
        b.wait_for_timeout(300)
        ok("here" in b.inner_text("#mu-owner"), "new window takes the 8mu")
        ok("elsewhere" in a.inner_text("#mu-owner"), "first window lets it go")
        # A fader move reaches both windows (as the 8mu's MIDI does); only B edits
        a_page, b_page = a.evaluate("state.page"), b.evaluate("state.page")
        a_before = a.evaluate(f"state.values[{a_page}][0]")
        b_target = b.evaluate(f"state.values[{b_page}][0]")
        for pg in (a, b):
            pg.evaluate(f"__midi.deliver('mu1', [0xB0, 34, {b_target}])")
            pg.evaluate("__midi.deliver('mu1', [0xB0, 34, 77])")
        ok(b.evaluate(f"state.values[{b_page}][0]") == 77, "owning window edits")
        ok(a.evaluate(f"state.values[{a_page}][0]") == a_before, "other window ignores the 8mu")
        ok(a.evaluate("mu.phys[0]") == 77, "but follows where the fader is")
        # Buttons too
        b_pg = b.evaluate("state.page")
        a.evaluate("__midi.deliver('mu1', [0x90, 48, 127])"); a.evaluate("__midi.deliver('mu1', [0x80, 48, 0])")
        ok(a.evaluate("state.page") == a_page, "other window ignores buttons")
        # Clicking in the first window takes it back
        a.click("h1")
        a.wait_for_timeout(100)
        ok("here" in a.inner_text("#mu-owner"), "click takes the 8mu back")
        ok("elsewhere" in b.inner_text("#mu-owner"), "and the other window lets go")

        # Narrow screen: no horizontal scroll
        a.set_viewport_size({"width": 380, "height": 900})
        a.wait_for_timeout(100)
        ok(a.evaluate("document.documentElement.scrollWidth <= window.innerWidth + 1"), "no horizontal scroll at phone width")
        a.screenshot(path=str(SHOTS / "phone.png"), full_page=False)

        ok(not errors, "no page errors: %s" % errors)
        browser.close()

    print(f"{ok.n} checks, {len(ok.fails)} failures")
    return 1 if ok.fails else 0


if __name__ == "__main__":
    sys.exit(main())
