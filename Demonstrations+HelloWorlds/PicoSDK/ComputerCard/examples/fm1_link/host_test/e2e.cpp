// Host-side end-to-end test (run.sh): the real WorkshopLinkHost (Computer side, Pico/TinyUSB
// stubbed) linked byte-for-byte to the real fm1_open APP running in the
// browser simulator compiled natively.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cmath>

#define private public
#include "WorkshopLinkHost.h"
#undef private

extern "C" {
void sim_boot(unsigned clear);
void sim_midi_capture(unsigned enable);
unsigned sim_midi_output(void);
uint8_t *sim_file(void);
const float *sim_render(void);
void sim_control(unsigned kind, unsigned index, int value);
const uint16_t *sim_pixels(void);
void fm1_app_sysex(const uint8_t *m, size_t n);
void fm1_app_midi_received(unsigned source, const uint8_t *m, size_t n);
}

uint32_t fake_us = 1;
static bool mounted = true;
static std::vector<uint8_t> toDevice;      // host -> FM1 USB stream
static std::vector<std::vector<uint8_t>> shortToDevice;
static int failures = 0, checks = 0;
static bool deviceDeaf = false;             // simulate stock firmware: drop link frames

#define CHECK(cond, what) do { ++checks; if (!(cond)) { ++failures; printf("FAIL: %s (line %d)\n", what, __LINE__); } else printf("ok:   %s\n", what); } while (0)

bool tuh_midi_mounted(uint8_t idx) { return idx == 0 && mounted; }
uint32_t tuh_midi_stream_write(uint8_t, uint8_t, const uint8_t *p, uint32_t n)
{
	// Accept at most 40 bytes per call to exercise partial writes.
	uint32_t take = n > 40 ? 40 : n;
	toDevice.insert(toDevice.end(), p, p + take);
	return take;
}
uint32_t tuh_midi_write_flush(uint8_t) { return 0; }
uint32_t tuh_midi_stream_read(uint8_t, uint8_t *, uint8_t *, uint16_t) { return 0; }

// The FM1 SDK assembles SysEx per transport and passes complete frames.
static std::vector<uint8_t> devSysex;
static bool devInSysex = false;
static uint8_t devStatus = 0, devData[2];
static int devCount = 0;
static void DeviceTransport()
{
	for (uint8_t b : toDevice)
	{
		if (b == 0xF0) { devInSysex = true; devSysex.assign(1, b); continue; }
		if (devInSysex)
		{
			devSysex.push_back(b);
			if (b == 0xF7) { devInSysex = false; if (!deviceDeaf || devSysex[2] != 'W') fm1_app_sysex(devSysex.data(), devSysex.size()); }
			continue;
		}
		if (b & 0x80) { devStatus = b; devCount = 0; if (b >= 0xF8) { shortToDevice.push_back({b}); } continue; }
		devData[devCount++] = b;
		int need = ((devStatus & 0xF0) == 0xC0 || (devStatus & 0xF0) == 0xD0) ? 1 : 2;
		if (devCount == need)
		{
			uint8_t m[3] = {devStatus, devData[0], devData[1]};
			shortToDevice.push_back(std::vector<uint8_t>(m, m + 1 + need));
			fm1_app_midi_received(1, m, size_t(1 + need));
			devCount = 0;
		}
	}
	toDevice.clear();
}

static void HostReceiveFromDevice(WorkshopLinkHost &host)
{
	while (unsigned r = sim_midi_output())
	{
		unsigned n = r & 0xFF, port = r >> 8;
		if (port != 1) continue; // only USB reaches the Computer
		const uint8_t *d = sim_file();
		for (unsigned at = 0; at < n; at += 16) // arrive in small USB-sized pieces
			host.OnReceive(0, d + at, n - at < 16 ? n - at : 16);
	}
}

// One audio block of time on both sides (~2.9 ms).
static void Step(WorkshopLinkHost &host, bool hostRunning = true)
{
	fake_us += 2902;
	if (hostRunning) host.Service();
	DeviceTransport();
	sim_render();
	if (hostRunning) HostReceiveFromDevice(host);
	else while (sim_midi_output()) {}
}
static void Run(WorkshopLinkHost &host, int ms, bool hostRunning = true)
{
	for (int t = 0; t < ms * 1000 / 2902; ++t) Step(host, hostRunning);
}

