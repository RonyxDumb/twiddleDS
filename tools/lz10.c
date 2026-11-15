// GBA/DS BIOS LZ77 (type 0x10) compressor.  usage: lz10 in out
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HBITS 15
#define HSIZE (1 << HBITS)

int main(int argc, char **argv) {
	if (argc < 3) return 1;
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	unsigned char *d = malloc(n + 3);
	if (fread(d, 1, n, f) != (size_t)n) return 1;
	fclose(f);
	unsigned char *o = malloc(n * 2 + 16);
	long op = 0;
	o[op++] = 0x10; o[op++] = n & 255; o[op++] = (n >> 8) & 255; o[op++] = (n >> 16) & 255;
	int *head = malloc(HSIZE * sizeof(int));
	int *prev = malloc(n * sizeof(int));
	for (int i = 0; i < HSIZE; i++) head[i] = -1;
	long i = 0;
#define HASH(p) ((((unsigned)d[p] << 10) ^ ((unsigned)d[p + 1] << 5) ^ d[p + 2]) & (HSIZE - 1))
#define INSERT(p) do { if ((p) + 3 <= n) { unsigned h = HASH(p); prev[p] = head[h]; head[h] = (p); } } while (0)
	while (i < n) {
		long fp = op++;
		unsigned char flags = 0;
		for (int bit = 0; bit < 8 && i < n; bit++) {
			int bl = 0, bd = 0;
			if (i + 3 <= n) {
				int j = head[HASH(i)], tries = 0;
				int maxl = n - i < 18 ? n - i : 18;
				while (j >= 0 && i - j <= 4096 && tries++ < 256) {
					int l = 0;
					while (l < maxl && d[j + l] == d[i + l]) l++;
					if (l > bl) { bl = l; bd = i - j; if (l == maxl) break; }
					j = prev[j];
				}
			}
			if (bl >= 3) {
				flags |= 0x80 >> bit;
				int dd = bd - 1;
				o[op++] = ((bl - 3) << 4) | (dd >> 8);
				o[op++] = dd & 255;
				for (int k = 0; k < bl; k++) { INSERT(i); i++; }
			} else {
				o[op++] = d[i];
				INSERT(i);
				i++;
			}
		}
		o[fp] = flags;
	}
	while (op % 4) o[op++] = 0;
	f = fopen(argv[2], "wb");
	fwrite(o, 1, op, f);
	fclose(f);
	return 0;
}
