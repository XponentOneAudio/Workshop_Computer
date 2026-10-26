// Runs the fixed-point NAM engine on a desktop computer, for nam2header.py --verify
//
// Usage: host_check <model index> <input.raw> <output.raw>
// Input is int32 Q12 samples, output int32 Q16.

#include <cstdio>
#include <cstdlib>
#include <vector>
#include MODEL_HEADER

static int16_t ring[NAM_RING_WORDS];

int main(int argc, char **argv)
{
	if (argc != 4) return 1;
	int idx = atoi(argv[1]);
	if (idx < 0 || idx >= kNumModels) return 1;

	FILE *fi = fopen(argv[2], "rb");
	if (!fi) return 1;
	std::vector<int32_t> x;
	int32_t s;
	while (fread(&s, sizeof(s), 1, fi) == 1) x.push_back(s);
	fclose(fi);

	const nam_fx::Model *m = kModels[idx];
	static nam_fx::Engine engine;
	engine.Init(m, ring);

	// Run the model split in two, as on the Computer
	std::vector<int32_t> y(x.size());
	for (size_t i = 0; i < x.size(); i++)
	{
		nam_fx::Frame f;
		f.x = x[i];
		engine.Run(f, 0, m->split);
		engine.Run(f, m->split, m->num_units);
		y[i] = f.y;
	}

	FILE *fo = fopen(argv[3], "wb");
	if (!fo) return 1;
	fwrite(y.data(), sizeof(int32_t), y.size(), fo);
	fclose(fo);
	return 0;
}