static void SavePPM(const char *path)
{
	const uint16_t *px = sim_pixels();
	FILE *f = fopen(path, "wb");
	fprintf(f, "P6\n240 240\n255\n");
	for (int i = 0; i < 240 * 240; ++i)
	{
		uint16_t c = px[i];
		uint8_t rgb[3] = {uint8_t((c >> 11) * 255 / 31), uint8_t(((c >> 5) & 63) * 255 / 63), uint8_t((c & 31) * 255 / 31)};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

static uint32_t Checksum()
{
	const uint16_t *px = sim_pixels();
	uint32_t h = 2166136261u;
	for (int i = 0; i < 240 * 240; ++i) h = (h ^ px[i]) * 16777619u;
	return h;
}

enum { HOME = 8, ARP = 10, SAVE = 9 };
static void Tap(unsigned button) { sim_control(0, button, 1); sim_control(0, button, 0); }

static void ProtocolTests()
{
	printf("-- protocol\n");
	CHECK(WL_MAX_FRAME <= 128, "largest frame fits the FM1 128-byte output limit");
	srand(1);
	int bad = 0;
	for (int iter = 0; iter < 20000; ++iter)
	{
		uint8_t payload[WL_MAX_PAYLOAD], frame[WL_MAX_FRAME], back[WL_MAX_PAYLOAD], cmd, seq;
		size_t n = size_t(rand()) % (WL_MAX_PAYLOAD + 1);
		for (size_t i = 0; i < n; ++i) payload[i] = uint8_t(rand());
		size_t len = wl_encode(frame, sizeof frame, 0x11, unsigned(iter), payload, n);
		int got = wl_decode(frame, len, &cmd, &seq, back, sizeof back);
		if (got != int(n) || memcmp(payload, back, n) || cmd != 0x11 || seq != (iter & 127)) ++bad;
		for (size_t i = 1; i + 1 < len; ++i) if (frame[i] & 0x80) ++bad;
		// Any single-byte corruption of a data byte must be rejected.
		size_t at = 1 + size_t(rand()) % (len - 2);
		uint8_t old = frame[at];
		frame[at] = uint8_t((old + 1 + rand() % 126) & 0x7F);
		if (frame[at] != old && wl_decode(frame, len, nullptr, nullptr, back, sizeof back) >= 0 && at >= 4) ++bad;
	}
	CHECK(bad == 0, "20000 random payloads round-trip; corruption is detected");
	uint8_t f[16];
	size_t len = wl_encode(f, sizeof f, 1, 0, nullptr, 0);
	CHECK(len == 9 && wl_decode(f, len, nullptr, nullptr, f, 0) == 0, "empty payload frame is 9 bytes");
	uint8_t other[] = {0xF0, 0x7D, 'F', 'M', 2, 0, 0, 0, 0xF7};
	CHECK(!wl_is_link_frame(other, sizeof other), "FM1 transfer protocol frames are not link frames");
}

static void ParserTests()
{
	printf("-- host MIDI parser\n");
	WorkshopLinkHost host;
	host.OnMount(0);
	uint8_t stream[] = {0x90, 60, 100, 62, 90,           // note on + running status
	                    0xF0, 0x7D, 0x00, 0xF8, 0x00, 0xF7, // non-link SysEx with a clock inside
	                    0xC3, 5, 0xB0, 7, 127};
	host.OnReceive(0, stream, 7);
	host.OnReceive(0, stream + 7, sizeof stream - 7);
	uint8_t m[3];
	int n;
	std::vector<std::vector<uint8_t>> got;
	while ((n = host.ReadMIDI(m))) got.push_back(std::vector<uint8_t>(m, m + n));
	CHECK(got.size() == 5, "five messages parsed");
	if (got.size() == 5)
	{
		CHECK(got[0] == std::vector<uint8_t>({0x90, 60, 100}), "note on");
		CHECK(got[1] == std::vector<uint8_t>({0x90, 62, 90}), "running status expanded");
		CHECK(got[2] == std::vector<uint8_t>({0xF8}), "clock inside SysEx delivered");
		CHECK(got[3] == std::vector<uint8_t>({0xC3, 5}), "program change");
		CHECK(got[4] == std::vector<uint8_t>({0xB0, 7, 127}), "control change");
	}
}

int main()
{
	ProtocolTests();
	ParserTests();

	printf("-- link session with the FM1 simulator\n");
	sim_boot(1);
	sim_midi_capture(1);
	for (int i = 0; i < 200; ++i) sim_render(); // let the boot settle

	static WorkshopLinkHost host;
	host.SetName("FM1 LINK DEMO");
	host.SetTitle("WORKSHOP COMPUTER");
	host.ShowOnConnect(true);
	int wMain = host.AddWidget(WL_W_BAR, 8, 36, 72, 26, WL_C_ACCENT, "MAIN", 0, 4095);
	int wX = host.AddWidget(WL_W_BAR, 84, 36, 72, 26, WL_C_ACCENT, "X  CC1", 0, 4095);
	int wY = host.AddWidget(WL_W_BAR, 160, 36, 72, 26, WL_C_ACCENT, "Y  TIME", 0, 4095);
	int wCV1 = host.AddWidget(WL_W_VALUE, 8, 68, 72, 24, WL_C_TEXT, "CV IN 1");
	int wCV2 = host.AddWidget(WL_W_VALUE, 84, 68, 72, 24, WL_C_TEXT, "CV IN 2");
	int wT = host.AddWidget(WL_W_VALUE, 160, 68, 72, 24, WL_C_AMBER, "TRANSPOSE");
	int wP1 = host.AddWidget(WL_W_LED, 8, 98, 72, 12, WL_C_GREEN, "PULSE 1");
	int wP2 = host.AddWidget(WL_W_LED, 84, 98, 72, 12, WL_C_GREEN, "PULSE 2");
	int wSw = host.AddWidget(WL_W_VALUE, 160, 98, 72, 24, WL_C_DIM, "SWITCH");
	int wNote = host.AddWidget(WL_W_TEXT, 8, 126, 224, 18, WL_C_GREEN, "FROM FM1");
	int wScope = host.AddWidget(WL_W_SCOPE, 8, 148, 224, 72, WL_C_ACCENT, "AUDIO IN 1");
	CHECK(wScope == 10, "eleven widgets added");
	host.Start();
	host.OnMount(0);

	host.SetValue(wMain, 3000); host.SetValue(wX, 1200); host.SetValue(wY, 400);
	host.SetValue(wCV1, 25); host.SetValue(wCV2, -12); host.SetValue(wT, 0);
	host.SetValue(wP1, 1); host.SetValue(wP2, 0); host.SetValue(wSw, 1);
	host.SetText(wNote, "A3  VEL 100");
	for (int i = 0; i < 400; ++i) host.PushScope(wScope, int16_t(1800 * sin(i * 0.11) * (0.6 + 0.4 * sin(i * 0.013))));

	Run(host, 600);
	CHECK(host.GetState() == WorkshopLinkHost::Linked, "host reaches Linked");
	CHECK(host.PageVisible(), "FM1 opened its link page on connect (OVERVIEW -> LINK)");
	SavePPM("link_page.ppm");
	uint32_t before = Checksum();

	host.SetValue(wMain, 500);
	Run(host, 100);
	CHECK(Checksum() != before, "value change repaints the FM1 screen");

	printf("-- FM1 controls back to the Computer\n");
	host.EncoderDelta(0);
	sim_control(2, 0, 3);
	Run(host, 50);
	sim_control(2, 0, -1);
	Run(host, 50);
	CHECK(host.EncoderDelta(0) == 2, "encoder 1 detents arrive at the Computer (+3 -1)");
	sim_control(0, SAVE, 1);
	Run(host, 50);
	CHECK(host.Buttons() & (1u << SAVE), "held button reported");
	sim_control(0, SAVE, 0);
	Run(host, 50);
	CHECK(host.Buttons() == 0, "button release reported");

	Tap(HOME);
	Run(host, 50);
	CHECK(!host.PageVisible(), "HOME leaves the link page");
	sim_control(0, HOME, 1); sim_control(0, ARP, 1); sim_control(0, ARP, 0); sim_control(0, HOME, 0);
	Run(host, 50);
	CHECK(host.PageVisible(), "HOME+ARP opens it again");

	printf("-- FM1 restarts while the Computer stays linked\n");
	sim_boot(0);
	sim_midi_capture(1);
	host.SetValue(wMain, 1234); // provokes an update the restarted FM1 has no session for
	Run(host, 1500);
	CHECK(host.GetState() == WorkshopLinkHost::Linked, "host re-linked after NEED_LAYOUT");
	CHECK(host.PageVisible(), "link page shown again");

	printf("-- Computer disappears without BYE\n");
	Run(host, 3500, false);
	SavePPM("link_lost.ppm");
	Run(host, 100);
	CHECK(host.GetState() == WorkshopLinkHost::Linked, "session recovers when the Computer returns (ping/NEED_LAYOUT)");

	printf("-- stock firmware (no link support): MIDI only\n");
	mounted = true;
	deviceDeaf = true;
	host.OnUnmount(0);
	host.OnMount(0);
	Run(host, 2500);
	CHECK(host.GetState() == WorkshopLinkHost::MidiOnly, "host falls back to MidiOnly");
	shortToDevice.clear();
	host.NoteOn(1, 64, 100);
	host.Realtime(0xF8);
	Run(host, 20);
	bool sawNote = false, sawClock = false;
	for (auto &m : shortToDevice)
	{
		if (m == std::vector<uint8_t>({0x91, 64, 100})) sawNote = true;
		if (m == std::vector<uint8_t>({0xF8})) sawClock = true;
	}
	CHECK(sawNote && sawClock, "notes and clock still reach the device");
	deviceDeaf = false;

	printf("\n%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
