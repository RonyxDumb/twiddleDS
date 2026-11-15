// Minimal headless melonDS frontend used to test Twiddlefinger DS.
// usage: melonheadless rom.nds outdir "script"
//   script steps (comma separated):
//     wN        run N frames
//     sNAME     screenshot both screens -> outdir/NAME.ppm
//     pKEYN     hold KEY for N frames (A,B,X,Y,L,R,START,SELECT,UP,DOWN,LEFT,RIGHT)
//     tX:Y:N    touch at X,Y for N frames
//     aNAME     start/stop dumping audio to outdir/NAME.raw (s16 stereo 32768Hz)
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <memory>
#include "NDS.h"
#include "NDSCart.h"
#include "GPU.h"
#include "GPU_Soft.h"
#include "SPU.h"
#include "Args.h"

using namespace melonDS;

static FILE *audioOut = nullptr;

static void save_ppm(const std::string &path, u32 *top, u32 *bot) {
	FILE *f = fopen(path.c_str(), "wb");
	fprintf(f, "P6\n256 384\n255\n");
	for (int s = 0; s < 2; s++) {
		u32 *fb = s ? bot : top;
		for (int i = 0; i < 256 * 192; i++) {
			u32 c = fb[i];
			unsigned char rgb[3] = { (unsigned char)((c >> 16) & 255), (unsigned char)((c >> 8) & 255), (unsigned char)(c & 255) };
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

static u32 keybit(const std::string &k) {
	static const char *names[] = { "A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y" };
	for (int i = 0; i < 12; i++)
		if (k == names[i]) return 1u << i;
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 4) { fprintf(stderr, "usage\n"); return 1; }
	std::string out = argv[2];
	std::ifstream rf(argv[1], std::ios::binary);
	std::vector<char> rom((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
	auto data = std::make_unique<u8[]>(rom.size());
	memcpy(data.get(), rom.data(), rom.size());

	NDSArgs args;
	args.JIT = std::nullopt;
	auto nds = std::make_unique<NDS>(std::move(args), nullptr);
	nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
	auto cart = NDSCart::ParseROM(std::move(data), (u32)rom.size(), nullptr, std::nullopt);
	if (!cart) { fprintf(stderr, "bad rom\n"); return 1; }
	nds->SetNDSCart(std::move(cart));
	nds->Reset();
	nds->SetupDirectBoot("game.nds");
	nds->Start();

	u32 keys = 0xFFF;
	std::vector<s16> abuf(4096 * 2);
	auto frame = [&](int n) {
		for (int i = 0; i < n; i++) {
			nds->SetKeyMask(keys);
			nds->RunFrame();
			int avail = nds->SPU.GetOutputSize();
			while (avail > 0) {
				int got = nds->SPU.ReadOutput(abuf.data(), avail > 4096 ? 4096 : avail);
				if (got <= 0) break;
				if (audioOut) fwrite(abuf.data(), 4, got, audioOut);
				avail -= got;
			}
		}
	};

	std::stringstream ss(argv[3]);
	std::string step;
	while (std::getline(ss, step, ',')) {
		if (step.empty()) continue;
		char c = step[0];
		std::string rest = step.substr(1);
		if (c == 'w') frame(atoi(rest.c_str()));
		else if (c == 's') {
			void *top, *bot;
			nds->GPU.GetFramebuffers(&top, &bot);
			save_ppm(out + "/" + rest + ".ppm", (u32 *)top, (u32 *)bot);
		} else if (c == 'p') {
			std::string name, num;
			for (char ch : rest) (isalpha(ch) ? name : num) += ch;
			u32 b = keybit(name);
			keys &= ~b;
			frame(num.empty() ? 2 : atoi(num.c_str()));
			keys |= b;
			frame(1);
		} else if (c == 't') {
			int x, y, n;
			sscanf(rest.c_str(), "%d:%d:%d", &x, &y, &n);
			nds->TouchScreen(x, y);
			frame(n);
			nds->ReleaseScreen();
			frame(1);
		} else if (c == 'a') {
			if (audioOut) { fclose(audioOut); audioOut = nullptr; }
			else audioOut = fopen((out + "/" + rest + ".raw").c_str(), "wb");
		}
	}
	if (audioOut) fclose(audioOut);
	printf("done\n");
	return 0;
}
