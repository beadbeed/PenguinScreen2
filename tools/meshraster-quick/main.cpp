// Scratch harness only (branch meshraster-quick): runs MeshRaster::SelfTest and writes a few pictures as PNG.
#include "VR/MeshRaster.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace VR::MeshRaster;

static unsigned int Crc(const unsigned char* data, size_t n, unsigned int crc = 0xFFFFFFFFu)
{
	static unsigned int table[256];
	static bool ready = false;
	if (!ready)
	{
		for (unsigned int i = 0; i < 256; i++)
		{
			unsigned int c = i;
			for (int k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		ready = true;
	}
	for (size_t i = 0; i < n; i++)
		crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
	return crc;
}

static void Chunk(FILE* f, const char* type, const std::vector<unsigned char>& body)
{
	const unsigned int len = static_cast<unsigned int>(body.size());
	const unsigned char l[4] = {static_cast<unsigned char>(len >> 24), static_cast<unsigned char>(len >> 16), static_cast<unsigned char>(len >> 8), static_cast<unsigned char>(len)};
	std::fwrite(l, 1, 4, f);
	std::fwrite(type, 1, 4, f);
	if (!body.empty())
		std::fwrite(body.data(), 1, body.size(), f);
	unsigned int crc = Crc(reinterpret_cast<const unsigned char*>(type), 4);
	crc = Crc(body.data(), body.size(), crc) ^ 0xFFFFFFFFu;
	const unsigned char c[4] = {static_cast<unsigned char>(crc >> 24), static_cast<unsigned char>(crc >> 16), static_cast<unsigned char>(crc >> 8), static_cast<unsigned char>(crc)};
	std::fwrite(c, 1, 4, f);
}

static void WritePng(const char* path, int w, int h, const std::vector<unsigned char>& rgb)
{
	FILE* f = std::fopen(path, "wb");
	if (!f)
		return;
	const unsigned char sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	std::fwrite(sig, 1, 8, f);
	std::vector<unsigned char> ihdr = {static_cast<unsigned char>(w >> 24), static_cast<unsigned char>(w >> 16), static_cast<unsigned char>(w >> 8), static_cast<unsigned char>(w),
		static_cast<unsigned char>(h >> 24), static_cast<unsigned char>(h >> 16), static_cast<unsigned char>(h >> 8), static_cast<unsigned char>(h), 8, 2, 0, 0, 0};
	Chunk(f, "IHDR", ihdr);
	std::vector<unsigned char> raw;
	for (int y = 0; y < h; y++)
	{
		raw.push_back(0);
		raw.insert(raw.end(), rgb.begin() + static_cast<size_t>(y) * w * 3, rgb.begin() + static_cast<size_t>(y + 1) * w * 3);
	}
	std::vector<unsigned char> z = {0x78, 0x01};
	unsigned int a = 1, b = 0;
	for (const unsigned char v : raw)
	{
		a = (a + v) % 65521;
		b = (b + a) % 65521;
	}
	for (size_t pos = 0; pos < raw.size(); pos += 65535)
	{
		const size_t n = std::min<size_t>(65535, raw.size() - pos);
		z.push_back(pos + n >= raw.size() ? 1 : 0);
		z.push_back(static_cast<unsigned char>(n & 0xFF));
		z.push_back(static_cast<unsigned char>(n >> 8));
		z.push_back(static_cast<unsigned char>(~n & 0xFF));
		z.push_back(static_cast<unsigned char>((~n >> 8) & 0xFF));
		z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
	}
	const unsigned int adler = (b << 16) | a;
	z.push_back(static_cast<unsigned char>(adler >> 24));
	z.push_back(static_cast<unsigned char>(adler >> 16));
	z.push_back(static_cast<unsigned char>(adler >> 8));
	z.push_back(static_cast<unsigned char>(adler));
	Chunk(f, "IDAT", z);
	Chunk(f, "IEND", {});
	std::fclose(f);
}

// The target composited (premultiplied) over a grey checkerboard, side by side in one strip.
static void Blit(std::vector<unsigned char>& strip, int strip_w, int x_offset, const Target& t)
{
	for (unsigned int y = 0; y < t.Height(); y++)
	{
		for (unsigned int x = 0; x < t.Width(); x++)
		{
			const unsigned int p = t.Pixel(x, y);
			const unsigned int a = p >> 24;
			const unsigned int bg = (((x >> 4) + (y >> 4)) & 1) ? 90 : 120;
			unsigned char* out = &strip[(static_cast<size_t>(y) * strip_w + x_offset + x) * 3];
			const unsigned int c[3] = {p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF};
			for (int k = 0; k < 3; k++)
				out[k] = static_cast<unsigned char>(std::min(255u, c[k] + (bg * (255 - a)) / 255));
		}
	}
}

struct Sphere
{
	std::vector<float> positions;
	std::vector<float> uvs;
	std::vector<unsigned short> indices;
};

static Sphere MakeSphere(int stacks, int slices)
{
	Sphere s;
	const float pi = 3.14159265f;
	for (int i = 0; i <= stacks; i++)
	{
		for (int j = 0; j <= slices; j++)
		{
			const float theta = pi * i / stacks;
			const float phi = 2.0f * pi * j / slices;
			s.positions.push_back(std::sin(theta) * std::cos(phi));
			s.positions.push_back(std::cos(theta));
			s.positions.push_back(std::sin(theta) * std::sin(phi));
			s.uvs.push_back(static_cast<float>(j) / slices);
			s.uvs.push_back(static_cast<float>(i) / stacks);
		}
	}
	for (int i = 0; i < stacks; i++)
	{
		for (int j = 0; j < slices; j++)
		{
			const unsigned short v00 = static_cast<unsigned short>(i * (slices + 1) + j);
			const unsigned short v01 = static_cast<unsigned short>(v00 + 1);
			const unsigned short v10 = static_cast<unsigned short>(v00 + slices + 1);
			const unsigned short v11 = static_cast<unsigned short>(v10 + 1);
			s.indices.insert(s.indices.end(), {v00, v01, v10, v10, v01, v11});
		}
	}
	return s;
}


static double TimeMs(Target& t, const Camera& cam, const Mesh& mesh, const Mat4& model, const Material& mat, int runs, DrawStats* out_stats)
{
	double total = 0.0;
	for (int i = 0; i < runs + 3; i++)
	{
		const auto begin = std::chrono::steady_clock::now();
		t.Clear();
		Draw(t, cam, mesh, model, mat, out_stats);
		t.Resolve();
		const auto end = std::chrono::steady_clock::now();
		if (i >= 3)
			total += std::chrono::duration<double, std::milli>(end - begin).count();
	}
	return total / runs;
}

static void Bench(const Mesh& mesh, const Texture& nearest_tex, const Texture& bilinear_tex)
{
	struct Case
	{
		const char* name;
		bool lit;
		bool rim;
		const Texture* tex;
		bool cull;
	};
	const Case cases[] = {
		{"flat unlit", false, false, nullptr, true},
		{"lit", true, false, nullptr, true},
		{"lit+rim", true, true, nullptr, true},
		{"nearest unlit", false, false, &nearest_tex, true},
		{"nearest lit", true, false, &nearest_tex, true},
		{"nearest lit+rim", true, true, &nearest_tex, true},
		{"bilinear lit", true, false, &bilinear_tex, true},
		{"bilinear lit+rim", true, true, &bilinear_tex, true},
		{"lit, no culling", true, false, nullptr, false},
	};
	const float scales[2] = {0.9f, 0.45f};
	for (const float sc : scales)
	{
		const float pos[3] = {0.0f, 0.0f, -2.4f};
		const float rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};
		const float scale[3] = {sc, sc, sc};
		const Mat4 model = ModelMatrix(pos, rot, scale);
		const Camera cam = Camera::Perspective(Mat4(), 60.0f * 3.14159265f / 180.0f, 1.0f, 0.03f, 20.0f);
		for (const Case& c : cases)
		{
			Material m;
			m.texture = c.tex;
			m.lighting.enabled = c.lit;
			m.lighting.rim = c.rim ? 0.4f : 0.0f;
			m.cull_back = c.cull;
			Target t(256, 256, 1);
			DrawStats st;
			const double ms1 = TimeMs(t, cam, mesh, model, m, 200, &st);
			Target t2(256, 256, 2);
			const double ms2 = TimeMs(t2, cam, mesh, model, m, 100, nullptr);
			std::printf("bench radius %.2f: %-18s 1x1 %.3f ms (%llu samples, %.1f ns each)   2x2 %.3f ms
", sc, c.name, ms1, static_cast<unsigned long long>(st.written),
				st.written ? ms1 * 1.0e6 / static_cast<double>(st.written) : 0.0, ms2);
		}
	}
	std::fflush(stdout);
}

int main()
{
	const int failed = SelfTest();

	// A textured, lit sphere at each sample grid.
	const Sphere sphere = MakeSphere(21, 24);
	unsigned int texels[64 * 64];
	for (unsigned int y = 0; y < 64; y++)
	{
		for (unsigned int x = 0; x < 64; x++)
			texels[y * 64 + x] = (((x >> 3) + (y >> 3)) & 1u) ? PackRgba(220, 170, 110) : PackRgba(80, 60, 50);
	}
	Texture tex;
	tex.texels = texels;
	tex.width = 64;
	tex.height = 64;
	tex.filter = Texture::Filter::Bilinear;
	Material mat;
	mat.texture = &tex;
	mat.lighting.light_in_view_space = true;
	mat.lighting.rim = 0.5f;
	mat.lighting.rim_colour[0] = 0.5f;
	mat.lighting.rim_colour[1] = 0.7f;
	mat.lighting.headlight = 0.15f;
	Mesh mesh;
	mesh.positions = sphere.positions.data();
	mesh.normals = sphere.positions.data();
	mesh.uvs = sphere.uvs.data();
	mesh.vertex_count = static_cast<unsigned int>(sphere.positions.size() / 3);
	mesh.indices16 = sphere.indices.data();
	mesh.index_count = static_cast<unsigned int>(sphere.indices.size());

	const float pos[3] = {0.1f, -0.05f, -2.2f};
	const float rot[4] = {0.3f, 0.5f, 0.1f, 0.8f};
	const float scale[3] = {0.9f, 0.9f, 0.9f};
	const Mat4 model = ModelMatrix(pos, rot, scale);
	const Camera cam = Camera::Perspective(Mat4(), 60.0f * 3.14159265f / 180.0f, 1.0f, 0.03f, 20.0f);

	Texture nearest_tex = tex;
	nearest_tex.filter = Texture::Filter::Nearest;
	Bench(mesh, nearest_tex, tex);

	std::vector<unsigned char> strip(static_cast<size_t>(768) * 256 * 3);
	const unsigned int grids[3] = {1, 2, 4};
	for (int k = 0; k < 3; k++)
	{
		Target t(256, 256, grids[k]);
		t.Clear();
		Draw(t, cam, mesh, model, mat);
		t.Resolve();
		Blit(strip, 768, k * 256, t);
	}
	WritePng("sphere_1x_2x_4x.png", 768, 256, strip);

	// A floor of checkers reaching from far ahead to behind the camera (cut by the near plane), seen from low down, with
	// the sphere on it.
	{
		unsigned int check[16 * 16];
		for (unsigned int y = 0; y < 16; y++)
		{
			for (unsigned int x = 0; x < 16; x++)
				check[y * 16 + x] = (((x >> 1) + (y >> 1)) & 1u) ? PackRgba(230, 230, 230) : PackRgba(40, 60, 120);
		}
		Texture floor_tex;
		floor_tex.texels = check;
		floor_tex.width = 16;
		floor_tex.height = 16;
		floor_tex.filter = Texture::Filter::Nearest;
		Material floor_mat;
		floor_mat.texture = &floor_tex;
		floor_mat.cull_back = false;
		floor_mat.lighting.light_dir[0] = 0.2f;
		floor_mat.lighting.light_dir[1] = 1.0f;
		floor_mat.lighting.light_dir[2] = 0.3f;
		const float p[4][3] = {{-30, -1, 40}, {30, -1, 40}, {30, -1, -60}, {-30, -1, -60}};
		const float uv[4][2] = {{0, 0}, {20, 0}, {20, 40}, {0, 40}};
		std::vector<float> positions, normals, uvs;
		for (int i = 0; i < 4; i++)
		{
			positions.insert(positions.end(), {p[i][0], p[i][1], p[i][2]});
			normals.insert(normals.end(), {0.0f, 1.0f, 0.0f});
			uvs.insert(uvs.end(), {uv[i][0], uv[i][1]});
		}
		const unsigned short idx[6] = {0, 1, 2, 0, 2, 3};
		Mesh floor_mesh;
		floor_mesh.positions = positions.data();
		floor_mesh.normals = normals.data();
		floor_mesh.uvs = uvs.data();
		floor_mesh.vertex_count = 4;
		floor_mesh.indices16 = idx;
		floor_mesh.index_count = 6;
		const float eye[3] = {0.0f, 0.2f, 0.0f};
		const float at[3] = {0.0f, 0.0f, -5.0f};
		const float up[3] = {0.0f, 1.0f, 0.0f};
		const Camera floor_cam = Camera::Perspective(LookAt(eye, at, up), 75.0f * 3.14159265f / 180.0f, 1.0f, 0.05f, 200.0f);
		std::vector<unsigned char> pic(static_cast<size_t>(512) * 256 * 3);
		for (int k = 0; k < 2; k++)
		{
			Target t(256, 256, k == 0 ? 1 : 2);
			t.Clear();
			DrawStats st;
			Draw(t, floor_cam, floor_mesh, Mat4(), floor_mat, &st);
			std::printf("harness: floor at %dx: %u triangles, %u clipped, %llu samples written\n", k == 0 ? 1 : 2, st.triangles, st.clipped,
				static_cast<unsigned long long>(st.written));
			Draw(t, floor_cam, mesh, model, mat, &st);
			t.Resolve();
			Blit(pic, 512, k * 256, t);
		}
		WritePng("floor_clipped.png", 512, 256, pic);
	}

	std::printf("harness: SelfTest returned %d\n", failed);
	return failed == 0 ? 0 : 1;
}
