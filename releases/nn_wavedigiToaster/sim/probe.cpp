// probe.cpp - dumps internal model signals (Q24 -> volts) as raw float32
// columns for inspection: vco, lfo, arg, vcf, vca, gate
#include <cstdio>
#include <cstdlib>
#include <string>
#include "../src/toaster_model.h"
using namespace wdt;
int main(int argc, char **argv)
{
	Controls c;
	float secs = 3.0f;
	for (int i = 1; i + 1 < argc; i += 2)
	{
		std::string k = argv[i]; float v = (float)atof(argv[i + 1]);
		if (k == "cutoff") c.cutoff = v; else if (k == "reso") c.reso = v; else if (k == "pitch") c.pitch = v;
		else if (k == "lfo") c.lfoRate = v; else if (k == "att") c.attack = v; else if (k == "rel") c.release = v;
		else if (k == "vcfmod") c.vcfMod = v; else if (k == "vcomod") c.vcoMod = v; else if (k == "src") c.source = (int)v;
		else if (k == "secs") secs = v; else if (k == "cv") c.pitchCV = v; else if (k == "repeat") c.repeat = v > 0;
	}
	NoiseToaster m;
	m.ControlAll(c, 0, 0);
	int n = (int)(secs * 48000);
	FILE *f = fopen("probe.f32", "wb");
	for (int i = 0; i < n; i++)
	{
		if (i % 4 == 0) m.Control(c, m.LastLfo(), m.LastArg());
		Inputs in; Outputs o;
		in.gate = (i / 24000) % 2 == 0;
		m.Process(in, o);
		float row[6] = {toVolts(o.vco), toVolts(o.lfo), toVolts(o.arg), toVolts(o.vcf), toVolts(o.vca), (float)o.aregGate};
		fwrite(row, 4, 6, f);
	}
	fclose(f);
}
