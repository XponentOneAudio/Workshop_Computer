// Host build of the boot-time WDF tables, for the cycle bench.
#include <cstdio>
#include "hypster_core.h"
int main(int argc, char **argv)
{
	static hyp::ShaperTables t;
	static hyp::ShapeTables s;
	hyp::BuildShaperTables(t);
	hyp::BuildShapeTables(s, hyp::ShapeMode::Opt2Asym, 0.5f);
	FILE *f = fopen(argc > 1 ? argv[1] : "tables.bin", "wb");
	fwrite(&t, sizeof t, 1, f);
	fwrite(&s, sizeof s, 1, f);
	fclose(f);
	printf("wrote %zu + %zu bytes\n", sizeof t, sizeof s);
}
