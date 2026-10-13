// SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
// SPDX-License-Identifier: GPL-3.0
//
// Reads Resident Evil Outbreak's weapon models from the player's disc image and decodes them in memory.
//
// The format knowledge and several routines are ported from two GPL-3.0 projects by Fothsid, with thanks:
// - libthanatos, https://github.com/Fothsid/libthanatos : the SLD (LZSS) texture decompressor (src/OBSld.cpp), the NBD
//   header (src/OBNbd.cpp), the AMO chunk tree and its lists (src/OBModel.cpp, OBMesh.cpp, OBPrimLists.cpp,
//   OBTStripList.cpp, OBMaterial.cpp, OBTextureList.cpp, OBWeightList.cpp, include/thanatos/OBTypes.h) and the AHI
//   hierarchy and node layout (src/OBHierarchy.cpp, OBNode.cpp).
// - OBTool, https://github.com/Fothsid/OBTool : the TIM2 picture header and the 8-bit CLUT order (src/tim2.c, tim2.h),
//   the strip unrolling with alternating winding (src/convert_to_fbx.cpp) and the AFS table (src/afs.c).
// Rewritten here to check every offset, count and size against its buffer: a bad or unexpected disc must leave an
// item out, never crash, assert or stall the emulator. The layout notes are in
// tools/outbreak-vr/research/2026-10-12-weapon-models-research.md; tools/outbreak-vr/gamemodels_check.py is the same
// decoder in Python, to compare on the same disc.

#include "VR/GameModels.h"

#include "CDVD/IsoFileFormats.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/StringUtil.h"
#include "common/Threading.h"
#include "common/Timer.h"

#include "fmt/format.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace VR::GameModels
{
	namespace
	{
		constexpr const char* kFile1Serial = "SLPM-65428";
		constexpr const char* kFile2Serial = "SLPM-65692";

		constexpr u32 kSector = 2048;
		// PCSX2's image readers hand a sector's 2048 bytes of user data back 24 bytes into the raw frame, whatever the
		// image's block size (InputIsoFile::FinishRead3 with CDVD_MODE_2048 reads from there too).
		constexpr u32 kUserDataOffset = 24;

		// Bounds for a malformed or unexpected disc. The two discs checked need 4.0 MB (File #1) and 10.9 MB (File #2)
		// of reads and decode to under 5 MB.
		constexpr u64 kMaxReadBytes = 64ull << 20;
		constexpr u32 kMaxNbdBytes = 16u << 20;
		constexpr size_t kMaxTim2Bytes = 4u << 20; // one decompressed texture
		constexpr u32 kMaxTextureSide = 1024;
		constexpr size_t kMaxLibraryBytes = 48u << 20;
		constexpr u32 kMaxAfsEntries = 8192;
		constexpr u32 kMaxRootDirBytes = 64 * kSector;
		constexpr u32 kMaxListCount = 1u << 16;
		constexpr u32 kMaxVertices = 65535; // u16 indices
		constexpr u32 kMaxWeightsPerVertex = 16;
		constexpr u32 kMaxSlots = 16;
		constexpr u32 kMaxChildHops = 16;
		constexpr size_t kMaxErrorLines = 8;

		// NBD header entries: {u32 fourcc, u32 offset, u32 size, u32 count}, four of them in an OBJS file.
		constexpr u32 kFourccTex = 0x584554; // "TEX"
		constexpr u32 kFourccAmo = 0x4F4D41; // "AMO"
		constexpr u32 kFourccAhi = 0x494841; // "AHI"
		constexpr size_t kNbdHeaderBytes = 0x40;

		// AMO and AHI chunk types (libthanatos OBTypes.h). Every chunk starts {u32 type, u32 count, u32 size}, the size
		// counting that 12-byte header.
		constexpr u32 kChunkTexRef = 0x0;
		constexpr u32 kChunkModel = 0x1;
		constexpr u32 kChunkMeshList = 0x2;
		constexpr u32 kChunkMesh = 0x4;
		constexpr u32 kChunkPrimLists = 0x5;
		constexpr u32 kChunkMaterialList = 0x9;
		constexpr u32 kChunkTextureList = 0xA;
		constexpr u32 kChunkStripsFW = 0x30000;
		constexpr u32 kChunkStripsPW = 0x40000;
		constexpr u32 kChunkMaterialRefs = 0x50000;
		constexpr u32 kChunkStripMaterials = 0x60000;
		constexpr u32 kChunkVertices = 0x70000;
		constexpr u32 kChunkNormals = 0x80000;
		constexpr u32 kChunkTexCoords = 0xA0000;
		constexpr u32 kChunkWeights = 0xC0000;
		constexpr u32 kChunkJointRefs = 0x100000;
		constexpr u32 kChunkMaterialConstant = 0x120000;
		constexpr u32 kChunkMaterialLambert = 0x130000;
		constexpr u32 kChunkMaterialPhong = 0x140000;
		constexpr u32 kChunkMaterialVertexColour = 0x150000;
		constexpr u32 kChunkHierarchy = 0xC0000000u;
		constexpr u32 kChunkRootList = 0x80000000u; // libthanatos writes it with the top bit cleared; the discs set it
		constexpr u32 kChunkNodePlain = 0x40000001u;
		constexpr u32 kChunkNodeMesh = 0x40000002u;
		constexpr u32 kChunkNodeJoint = 0x40000003u;

		// A material: ambient, diffuse, specular (float4 each), specular decay, texture count, 50 pad words, then the
		// texture count's indices into the texture list.
		constexpr size_t kMaterialBodyBytes = 3 * 16 + 4 + 4 + 50 * 4;
		constexpr size_t kMaterialTexCountAt = 3 * 16 + 4;
		// A texture reference: its chunk header, id (into the NBD's TEX list), width, height, tiling, 60 pad words.
		constexpr size_t kTexRefBytes = 12 + 4 * 4 + 60 * 4;
		// A node: its chunk header, id, parent, child, next sibling, scale[4], rotation[4], translation[4], mesh id,
		// group id, 46 pad words.
		constexpr size_t kNodeBytes = 12 + 4 * 4 + 12 * 4 + 2 * 4 + 46 * 4;

		u16 Get16(const u8* p)
		{
			u16 v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}

		u32 Get32(const u8* p)
		{
			u32 v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}

		s32 GetS32(const u8* p)
		{
			s32 v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}

		float GetF32(const u8* p)
		{
			float v;
			std::memcpy(&v, p, sizeof(v));
			return v;
		}

		// [off, off + len) lies inside a buffer of size bytes, without overflowing.
		bool In(size_t size, size_t off, size_t len)
		{
			return off <= size && len <= size - off;
		}

		bool IsPow2(u32 v)
		{
			return v != 0 && (v & (v - 1)) == 0;
		}

		struct Chunk
		{
			u32 type = 0;
			u32 count = 0;
			size_t off = 0;  // of the header
			size_t size = 0; // the header included
			size_t End() const { return off + size; }
		};

		// The chunk whose header is at off inside a parent that ends at end (which the caller has checked against the
		// buffer): false unless the header and the size it gives fit the parent.
		bool ReadChunk(const u8* data, size_t off, size_t end, Chunk* out)
		{
			if (off > end || end - off < 12)
				return false;
			const u32 size = Get32(data + off + 8);
			if (size < 12 || size > end - off)
				return false;
			out->type = Get32(data + off);
			out->count = Get32(data + off + 4);
			out->off = off;
			out->size = size;
			return true;
		}

		// Everything decoded from one disc. Immutable once published: Find hands out pointers into it.
		struct Library
		{
			std::vector<Model> models; // sorted by item id once the load ends
			std::vector<std::unique_ptr<Texture>> textures;
			size_t bytes = 0;
			u32 files = 0;  // OBJS files that parsed
			bool file1 = false;

			bool Has(u16 id) const
			{
				for (const Model& m : models)
				{
					if (m.item_id == id)
						return true;
				}
				return false;
			}
		};

		// --- Disc access ----------------------------------------------------------------------------------------------

		// A reader of its own on the image the VM booted, so the emulated CDVD's reader is never shared or moved.
		class DiscReader
		{
		public:
			explicit DiscReader(const std::atomic<bool>& cancel)
				: m_cancel(cancel)
			{
			}

			bool Open(const std::string& path, Error* error) { return m_iso.Open(path, error); }
			u64 BytesRead() const { return m_bytes_read; }
			bool BudgetSpent() const { return m_budget_spent; }

			// count sectors from lsn, their user data only, into out (count * 2048 bytes).
			const char* ReadSectors(u32 lsn, u32 count, u8* out)
			{
				const u32 blocks = m_iso.GetBlockCount();
				if (count > blocks || lsn > blocks - count)
					return "a read past the end of the disc";
				if (m_bytes_read + u64{count} * kSector > kMaxReadBytes)
				{
					m_budget_spent = true;
					return "the 64 MB read budget is spent";
				}
				for (u32 i = 0; i < count; i++)
				{
					if (m_cancel.load(std::memory_order_relaxed))
						return "cancelled";
					if (m_iso.ReadSync(m_frame, lsn + i) < static_cast<int>(kSector))
						return "a sector read failed";
					std::memcpy(out + size_t{i} * kSector, m_frame + kUserDataOffset, kSector);
				}
				m_bytes_read += u64{count} * kSector;
				return nullptr;
			}

			// size bytes from byte offset off (counted from the start of the disc) into out.
			const char* ReadBytes(u64 off, u32 size, std::vector<u8>* out)
			{
				out->resize(size);
				u32 done = 0;
				while (done < size)
				{
					const u64 at = off + done;
					if (at / kSector > 0xFFFFFFFFull)
						return "a read past the end of the disc";
					const u32 in_sector = static_cast<u32>(at % kSector);
					const u32 n = std::min(kSector - in_sector, size - done);
					if (const char* why = ReadSectors(static_cast<u32>(at / kSector), 1, m_sector))
						return why;
					std::memcpy(out->data() + done, m_sector + in_sector, n);
					done += n;
				}
				return nullptr;
			}

		private:
			const std::atomic<bool>& m_cancel;
			InputIsoFile m_iso;
			u64 m_bytes_read = 0;
			bool m_budget_spent = false;
			// InputIsoFile::ReadSync writes a whole raw block (up to CD_FRAMESIZE_RAW) at its block offset.
			u8 m_frame[CD_FRAMESIZE_RAW + 8] = {};
			u8 m_sector[kSector] = {};
		};

		// The ISO9660 root directory's file name (case does not matter, ";1" ignored).
		const char* FindRootFile(DiscReader& disc, std::string_view name, u32* lsn, u32* size)
		{
			u8 pvd[kSector];
			if (const char* why = disc.ReadSectors(16, 1, pvd))
				return why;
			if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
				return "no ISO9660 volume descriptor";
			const u8* root = pvd + 156;
			const u32 dir_lsn = Get32(root + 2);
			const u32 dir_size = Get32(root + 10);
			if (dir_size == 0 || dir_size > kMaxRootDirBytes)
				return "an unexpected root directory size";
			const u32 sectors = (dir_size + kSector - 1) / kSector;
			std::vector<u8> dir(size_t{sectors} * kSector);
			if (const char* why = disc.ReadSectors(dir_lsn, sectors, dir.data()))
				return why;
			size_t pos = 0;
			while (pos < dir_size)
			{
				const u8 record = dir[pos];
				// Records never cross a sector: a zero length pads to the next one.
				if (record == 0)
				{
					pos = (pos / kSector + 1) * kSector;
					continue;
				}
				if (record < 34 || pos + record > dir.size())
					return "a bad root directory record";
				const u8 name_length = dir[pos + 32];
				if (33u + name_length > record)
					return "a bad root directory record";
				std::string_view ident(reinterpret_cast<const char*>(&dir[pos + 33]), name_length);
				if (const size_t semi = ident.find(';'); semi != std::string_view::npos)
					ident = ident.substr(0, semi);
				if (!(dir[pos + 25] & 2) && StringUtil::compareNoCase(ident, name))
				{
					*lsn = Get32(&dir[pos + 2]);
					*size = Get32(&dir[pos + 10]);
					return nullptr;
				}
				pos += record;
			}
			return "the file is not in the root directory";
		}

		struct AfsEntry
		{
			char name[33] = {}; // "" when the archive has no usable name table
			u32 lsn = 0;
			u32 size = 0; // 0: no usable file here
		};

		// An AFS archive at base_lsn, total_size bytes long: "AFS\0", u32 count, count x (u32 offset, u32 size) from the
		// archive's start, then the name table's (offset, size); its 48-byte records start with a 32-byte name.
		const char* ReadAfs(DiscReader& disc, u32 base_lsn, u32 total_size, std::vector<AfsEntry>* out)
		{
			const u64 base = u64{base_lsn} * kSector;
			std::vector<u8> head;
			if (const char* why = disc.ReadBytes(base, 8, &head))
				return why;
			if (std::memcmp(head.data(), "AFS\0", 4) != 0)
				return "not an AFS archive";
			const u32 count = Get32(head.data() + 4);
			if (count == 0 || count > kMaxAfsEntries)
				return "an unexpected AFS entry count";
			const u32 table_bytes = 8 + count * 8 + 8;
			if (table_bytes > total_size)
				return "an AFS table past the archive";
			std::vector<u8> table;
			if (const char* why = disc.ReadBytes(base, table_bytes, &table))
				return why;

			out->assign(count, AfsEntry{});
			u32 first = 0xFFFFFFFFu;
			for (u32 i = 0; i < count; i++)
			{
				const u32 off = Get32(&table[8 + size_t{i} * 8]);
				const u32 size = Get32(&table[12 + size_t{i} * 8]);
				// One bad entry makes only that entry unusable (File #2's r010.afs has a size of 0xFFFFF801).
				if (size == 0 || off % kSector != 0 || off < table_bytes - 8 || off > total_size || size > total_size - off)
					continue;
				(*out)[i].lsn = base_lsn + off / kSector;
				(*out)[i].size = size;
				first = std::min(first, off);
			}

			// The name table's pointer follows the entries, or in some archives sits just before the first file.
			const auto usable = [count, total_size](u32 off, u32 size) {
				return off != 0 && off % 4 == 0 && size >= count * 48u && off <= total_size && count * 48u <= total_size - off;
			};
			u32 names_off = Get32(&table[8 + size_t{count} * 8]);
			u32 names_size = Get32(&table[12 + size_t{count} * 8]);
			if (!usable(names_off, names_size) && first != 0xFFFFFFFFu && first >= 8)
			{
				std::vector<u8> pointer;
				if (!disc.ReadBytes(base + first - 8, 8, &pointer))
				{
					names_off = Get32(pointer.data());
					names_size = Get32(pointer.data() + 4);
				}
			}
			if (usable(names_off, names_size))
			{
				std::vector<u8> names;
				if (const char* why = disc.ReadBytes(base + names_off, count * 48, &names))
					return why;
				for (u32 i = 0; i < count; i++)
					std::memcpy((*out)[i].name, &names[size_t{i} * 48], 32);
			}
			return nullptr;
		}

		bool IsHexDigit(char c)
		{
			return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		}

		bool IsDigit(char c)
		{
			return c >= '0' && c <= '9';
		}

		// rXXX.afs: one scenario's archive (rfff.afs, the lobby's, has no OBJS file but costs only its table).
		bool IsScenarioArchive(const char* name)
		{
			const std::string_view n(name);
			return n.size() == 8 && (n[0] == 'r' || n[0] == 'R') && IsHexDigit(n[1]) && IsHexDigit(n[2]) &&
			       IsHexDigit(n[3]) && StringUtil::compareNoCase(n.substr(4), ".afs");
		}

		// OBJSxx.NBD: the scenario's gameplay items, the weapons among them.
		bool IsObjsFile(const char* name)
		{
			const std::string_view n(name);
			return n.size() == 10 && StringUtil::compareNoCase(n.substr(0, 4), "OBJS") && IsDigit(n[4]) &&
			       IsDigit(n[5]) && StringUtil::compareNoCase(n.substr(6), ".NBD");
		}

		// --- Textures ---------------------------------------------------------------------------------------------------

		// libthanatos OBSld::decompress, bounded. 16-bit words: a flag word read MSB first; a 0 bit copies one literal
		// word, a 1 bit reads a code: count = code >> 11 (0: the next word is the count), offset = code & 0x7FF words
		// back (0: a run of zero words). False on output past out_cap bytes or a reference before the start.
		bool DecompressSld(const u8* src, size_t src_size, size_t out_cap, std::vector<u8>* out)
		{
			const size_t n = src_size / 2;
			const size_t cap = out_cap / 2;
			std::vector<u16> words;
			words.reserve(std::min<size_t>(cap, 1u << 16));
			size_t sp = 0;
			u32 bits = 0;
			u16 flag = 0;
			while (sp < n)
			{
				if (bits == 0)
				{
					flag = Get16(src + sp * 2);
					sp++;
					bits = 16;
					if (sp >= n)
						break;
				}
				if (flag & 0x8000)
				{
					const u16 code = Get16(src + sp * 2);
					sp++;
					u32 count = code >> 11;
					if (count == 0)
					{
						if (sp >= n)
							return false;
						count = Get16(src + sp * 2);
						sp++;
					}
					const u32 back = code & 0x7FF;
					if (count > cap - words.size())
						return false;
					if (back == 0)
					{
						words.insert(words.end(), count, 0);
					}
					else
					{
						if (back > words.size())
							return false;
						// The source may overlap what this copy writes: a short offset repeats a pattern.
						const size_t from = words.size() - back;
						for (u32 i = 0; i < count; i++)
						{
							const u16 w = words[from + i];
							words.push_back(w);
						}
					}
				}
				else
				{
					if (words.size() >= cap)
						return false;
					words.push_back(Get16(src + sp * 2));
					sp++;
				}
				flag = static_cast<u16>(flag << 1);
				bits--;
			}
			out->resize(words.size() * 2);
			if (!words.empty())
				std::memcpy(out->data(), words.data(), out->size());
			return true;
		}

		// The first picture of an Outbreak TIM2 (OBTool tim2.c): a 16-byte file header (byte 5: the picture header at
		// 0x10, or at 0x80 when 1, as on File #2), then {total, CLUT and image sizes, header size, CLUT colours,
		// format, mipmaps, CLUT type, image type, width, height, GS registers}. 4- and 8-bit indexed, low nibble first,
		// pixels linear; a 16-, 24- or 32-bit CLUT; an 8-bit CLUT in CSM1 order (type bit 7 clear) has entries 8-15
		// and 16-23 of every 32 swapped; alpha on the PS2's 0-0x80 scale is doubled.
		const char* DecodeTim2(const u8* raw, size_t size, Texture* out)
		{
			if (size < 16 || std::memcmp(raw, "TIM2", 4) != 0)
				return "a texture that is not TIM2";
			if (Get16(raw + 6) < 1)
				return "a TIM2 file without pictures";
			const size_t ph = (raw[5] == 1) ? 0x80 : 0x10;
			if (!In(size, ph, 48))
				return "a TIM2 picture header past the end";
			const u32 image_size = Get32(raw + ph + 8);
			const u16 header_size = Get16(raw + ph + 12);
			const u16 clut_colours = Get16(raw + ph + 14);
			const u8 mipmaps = raw[ph + 17];
			const u8 clut_type = raw[ph + 18];
			const u8 image_type = raw[ph + 19];
			const u16 width = Get16(raw + ph + 20);
			const u16 height = Get16(raw + ph + 22);
			if (image_type != 4 && image_type != 5)
				return "a TIM2 picture that is not 4- or 8-bit indexed";
			if (!IsPow2(width) || !IsPow2(height) || width > kMaxTextureSide || height > kMaxTextureSide)
				return "a TIM2 picture size that is not a power of two up to 1024";
			if (mipmaps < 1 || header_size < 48)
				return "a bad TIM2 picture header";
			const bool four_bit = (image_type == 4);
			const size_t pixel_bytes = four_bit ? size_t{width} * height / 2 : size_t{width} * height;
			const size_t image_at = ph + header_size;
			if (image_size < pixel_bytes || !In(size, image_at, pixel_bytes))
				return "TIM2 pixels past the end";
			u32 entry_bytes = 0;
			switch (clut_type & 0x3F)
			{
				case 1: entry_bytes = 2; break;
				case 2: entry_bytes = 3; break;
				case 3: entry_bytes = 4; break;
				default: return "an unknown TIM2 CLUT format";
			}
			const u32 colours = four_bit ? 16 : 256;
			const size_t clut_at = image_at + image_size;
			if (clut_colours < colours || !In(size, clut_at, size_t{colours} * entry_bytes))
				return "a TIM2 CLUT past the end";

			u8 palette[256][4];
			for (u32 i = 0; i < colours; i++)
			{
				const u8* e = raw + clut_at + size_t{i} * entry_bytes;
				if (entry_bytes == 2)
				{
					const u16 v = Get16(e);
					palette[i][0] = static_cast<u8>((v & 0x1F) << 3);
					palette[i][1] = static_cast<u8>(((v >> 5) & 0x1F) << 3);
					palette[i][2] = static_cast<u8>(((v >> 10) & 0x1F) << 3);
					palette[i][3] = (v & 0x8000) ? 0x80 : 0;
				}
				else
				{
					palette[i][0] = e[0];
					palette[i][1] = e[1];
					palette[i][2] = e[2];
					palette[i][3] = (entry_bytes == 4) ? e[3] : 0x80;
				}
			}
			if (!four_bit && !(clut_type & 0x80))
			{
				for (u32 base = 0; base < 256; base += 32)
				{
					for (u32 k = 0; k < 8; k++)
						std::swap_ranges(palette[base + 8 + k], palette[base + 8 + k] + 4, palette[base + 16 + k]);
				}
			}
			u8 max_alpha = 0;
			for (u32 i = 0; i < colours; i++)
				max_alpha = std::max(max_alpha, palette[i][3]);
			u32 packed[256];
			for (u32 i = 0; i < colours; i++)
			{
				const u32 a = (max_alpha <= 0x80) ? std::min<u32>(255, palette[i][3] * 2u) : palette[i][3];
				packed[i] = (a << 24) | (u32{palette[i][2]} << 16) | (u32{palette[i][1]} << 8) | palette[i][0];
			}

			out->width = width;
			out->height = height;
			out->rgba.resize(size_t{width} * height);
			const u8* px = raw + image_at;
			if (four_bit)
			{
				for (size_t i = 0; i < pixel_bytes; i++)
				{
					out->rgba[i * 2] = packed[px[i] & 0xF];
					out->rgba[i * 2 + 1] = packed[px[i] >> 4];
				}
			}
			else
			{
				for (size_t i = 0; i < pixel_bytes; i++)
					out->rgba[i] = packed[px[i]];
			}
			return nullptr;
		}

		// --- Meshes -----------------------------------------------------------------------------------------------------

		struct Material
		{
			bool textured = false;
			s32 texture_ref = -1; // its first texture: an index into the texture list
		};

		struct AmoIndex
		{
			std::vector<Chunk> meshes;
			std::vector<Material> materials;
			std::vector<s32> texture_ids; // per texture reference: the NBD TEX index
		};

		// AMO: Model -> {MeshList -> Mesh..., MaterialList -> Material..., TextureList -> TextureRef...}.
		const char* ParseAmo(const u8* d, size_t off, size_t size, AmoIndex* out)
		{
			Chunk model;
			if (!ReadChunk(d, off, off + size, &model) || model.type != kChunkModel)
				return "no AMO model chunk";
			size_t p = model.off + 12;
			for (u32 i = 0; i < std::min<u32>(model.count, 16) && p + 12 <= model.End(); i++)
			{
				Chunk c;
				if (!ReadChunk(d, p, model.End(), &c))
					return "a bad AMO chunk";
				if (c.count > kMaxListCount)
					return "an AMO list that is too long";
				if (c.type == kChunkMeshList)
				{
					size_t q = c.off + 12;
					for (u32 m = 0; m < c.count; m++)
					{
						Chunk mesh;
						if (!ReadChunk(d, q, c.End(), &mesh))
							return "a bad mesh chunk";
						out->meshes.push_back(mesh);
						q = mesh.End();
					}
				}
				else if (c.type == kChunkMaterialList)
				{
					size_t q = c.off + 12;
					for (u32 m = 0; m < c.count; m++)
					{
						Chunk mat;
						if (!ReadChunk(d, q, c.End(), &mat) ||
							(mat.type != kChunkMaterialConstant && mat.type != kChunkMaterialLambert &&
								mat.type != kChunkMaterialPhong && mat.type != kChunkMaterialVertexColour) ||
							mat.size < 12 + kMaterialBodyBytes)
						{
							return "a bad material";
						}
						const s32 textures = GetS32(d + mat.off + 12 + kMaterialTexCountAt);
						if (textures < 0 || textures > 16 || mat.size < 12 + kMaterialBodyBytes + size_t(textures) * 4)
							return "a bad material texture count";
						Material material;
						material.textured = (textures > 0);
						if (material.textured)
							material.texture_ref = GetS32(d + mat.off + 12 + kMaterialBodyBytes);
						out->materials.push_back(material);
						q = mat.End();
					}
				}
				else if (c.type == kChunkTextureList)
				{
					if (c.size < 12 + size_t{c.count} * kTexRefBytes)
						return "a bad texture list";
					for (u32 k = 0; k < c.count; k++)
					{
						const u8* r = d + c.off + 12 + size_t{k} * kTexRefBytes;
						if (Get32(r) != kChunkTexRef || Get32(r + 8) != kTexRefBytes)
							return "a bad texture reference";
						out->texture_ids.push_back(GetS32(r + 12));
					}
				}
				p = c.End();
			}
			return nullptr;
		}

		struct Node
		{
			s32 child = -1;
			s32 next = -1;
			s32 group = -1;
			float scale[3] = {1.0f, 1.0f, 1.0f};
			float rotation[3] = {0.0f, 0.0f, 0.0f};
			float translation[3] = {0.0f, 0.0f, 0.0f};
		};

		// AHI: Hierarchy (count = nodes + 1) -> RootList (int32 node ids), then the nodes.
		const char* ParseAhi(const u8* d, size_t off, size_t size, std::vector<s32>* roots, std::vector<Node>* nodes)
		{
			Chunk h;
			if (!ReadChunk(d, off, off + size, &h) || h.type != kChunkHierarchy || h.count < 1)
				return "no AHI hierarchy chunk";
			Chunk rl;
			if (!ReadChunk(d, h.off + 12, h.End(), &rl) || (rl.type != 0 && rl.type != kChunkRootList) ||
				rl.count > kMaxListCount || rl.size < 12 + size_t{rl.count} * 4)
			{
				return "a bad AHI root list";
			}
			const u32 node_count = h.count - 1;
			if (node_count > kMaxListCount)
				return "too many AHI nodes";
			size_t p = rl.End();
			for (u32 i = 0; i < node_count; i++)
			{
				Chunk n;
				if (!ReadChunk(d, p, h.End(), &n) ||
					(n.type != kChunkNodePlain && n.type != kChunkNodeMesh && n.type != kChunkNodeJoint) ||
					n.size != kNodeBytes)
				{
					return "a bad AHI node";
				}
				const u8* b = d + n.off + 12;
				Node node;
				node.child = GetS32(b + 8);
				node.next = GetS32(b + 12);
				for (int k = 0; k < 3; k++)
				{
					node.scale[k] = GetF32(b + 16 + 4 * k);
					node.rotation[k] = GetF32(b + 32 + 4 * k);
					node.translation[k] = GetF32(b + 48 + 4 * k);
				}
				node.group = GetS32(b + 68);
				// libthanatos OBNode::read: a joint stores its rotation negated and its scale as the reciprocal.
				if (n.type == kChunkNodeJoint)
				{
					for (int k = 0; k < 3; k++)
					{
						node.rotation[k] = -node.rotation[k];
						node.scale[k] = (node.scale[k] != 0.0f) ? 1.0f / node.scale[k] : 0.0f;
					}
				}
				nodes->push_back(node);
				p = n.End();
			}
			for (u32 i = 0; i < rl.count; i++)
			{
				const s32 r = GetS32(d + rl.off + 12 + size_t{i} * 4);
				if (r < 0 || static_cast<u32>(r) >= node_count)
					return "an AHI root that is not a node";
				roots->push_back(r);
			}
			return nullptr;
		}

		struct Strip
		{
			size_t indices = 0; // offset of the first u32 index
			u32 count = 0;
		};

		struct MeshData
		{
			std::vector<Strip> strips; // the FW list's, then the PW list's
			std::vector<s32> material_refs;
			std::vector<s32> strip_materials;
			std::vector<s32> joints;
			const u8* positions = nullptr;
			const u8* normals = nullptr;
			const u8* uvs = nullptr;
			u32 position_count = 0;
			u32 normal_count = 0;
			u32 uv_count = 0;
			std::vector<u32> slots; // per vertex: its heaviest weight's slot (empty: no weight list)
		};

		bool ReadIntList(const u8* d, const Chunk& c, std::vector<s32>* out)
		{
			if (c.count > kMaxListCount || c.size < 12 + size_t{c.count} * 4)
				return false;
			out->resize(c.count);
			for (u32 i = 0; i < c.count; i++)
				(*out)[i] = GetS32(d + c.off + 12 + size_t{i} * 4);
			return true;
		}

		const char* ParseMesh(const u8* d, const Chunk& mesh, MeshData* m)
		{
			if (mesh.type != kChunkMesh)
				return "not a mesh chunk";
			size_t p = mesh.off + 12;
			// The subchunk count can be larger than the chunk holds (File #1's OBJS files; libthanatos warns about it):
			// stop at the chunk's size.
			for (u32 i = 0; i < std::min<u32>(mesh.count, 64) && p + 12 <= mesh.End(); i++)
			{
				Chunk c;
				if (!ReadChunk(d, p, mesh.End(), &c))
					return "a bad mesh subchunk";
				switch (c.type)
				{
					case kChunkPrimLists:
					{
						if (c.count > 2)
							return "more than two strip lists";
						size_t q = c.off + 12;
						for (u32 l = 0; l < c.count; l++)
						{
							Chunk list;
							if (!ReadChunk(d, q, c.End(), &list) || (list.type != kChunkStripsFW && list.type != kChunkStripsPW) ||
								list.count > kMaxListCount)
							{
								return "a bad strip list";
							}
							size_t r = list.off + 12;
							for (u32 s = 0; s < list.count; s++)
							{
								if (!In(list.End(), r, 4))
									return "a strip past its list";
								// Bit 31 is a direction flag no weapon strip sets; the winding comes from the normals below.
								const u32 n = Get32(d + r) & 0x7FFFFFFFu;
								if (n > (list.End() - r - 4) / 4)
									return "a strip past its list";
								m->strips.push_back({r + 4, n});
								r += 4 + size_t{n} * 4;
							}
							q = list.End();
						}
						break;
					}
					case kChunkMaterialRefs:
						if (!ReadIntList(d, c, &m->material_refs))
							return "a bad material reference list";
						break;
					case kChunkStripMaterials:
						if (!ReadIntList(d, c, &m->strip_materials))
							return "a bad strip material list";
						break;
					case kChunkJointRefs:
						if (!ReadIntList(d, c, &m->joints))
							return "a bad joint reference list";
						break;
					case kChunkVertices:
					case kChunkNormals:
						if (c.count > kMaxVertices || c.size < 12 + size_t{c.count} * 12)
							return "a bad vertex list";
						(c.type == kChunkVertices ? m->positions : m->normals) = d + c.off + 12;
						(c.type == kChunkVertices ? m->position_count : m->normal_count) = c.count;
						break;
					case kChunkTexCoords:
						if (c.count > kMaxVertices || c.size < 12 + size_t{c.count} * 8)
							return "a bad texture coordinate list";
						m->uvs = d + c.off + 12;
						m->uv_count = c.count;
						break;
					case kChunkWeights:
					{
						if (c.count > kMaxVertices)
							return "a bad weight list";
						m->slots.resize(c.count);
						size_t q = c.off + 12;
						for (u32 v = 0; v < c.count; v++)
						{
							if (!In(c.End(), q, 4))
								return "weights past their list";
							const u32 n = Get32(d + q);
							if (n > kMaxWeightsPerVertex || !In(c.End(), q + 4, size_t{n} * 8))
								return "weights past their list";
							// Each weapon vertex has one weight of 100; take the heaviest should one have more.
							u32 slot = 0;
							float best = -1.0f;
							for (u32 k = 0; k < n; k++)
							{
								const float w = GetF32(d + q + 4 + size_t{k} * 8 + 4);
								if (w > best)
								{
									best = w;
									slot = Get32(d + q + 4 + size_t{k} * 8);
								}
							}
							m->slots[v] = slot;
							q += 4 + size_t{n} * 8;
						}
						break;
					}
					default:
						break; // colours, render attributes, the date: not needed
				}
				p = c.End();
			}
			return nullptr;
		}

		// One OBJS file's texture pages, each decoded the first time a part needs it.
		class TexturePages
		{
		public:
			TexturePages(const u8* data, Library& lib)
				: m_data(data)
				, m_lib(lib)
			{
			}

			// TEX: count x (u32 compressed size, data).
			const char* Index(size_t off, size_t size, u32 count)
			{
				if (count > 256)
					return "too many textures";
				size_t p = off;
				for (u32 i = 0; i < count; i++)
				{
					if (!In(off + size, p, 4))
						return "a texture entry past its block";
					const u32 csize = Get32(m_data + p);
					if (!In(off + size, p + 4, csize))
						return "a texture entry past its block";
					m_spans.push_back({p + 4, csize});
					p += 4 + size_t{csize};
				}
				m_decoded.assign(count, nullptr);
				return nullptr;
			}

			u32 Count() const { return static_cast<u32>(m_spans.size()); }

			const char* Get(u32 index, const Texture** out)
			{
				if (index >= m_spans.size())
					return "a texture id past the texture list";
				if (!m_decoded[index])
				{
					std::vector<u8> raw;
					if (!DecompressSld(m_data + m_spans[index].off, m_spans[index].size, kMaxTim2Bytes, &raw))
						return "a texture that does not decompress";
					auto tex = std::make_unique<Texture>();
					if (const char* why = DecodeTim2(raw.data(), raw.size(), tex.get()))
						return why;
					m_lib.bytes += tex->rgba.size() * sizeof(u32);
					m_decoded[index] = tex.get();
					m_lib.textures.push_back(std::move(tex));
				}
				*out = m_decoded[index];
				return nullptr;
			}

		private:
			struct Span
			{
				size_t off;
				u32 size;
			};

			const u8* m_data;
			Library& m_lib;
			std::vector<Span> m_spans;
			std::vector<const Texture*> m_decoded;
		};

		struct Vec3d
		{
			double x, y, z;
		};

		Vec3d Load3(const u8* p, u32 index)
		{
			return {GetF32(p + size_t{index} * 12), GetF32(p + size_t{index} * 12 + 4), GetF32(p + size_t{index} * 12 + 8)};
		}

		// Doubles, in this order, so the face test matches gamemodels_check.py bit for bit.
		Vec3d Sub(const Vec3d& a, const Vec3d& b)
		{
			return {a.x - b.x, a.y - b.y, a.z - b.z};
		}

		Vec3d Cross(const Vec3d& a, const Vec3d& b)
		{
			return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
		}

		double Dot(const Vec3d& a, const Vec3d& b)
		{
			return a.x * b.x + a.y * b.y + a.z * b.z;
		}

		// The item's model: strips unrolled into triangles (alternating winding, OBTool's convert_to_fbx), degenerate
		// ones dropped, each turned to agree with its vertex normals; split into parts by weight slot and texture page.
		// Every strip index indexes the position, normal, UV and weight lists at once.
		const char* BuildModel(const u8* d, u16 item_id, const MeshData& m, const AmoIndex& amo,
			const std::vector<Node>& nodes, s32 root, TexturePages& pages, Library& lib, Model* out)
		{
			const u32 nv = m.position_count;
			if (nv == 0 || !m.positions || !m.normals || !m.uvs || m.normal_count != nv || m.uv_count != nv)
				return "per-vertex lists that differ in length";
			if (!m.slots.empty() && m.slots.size() != nv)
				return "a weight list of another length";
			if (m.strip_materials.size() < m.strips.size())
				return "a strip material list that is too short";
			for (size_t i = 0; i < size_t{nv} * 3; i++)
			{
				if (!std::isfinite(GetF32(m.positions + i * 4)) || !std::isfinite(GetF32(m.normals + i * 4)))
					return "vertex data that is not finite";
			}
			for (size_t i = 0; i < size_t{nv} * 2; i++)
			{
				if (!std::isfinite(GetF32(m.uvs + i * 4)))
					return "texture coordinates that are not finite";
			}
			for (const u32 slot : m.slots)
			{
				if (slot >= kMaxSlots)
					return "a weight slot past 15";
			}

			struct Building
			{
				u32 slot = 0;
				s32 texture = -1;
				std::vector<s32> local; // original vertex -> this part's, -1 until first used
				Part part;
			};
			std::vector<Building> parts;

			u32 strip_triangles = 0;
			u32 degenerate = 0;
			u32 flipped = 0;
			for (size_t k = 0; k < m.strips.size(); k++)
			{
				const s32 mref = m.strip_materials[k];
				if (mref < 0 || static_cast<size_t>(mref) >= m.material_refs.size())
					return "a strip material past the material references";
				const s32 mat = m.material_refs[static_cast<size_t>(mref)];
				if (mat < 0 || static_cast<size_t>(mat) >= amo.materials.size())
					return "a material reference past the material list";
				s32 texture = -1;
				if (amo.materials[static_cast<size_t>(mat)].textured)
				{
					const s32 tref = amo.materials[static_cast<size_t>(mat)].texture_ref;
					if (tref < 0 || static_cast<size_t>(tref) >= amo.texture_ids.size())
						return "a material texture past the texture list";
					texture = amo.texture_ids[static_cast<size_t>(tref)];
					if (texture < 0 || static_cast<u32>(texture) >= pages.Count())
						return "a texture id past the NBD's textures";
				}

				const Strip& strip = m.strips[k];
				for (u32 t = 0; t + 2 < strip.count; t++)
				{
					strip_triangles++;
					const u8* ip = d + strip.indices + size_t{t} * 4;
					u32 a = Get32(ip);
					u32 b = Get32(ip + 4);
					u32 c = Get32(ip + 8);
					// Odd triangles of a strip run the other way round: swap their last two (OBTool's convert_to_fbx).
					if (t & 1)
						std::swap(b, c);
					if (a >= nv || b >= nv || c >= nv)
						return "a strip index past the vertex list";
					if (a == b || b == c || a == c)
					{
						degenerate++;
						continue;
					}
					const Vec3d pa = Load3(m.positions, a);
					const Vec3d face = Cross(Sub(Load3(m.positions, b), pa), Sub(Load3(m.positions, c), pa));
					if (Dot(face, face) <= 1e-12)
					{
						degenerate++;
						continue;
					}
					const Vec3d na = Load3(m.normals, a);
					const Vec3d nb = Load3(m.normals, b);
					const Vec3d nc = Load3(m.normals, c);
					// a + (b + c) sums the same whichever way b and c are turned, so CheckModel agrees bit for bit.
					const Vec3d normals = {na.x + (nb.x + nc.x), na.y + (nb.y + nc.y), na.z + (nb.z + nc.z)};
					if (Dot(face, normals) < 0.0)
					{
						std::swap(b, c);
						flipped++;
					}

					// A rigid weapon never splits a triangle between slots; should one, the majority (else the first
					// vertex) keeps it.
					const u32 sa = m.slots.empty() ? 0 : m.slots[a];
					const u32 sb = m.slots.empty() ? 0 : m.slots[b];
					const u32 sc = m.slots.empty() ? 0 : m.slots[c];
					const u32 slot = (sb == sc) ? sb : sa;

					Building* building = nullptr;
					for (Building& p : parts)
					{
						if (p.slot == slot && p.texture == texture)
						{
							building = &p;
							break;
						}
					}
					if (!building)
					{
						building = &parts.emplace_back();
						building->slot = slot;
						building->texture = texture;
						building->local.assign(nv, -1);
					}
					for (const u32 v : {a, b, c})
					{
						s32& local = building->local[v];
						if (local < 0)
						{
							local = static_cast<s32>(building->part.vertices.size());
							Vertex& vertex = building->part.vertices.emplace_back();
							for (int i = 0; i < 3; i++)
							{
								vertex.pos[i] = GetF32(m.positions + size_t{v} * 12 + i * 4);
								vertex.normal[i] = GetF32(m.normals + size_t{v} * 12 + i * 4);
							}
							vertex.uv[0] = GetF32(m.uvs + size_t{v} * 8);
							vertex.uv[1] = GetF32(m.uvs + size_t{v} * 8 + 4);
						}
						building->part.indices.push_back(static_cast<u16>(local));
					}
				}
			}

			u32 triangles = 0;
			for (const Building& p : parts)
				triangles += static_cast<u32>(p.part.indices.size() / 3);
			if (triangles == 0)
				return "no triangles";

			// Each slot's node: the item root's children in order (child, then next sibling), slot k the k-th.
			s32 children[kMaxSlots];
			u32 child_count = 0;
			for (s32 n = nodes[static_cast<size_t>(root)].child;
				 n >= 0 && static_cast<size_t>(n) < nodes.size() && child_count < kMaxSlots && child_count < kMaxChildHops;
				 n = nodes[static_cast<size_t>(n)].next)
			{
				children[child_count++] = n;
			}

			out->item_id = item_id;
			out->kind = ItemData::KindOf(item_id);
			out->triangles = triangles;
			out->strip_triangles = strip_triangles;
			out->degenerate = degenerate;
			out->flipped = flipped;
			for (int i = 0; i < 3; i++)
			{
				out->bounds_min[i] = GetF32(m.positions + i * 4);
				out->bounds_max[i] = out->bounds_min[i];
			}
			for (u32 v = 1; v < nv; v++)
			{
				for (int i = 0; i < 3; i++)
				{
					const float x = GetF32(m.positions + size_t{v} * 12 + i * 4);
					out->bounds_min[i] = std::min(out->bounds_min[i], x);
					out->bounds_max[i] = std::max(out->bounds_max[i], x);
				}
			}
			size_t bytes = 0;
			for (Building& p : parts)
			{
				Part& part = p.part;
				part.slot = static_cast<u8>(p.slot);
				part.joint = (p.slot < m.joints.size()) ? m.joints[p.slot] : -1;
				if (p.slot < child_count)
				{
					const Node& node = nodes[static_cast<size_t>(children[p.slot])];
					part.has_node = true;
					std::copy(std::begin(node.translation), std::end(node.translation), part.pivot);
					std::copy(std::begin(node.rotation), std::end(node.rotation), part.rotation);
					std::copy(std::begin(node.scale), std::end(node.scale), part.scale);
				}
				if (p.texture >= 0)
				{
					if (const char* why = pages.Get(static_cast<u32>(p.texture), &part.texture))
						return why;
				}
				bytes += part.vertices.size() * sizeof(Vertex) + part.indices.size() * sizeof(u16);
				out->parts.push_back(std::move(part));
			}
			lib.bytes += bytes;
			return nullptr;
		}

		// Every weapon in one OBJS NBD that lib does not hold yet, decoded into lib. One line per failure goes to
		// errors: the file's (nothing from it is kept), or an item's (that item is left out). False when the file
		// itself does not parse.
		bool DecodeObjs(const u8* d, size_t size, const char* name, Library& lib, std::vector<std::string>& errors)
		{
			const auto fail = [&errors, name](const char* why) {
				errors.push_back(fmt::format("{}: {}", name, why));
				return false;
			};
			if (size < kNbdHeaderBytes)
				return fail("shorter than its header");
			size_t tex_off = 0, tex_size = 0, amo_off = 0, amo_size = 0, ahi_off = 0, ahi_size = 0;
			u32 tex_count = 0;
			bool have_tex = false, have_amo = false, have_ahi = false;
			for (size_t k = 0; k < 4; k++)
			{
				const u8* e = d + k * 16;
				const u32 fourcc = Get32(e);
				const u32 off = Get32(e + 4);
				const u32 len = Get32(e + 8);
				bool* have = (fourcc == kFourccTex) ? &have_tex : (fourcc == kFourccAmo) ? &have_amo :
				             (fourcc == kFourccAhi) ? &have_ahi : nullptr;
				if (!have || *have)
					continue;
				if (off < kNbdHeaderBytes || !In(size, off, len))
					return fail("an NBD entry past the file");
				*have = true;
				if (fourcc == kFourccTex)
				{
					tex_off = off;
					tex_size = len;
					tex_count = Get32(e + 12);
				}
				else if (fourcc == kFourccAmo)
				{
					amo_off = off;
					amo_size = len;
				}
				else
				{
					ahi_off = off;
					ahi_size = len;
				}
			}
			if (!have_tex || !have_amo || !have_ahi)
				return fail("no TEX, AMO and AHI entries");

			AmoIndex amo;
			if (const char* why = ParseAmo(d, amo_off, amo_size, &amo))
				return fail(why);
			std::vector<s32> roots;
			std::vector<Node> nodes;
			if (const char* why = ParseAhi(d, ahi_off, ahi_size, &roots, &nodes))
				return fail(why);
			// Root i is the item of mesh i; with the counts apart that pairing cannot be trusted.
			if (roots.size() != amo.meshes.size())
				return fail("hierarchy roots and meshes differ in number");
			TexturePages pages(d, lib);
			if (const char* why = pages.Index(tex_off, tex_size, tex_count))
				return fail(why);

			for (size_t i = 0; i < roots.size(); i++)
			{
				const s32 group = nodes[static_cast<size_t>(roots[i])].group;
				if (group < 0 || group > 0xFFFF)
					continue;
				const u16 id = static_cast<u16>(group);
				if (!ItemData::IsWeapon(ItemData::KindOf(id)) || lib.Has(id))
					continue;
				if (lib.bytes > kMaxLibraryBytes)
				{
					errors.push_back(fmt::format("{}: the library reached {} MB; the rest is left out", name,
						kMaxLibraryBytes >> 20));
					break;
				}
				MeshData mesh;
				Model model;
				const char* why = ParseMesh(d, amo.meshes[i], &mesh);
				if (!why)
					why = BuildModel(d, id, mesh, amo, nodes, roots[i], pages, lib, &model);
				if (why)
				{
					errors.push_back(fmt::format("{}: item {} ({}): {}", name, id, ItemData::Name(id), why));
					continue;
				}
				StringUtil::Strlcpy(model.source, name, sizeof(model.source));
				lib.models.push_back(std::move(model));
			}
			return true;
		}

		// The invariants every published model keeps (the self-test checks them on the disc's and the synthetic ones).
		const char* CheckModel(const Model& m)
		{
			if (m.parts.empty())
				return "a model without parts";
			u32 triangles = 0;
			for (const Part& p : m.parts)
			{
				if (p.indices.empty() || p.indices.size() % 3 != 0)
					return "a part whose index count is not a multiple of three";
				for (const u16 i : p.indices)
				{
					if (i >= p.vertices.size())
						return "a triangle index past its part's vertices";
				}
				for (const Vertex& v : p.vertices)
				{
					for (int k = 0; k < 3; k++)
					{
						if (!std::isfinite(v.pos[k]) || !std::isfinite(v.normal[k]))
							return "a vertex that is not finite";
					}
					if (!std::isfinite(v.uv[0]) || !std::isfinite(v.uv[1]))
						return "a UV that is not finite";
				}
				for (size_t t = 0; t < p.indices.size(); t += 3)
				{
					const Vertex& a = p.vertices[p.indices[t]];
					const Vertex& b = p.vertices[p.indices[t + 1]];
					const Vertex& c = p.vertices[p.indices[t + 2]];
					const Vec3d pa = {a.pos[0], a.pos[1], a.pos[2]};
					const Vec3d face = Cross(Sub({b.pos[0], b.pos[1], b.pos[2]}, pa), Sub({c.pos[0], c.pos[1], c.pos[2]}, pa));
					const Vec3d normals = {double(a.normal[0]) + (double(b.normal[0]) + double(c.normal[0])),
						double(a.normal[1]) + (double(b.normal[1]) + double(c.normal[1])),
						double(a.normal[2]) + (double(b.normal[2]) + double(c.normal[2]))};
					if (Dot(face, normals) < 0.0)
						return "a triangle turned away from its normals";
				}
				if (p.texture)
				{
					if (!IsPow2(p.texture->width) || !IsPow2(p.texture->height) ||
						p.texture->rgba.size() != size_t{p.texture->width} * p.texture->height)
					{
						return "a texture whose size is not a power of two";
					}
				}
				triangles += static_cast<u32>(p.indices.size() / 3);
			}
			if (triangles != m.triangles || m.triangles + m.degenerate != m.strip_triangles)
				return "triangle counts that do not add up";
			return nullptr;
		}

		// --- The load ---------------------------------------------------------------------------------------------------

		struct Loader
		{
			std::mutex mutex; // thread, running, again, summary, key, libraries
			std::thread thread;
			bool running = false; // the worker runs: a load, then the self-test
			bool again = false;   // StartLoad came while it ran: it loads once more before it ends
			std::atomic<bool> cancel{false};
			std::atomic<State> state{State::Idle};
			std::atomic<const Library*> current{nullptr};
			std::string summary;
			std::string key; // serial and image of the library in current
			// Every library ever published, so a Model* from Find stays valid when another disc replaces it.
			std::vector<std::unique_ptr<Library>> libraries;

			~Loader()
			{
				// At exit: the worker stops at its next sector and logs nothing.
				cancel.store(true, std::memory_order_relaxed);
				if (thread.joinable())
					thread.join();
			}
		};

		Loader s_loader;

		void Finish(State state, std::string summary, std::unique_ptr<Library> lib, std::string key, bool publish)
		{
			std::lock_guard lock(s_loader.mutex);
			if (publish)
			{
				const Library* published = lib.get();
				if (lib)
					s_loader.libraries.push_back(std::move(lib));
				s_loader.current.store(published, std::memory_order_release);
				s_loader.key = std::move(key);
			}
			s_loader.summary = std::move(summary);
			s_loader.state.store(state, std::memory_order_release);
		}

		void Load()
		{
			const std::string serial = VMManager::GetDiscSerial();
			const bool file1 = (serial == kFile1Serial);
			if (!file1 && serial != kFile2Serial)
			{
				// Another game: nothing to load, and nothing from an earlier Outbreak disc may answer for it.
				Finish(State::NotOutbreak, fmt::format("not Outbreak File #1 or #2 (serial {})", serial.empty() ? "none" : serial),
					nullptr, {}, true);
				return;
			}
			if (CDVDsys_GetSourceType() != CDVD_SourceType::Iso)
			{
				std::string summary = "(VR) Game models: not loaded: the game was not booted from a disc image.";
				Console.WriteLn(summary);
				Finish(State::NotOutbreak, std::move(summary), nullptr, {}, true);
				return;
			}
			const std::string path = VMManager::GetDiscPath();
			std::string key = serial + '\n' + path;
			{
				std::lock_guard lock(s_loader.mutex);
				if (s_loader.current.load(std::memory_order_acquire) && s_loader.key == key)
				{
					s_loader.state.store(State::Loaded, std::memory_order_release);
					return;
				}
			}

			Common::Timer timer;
			const auto failed = [&key](std::string why) {
				std::string summary = fmt::format("(VR) Game models: not loaded: {}.", why);
				Console.Warning(summary);
				Finish(State::Failed, std::move(summary), nullptr, std::move(key), true);
			};
			DiscReader disc(s_loader.cancel);
			Error error;
			if (!disc.Open(path, &error))
			{
				failed(fmt::format("cannot open the disc image ({})", error.GetDescription()));
				return;
			}
			u32 dat_lsn = 0, dat_size = 0;
			if (const char* why = FindRootFile(disc, "NETBIO00.DAT", &dat_lsn, &dat_size))
			{
				if (!s_loader.cancel.load(std::memory_order_relaxed))
					failed(fmt::format("NETBIO00.DAT: {}", why));
				return;
			}
			std::vector<AfsEntry> archives;
			if (const char* why = ReadAfs(disc, dat_lsn, dat_size, &archives))
			{
				if (!s_loader.cancel.load(std::memory_order_relaxed))
					failed(fmt::format("NETBIO00.DAT: {}", why));
				return;
			}

			auto lib = std::make_unique<Library>();
			lib->file1 = file1;
			std::vector<std::string> errors;
			std::vector<AfsEntry> entries;
			std::vector<u8> nbd;
			for (const AfsEntry& archive : archives)
			{
				if (archive.size < kSector || !IsScenarioArchive(archive.name))
					continue;
				if (const char* why = ReadAfs(disc, archive.lsn, archive.size, &entries))
				{
					errors.push_back(fmt::format("{}: {}", static_cast<const char*>(archive.name), why));
					if (disc.BudgetSpent() || s_loader.cancel.load(std::memory_order_relaxed))
						break;
					continue;
				}
				for (const AfsEntry& e : entries)
				{
					if (e.size == 0 || !IsObjsFile(e.name))
						continue;
					if (e.size > kMaxNbdBytes)
					{
						errors.push_back(fmt::format("{}: {} bytes, over {} MB", static_cast<const char*>(e.name), e.size, kMaxNbdBytes >> 20));
						continue;
					}
					if (const char* why = disc.ReadBytes(u64{e.lsn} * kSector, e.size, &nbd))
					{
						errors.push_back(fmt::format("{}: {}", static_cast<const char*>(e.name), why));
						if (disc.BudgetSpent() || s_loader.cancel.load(std::memory_order_relaxed))
							break;
						continue;
					}
					if (DecodeObjs(nbd.data(), nbd.size(), e.name, *lib, errors))
						lib->files++;
				}
				if (disc.BudgetSpent() || s_loader.cancel.load(std::memory_order_relaxed))
					break;
			}
			if (s_loader.cancel.load(std::memory_order_relaxed))
				return;
			std::sort(lib->models.begin(), lib->models.end(),
				[](const Model& a, const Model& b) { return a.item_id < b.item_id; });

			std::string missing;
			for (u32 i = 0; i < ItemData::Count(); i++)
			{
				const ItemData::Entry& item = ItemData::At(i);
				if (ItemData::IsWeapon(item.kind) && !lib->Has(item.id))
					missing += fmt::format("{}{}", missing.empty() ? "" : ", ", item.id);
			}
			const double ms = timer.GetTimeMilliseconds();
			std::string summary = fmt::format(
				"(VR) Game models: {} weapon(s) from {} file(s) in {:.0f} ms ({}, {:.1f} MB read, {:.1f} MB decoded); missing ids: {}.",
				lib->models.size(), lib->files, ms, file1 ? "File #1" : "File #2",
				static_cast<double>(disc.BytesRead()) / 1048576.0, static_cast<double>(lib->bytes) / 1048576.0,
				missing.empty() ? "none" : missing);
			for (size_t i = 0; i < errors.size() && i < kMaxErrorLines; i++)
				Console.Warning(fmt::format("(VR) Game models: {}", errors[i]));
			if (errors.size() > kMaxErrorLines)
				Console.Warning(fmt::format("(VR) Game models: {} more problem(s) not shown.", errors.size() - kMaxErrorLines));
			Console.WriteLn(summary);
			if (lib->models.empty())
				Finish(State::Failed, std::move(summary), nullptr, std::move(key), true);
			else
				Finish(State::Loaded, std::move(summary), std::move(lib), std::move(key), true);
		}

		void Worker()
		{
			Threading::SetNameOfCurrentThread("VR Game Models");
			static const bool s_selftest = (std::getenv("PCSX2_VR_MODELS_SELFTEST") != nullptr);
			bool tested = false;
			for (;;)
			{
				Load();
				if (s_selftest && !tested && !s_loader.cancel.load(std::memory_order_relaxed))
				{
					tested = true;
					SelfTest();
				}
				// Under the lock, so StartLoad either sees this worker running (and leaves it a request) or joins a
				// thread that only returns.
				std::lock_guard lock(s_loader.mutex);
				if (!s_loader.again || s_loader.cancel.load(std::memory_order_relaxed))
				{
					s_loader.running = false;
					return;
				}
				s_loader.again = false;
				s_loader.state.store(State::Loading, std::memory_order_release);
			}
		}

		// --- Self-test helpers: a small NBD built in code -----------------------------------------------------------

		class Writer
		{
		public:
			std::vector<u8> bytes;

			void U8(u8 v) { bytes.push_back(v); }
			void U16(u16 v) { Put(&v, sizeof(v)); }
			void U32(u32 v) { Put(&v, sizeof(v)); }
			void S32(s32 v) { Put(&v, sizeof(v)); }
			void F32(float v) { Put(&v, sizeof(v)); }
			void Text(const char* s) { Put(s, std::strlen(s)); }
			void Zeros(size_t n) { bytes.insert(bytes.end(), n, 0); }
			void Align(size_t a)
			{
				while (bytes.size() % a)
					bytes.push_back(0);
			}
			void Put(const void* p, size_t n)
			{
				const u8* b = static_cast<const u8*>(p);
				bytes.insert(bytes.end(), b, b + n);
			}
			void Patch32(size_t at, u32 v) { std::memcpy(&bytes[at], &v, sizeof(v)); }
			// A chunk header whose size End() fills in.
			size_t Begin(u32 type, u32 count)
			{
				const size_t at = bytes.size();
				U32(type);
				U32(count);
				U32(0);
				return at;
			}
			void End(size_t at) { Patch32(at + 8, static_cast<u32>(bytes.size() - at)); }
		};

		// What the synthetic CLUT entry i holds, as stored on the disc.
		u32 SyntheticClut(u32 i, bool eight_bit)
		{
			const u32 a = eight_bit ? 255 : 0x80;
			return (a << 24) | (((i ^ 0x5A) & 0xFF) << 16) | ((255 - i) << 8) | i;
		}

		// A 16 x 16 TIM2: 4-bit (picture header at 0x10, PS2 alpha 0x80, rows 8-15 one colour) or 8-bit (header at 0x80,
		// alpha 255, CSM1 so the decoder must swap its CLUT order back), pixel (x, y) = y * 16 + x.
		std::vector<u8> SyntheticTim2(bool eight_bit)
		{
			Writer w;
			w.Text("TIM2");
			w.U8(4);
			w.U8(eight_bit ? 1 : 0);
			w.U16(1);
			w.U16(0x1234); // Outbreak's global texture id
			w.Zeros(6);
			if (eight_bit)
				w.Zeros(0x80 - 16);
			const u32 colours = eight_bit ? 256 : 16;
			const u32 pixel_bytes = eight_bit ? 256 : 128;
			w.U32(48 + pixel_bytes + colours * 4);
			w.U32(colours * 4);
			w.U32(pixel_bytes);
			w.U16(48);
			w.U16(static_cast<u16>(colours));
			w.U8(0);
			w.U8(1);
			w.U8(0x03); // RGBA32, CSM1
			w.U8(eight_bit ? 5 : 4);
			w.U16(16);
			w.U16(16);
			w.Zeros(24);
			for (u32 y = 0; y < 16; y++)
			{
				for (u32 x = 0; x < 16; x += eight_bit ? 1 : 2)
				{
					if (eight_bit)
						w.U8(static_cast<u8>(y * 16 + x));
					else if (y < 8)
						w.U8(static_cast<u8>(((x + y) & 15) | (((x + 1 + y) & 15) << 4)));
					else
						w.U8(0x77);
				}
			}
			// The stored order is CSM1's: entries 8-15 and 16-23 of every 32 trade places.
			for (u32 i = 0; i < colours; i++)
			{
				u32 stored = i;
				if (eight_bit && (i & 31) >= 8 && (i & 31) < 16)
					stored = i + 8;
				else if (eight_bit && (i & 31) >= 16 && (i & 31) < 24)
					stored = i - 8;
				w.U32(SyntheticClut(stored, eight_bit));
			}
			return w.bytes;
		}

		// SLD with literals, zero runs (short and long codes), back references and the end code the game's compressor
		// writes: enough to walk every path of DecompressSld.
		std::vector<u8> SldEncode(const std::vector<u8>& raw)
		{
			std::vector<u16> in(raw.size() / 2);
			std::memcpy(in.data(), raw.data(), in.size() * 2);
			std::vector<u16> out;
			size_t flag_at = 0;
			u32 bit = 16;
			const auto put = [&](bool code, std::initializer_list<u16> words) {
				if (bit == 16)
				{
					flag_at = out.size();
					out.push_back(0);
					bit = 0;
				}
				if (code)
					out[flag_at] |= static_cast<u16>(0x8000u >> bit);
				bit++;
				out.insert(out.end(), words);
			};
			for (size_t i = 0; i < in.size();)
			{
				size_t zeros = 0;
				while (i + zeros < in.size() && in[i + zeros] == 0 && zeros < 0xFFFF)
					zeros++;
				if (zeros >= 2)
				{
					if (zeros < 32)
						put(true, {static_cast<u16>(zeros << 11)});
					else
						put(true, {0, static_cast<u16>(zeros)});
					i += zeros;
					continue;
				}
				size_t repeat = 0;
				while (i >= 1 && i + repeat < in.size() && in[i + repeat] == in[i - 1 + repeat] && repeat < 31)
					repeat++;
				if (repeat >= 2)
				{
					put(true, {static_cast<u16>((repeat << 11) | 1)});
					i += repeat;
					continue;
				}
				put(false, {in[i]});
				i++;
			}
			put(true, {0, 0});
			std::vector<u8> bytes(out.size() * 2);
			std::memcpy(bytes.data(), out.data(), bytes.size());
			return bytes;
		}

		struct SyntheticVertex
		{
			float x, y, z, u, v;
			u32 slot;
		};

		// A mesh whose normals all point +Z, with its strips (FW then PW) and per-strip material references.
		void WriteMesh(Writer& w, const std::vector<SyntheticVertex>& verts,
			const std::vector<std::vector<u32>>& fw, const std::vector<std::vector<u32>>& pw,
			const std::vector<s32>& strip_materials, const std::vector<s32>& material_refs, const std::vector<s32>& joints,
			bool weights)
		{
			// One more subchunk than it holds, like File #1's OBJS meshes: the parser must stop at the size.
			const size_t mesh = w.Begin(kChunkMesh, 12);
			const size_t prims = w.Begin(kChunkPrimLists, pw.empty() ? 1 : 2);
			for (int l = 0; l < (pw.empty() ? 1 : 2); l++)
			{
				const std::vector<std::vector<u32>>& strips = (l == 0) ? fw : pw;
				const size_t list = w.Begin(l == 0 ? kChunkStripsFW : kChunkStripsPW, static_cast<u32>(strips.size()));
				for (const std::vector<u32>& s : strips)
				{
					w.U32(static_cast<u32>(s.size()));
					for (const u32 i : s)
						w.U32(i);
				}
				w.End(list);
			}
			w.End(prims);
			const auto ints = [&w](u32 type, const std::vector<s32>& v) {
				const size_t c = w.Begin(type, static_cast<u32>(v.size()));
				for (const s32 i : v)
					w.S32(i);
				w.End(c);
			};
			ints(kChunkMaterialRefs, material_refs);
			ints(kChunkStripMaterials, strip_materials);
			size_t c = w.Begin(kChunkVertices, static_cast<u32>(verts.size()));
			for (const SyntheticVertex& v : verts)
			{
				w.F32(v.x);
				w.F32(v.y);
				w.F32(v.z);
			}
			w.End(c);
			c = w.Begin(kChunkNormals, static_cast<u32>(verts.size()));
			for (size_t i = 0; i < verts.size(); i++)
			{
				w.F32(0.0f);
				w.F32(0.0f);
				w.F32(1.0f);
			}
			w.End(c);
			c = w.Begin(kChunkTexCoords, static_cast<u32>(verts.size()));
			for (const SyntheticVertex& v : verts)
			{
				w.F32(v.u);
				w.F32(v.v);
			}
			w.End(c);
			if (weights)
			{
				c = w.Begin(kChunkWeights, static_cast<u32>(verts.size()));
				for (const SyntheticVertex& v : verts)
				{
					w.U32(1);
					w.U32(v.slot);
					w.F32(100.0f);
				}
				w.End(c);
				ints(kChunkJointRefs, joints);
			}
			w.End(mesh);
		}

		void WriteNode(Writer& w, u32 type, s32 id, s32 parent, s32 child, s32 next, const float scale[3],
			const float rotation[3], const float translation[3], s32 group)
		{
			const size_t n = w.Begin(type, 1);
			w.S32(id);
			w.S32(parent);
			w.S32(child);
			w.S32(next);
			for (const float* v : {scale, rotation, translation})
			{
				w.F32(v[0]);
				w.F32(v[1]);
				w.F32(v[2]);
				w.F32(1.0f);
			}
			w.S32(-1);
			w.S32(group);
			w.Zeros(46 * 4);
			w.End(n);
		}

		// The synthetic OBJS file: root 0 is item 0 (the Handgun) with two slots and two texture pages, root 1 is item
		// 300 (a herb, which is no weapon and must be skipped).
		std::vector<u8> SyntheticObjs()
		{
			Writer w;
			w.Zeros(kNbdHeaderBytes);

			const size_t tex_off = w.bytes.size();
			for (const bool eight_bit : {false, true})
			{
				const std::vector<u8> sld = SldEncode(SyntheticTim2(eight_bit));
				w.U32(static_cast<u32>(sld.size()));
				w.Put(sld.data(), sld.size());
			}
			const size_t tex_size = w.bytes.size() - tex_off;
			w.Align(16);

			const size_t amo_off = w.bytes.size();
			const size_t model = w.Begin(kChunkModel, 3);
			const size_t meshes = w.Begin(kChunkMeshList, 2);
			// Strip A (0 1 2 3): two triangles, both counter-clockwise about +Z once the odd one is swapped back. Strip
			// D repeats an index (dropped). Strip B (4 5 6) in the PW list winds clockwise about +Z: it is turned round.
			WriteMesh(w,
				{{0, 0, 0, 0.0f, 1.0f, 0}, {1, 0, 0, 0.1f, 0.9f, 0}, {0, 1, 0, 0.2f, 0.8f, 0}, {1, 1, 0, 0.3f, 0.7f, 0},
					{2, 0, 0, 0.4f, 0.6f, 1}, {2, 1, 0, 0.5f, 0.5f, 1}, {3, 0, 0, 0.6f, 0.4f, 1}},
				{{0, 1, 2, 3}, {0, 0, 1}}, {{4, 5, 6}}, {0, 0, 1}, {0, 1}, {7, 8}, true);
			WriteMesh(w, {{0, 0, 0, 0, 0, 0}, {1, 0, 0, 1, 0, 0}, {0, 1, 0, 0, 1, 0}}, {{0, 1, 2}}, {}, {0}, {0}, {}, false);
			w.End(meshes);
			const size_t mats = w.Begin(kChunkMaterialList, 2);
			for (s32 t = 0; t < 2; t++)
			{
				const size_t m = w.Begin(kChunkMaterialLambert, 1);
				w.Zeros(3 * 16 + 4);
				w.S32(1);
				w.Zeros(50 * 4);
				w.S32(t);
				w.End(m);
			}
			w.End(mats);
			const size_t texs = w.Begin(kChunkTextureList, 2);
			for (s32 t = 0; t < 2; t++)
			{
				const size_t r = w.Begin(kChunkTexRef, 1);
				w.S32(t);
				w.S32(16);
				w.S32(16);
				w.S32(0);
				w.Zeros(60 * 4);
				w.End(r);
			}
			w.End(texs);
			w.End(model);
			const size_t amo_size = w.bytes.size() - amo_off;
			w.Align(16);

			const size_t ahi_off = w.bytes.size();
			const size_t h = w.Begin(kChunkHierarchy, 5);
			const size_t rl = w.Begin(kChunkRootList, 2);
			w.S32(0);
			w.S32(3);
			w.End(rl);
			const float one[3] = {1.0f, 1.0f, 1.0f};
			const float zero[3] = {0.0f, 0.0f, 0.0f};
			// Node 2 is a joint: stored with its rotation negated and its scale as the reciprocal.
			const float joint_scale[3] = {2.0f, 4.0f, 0.5f};
			const float joint_rotation[3] = {0.1f, 0.2f, 0.3f};
			const float pivot[3] = {1.5f, 2.5f, 3.5f};
			WriteNode(w, kChunkNodePlain, 0, -1, 1, 3, one, zero, zero, 0);
			WriteNode(w, kChunkNodePlain, 1, 0, -1, 2, one, zero, zero, 0);
			WriteNode(w, kChunkNodeJoint, 2, 0, -1, -1, joint_scale, joint_rotation, pivot, 0);
			WriteNode(w, kChunkNodePlain, 3, -1, -1, -1, one, zero, zero, 300);
			w.End(h);
			const size_t ahi_size = w.bytes.size() - ahi_off;

			const u32 header[16] = {kFourccTex, static_cast<u32>(tex_off), static_cast<u32>(tex_size), 2, kFourccAmo,
				static_cast<u32>(amo_off), static_cast<u32>(amo_size), 0, kFourccAhi, static_cast<u32>(ahi_off),
				static_cast<u32>(ahi_size), 0, 0, 0, 0, 0};
			std::memcpy(w.bytes.data(), header, sizeof(header));
			return w.bytes;
		}

		u32 Swizzle8(u32 i)
		{
			if ((i & 31) >= 8 && (i & 31) < 16)
				return i + 8;
			if ((i & 31) >= 16 && (i & 31) < 24)
				return i - 8;
			return i;
		}
	} // namespace

	void StartLoad()
	{
		std::lock_guard lock(s_loader.mutex);
		if (s_loader.running)
		{
			s_loader.again = true;
			return;
		}
		// The last worker cleared running under this lock and only returns now: the join is immediate.
		if (s_loader.thread.joinable())
			s_loader.thread.join();
		s_loader.cancel.store(false, std::memory_order_relaxed);
		s_loader.running = true;
		s_loader.again = false;
		s_loader.state.store(State::Loading, std::memory_order_release);
		// Never on the EE, GS or VR pacer threads: a few MB of reads and decoding would stall them.
		s_loader.thread = std::thread(Worker);
	}

	const Model* Find(u16 item_id)
	{
		const Library* lib = s_loader.current.load(std::memory_order_acquire);
		if (!lib)
			return nullptr;
		const auto it = std::lower_bound(lib->models.begin(), lib->models.end(), item_id,
			[](const Model& m, u16 id) { return m.item_id < id; });
		return (it != lib->models.end() && it->item_id == item_id) ? &*it : nullptr;
	}

	State LoadState(std::string* summary)
	{
		if (!summary)
			return s_loader.state.load(std::memory_order_acquire);
		std::lock_guard lock(s_loader.mutex);
		*summary = s_loader.summary;
		return s_loader.state.load(std::memory_order_acquire);
	}

	const char* StateName(State state)
	{
		switch (state)
		{
			case State::Idle: return "idle";
			case State::Loading: return "loading";
			case State::Loaded: return "loaded";
			case State::NotOutbreak: return "not Outbreak";
			case State::Failed: return "failed";
		}
		return "unknown";
	}

	bool SelfTest()
	{
		int checks = 0;
		int failures = 0;
		const auto check = [&](bool ok, const std::string& name) {
			checks++;
			if (ok)
				return;
			failures++;
			Console.Warning(fmt::format("(VR) Game models self-test FAILED: {}", name));
		};

		// --- The synthetic NBD --------------------------------------------------------------------------------------
		const std::vector<u8> objs = SyntheticObjs();
		{
			Library lib;
			std::vector<std::string> errors;
			const bool parsed = DecodeObjs(objs.data(), objs.size(), "SELFTEST.NBD", lib, errors);
			check(parsed && errors.empty(), "the synthetic NBD decodes without a problem");
			check(lib.models.size() == 1 && !lib.models.empty() && lib.models[0].item_id == 0,
				"only its weapon (item 0) is kept, the herb (300) is skipped");
			if (lib.models.size() == 1)
			{
				const Model& m = lib.models[0];
				check(CheckModel(m) == nullptr, "the synthetic model keeps the invariants");
				check(m.strip_triangles == 4 && m.degenerate == 1 && m.flipped == 1 && m.triangles == 3,
					"4 strip triangles: 1 degenerate dropped, 1 turned to its normals, 3 kept");
				check(m.parts.size() == 2, "two parts: slot 0 on page 0, slot 1 on page 1");
				check(m.bounds_min[0] == 0.0f && m.bounds_max[0] == 3.0f && m.bounds_max[1] == 1.0f,
					"the bounds cover every vertex");
				check(m.kind == ItemData::Kind::Handgun && std::strcmp(m.source, "SELFTEST.NBD") == 0,
					"the model names its kind and source");
				if (m.parts.size() == 2)
				{
					const Part& p0 = m.parts[0];
					const Part& p1 = m.parts[1];
					check(p0.slot == 0 && p0.vertices.size() == 4 && p0.indices.size() == 6 && p0.joint == 7 &&
							  p0.has_node && p0.pivot[0] == 0.0f,
						"part 0: slot 0, 4 vertices, 2 triangles, joint 7, the first child's pivot");
					check(p1.slot == 1 && p1.vertices.size() == 3 && p1.indices.size() == 3 && p1.joint == 8,
						"part 1: slot 1, 3 vertices, 1 triangle, joint 8");
					check(p1.has_node && p1.pivot[0] == 1.5f && p1.pivot[1] == 2.5f && p1.pivot[2] == 3.5f,
						"part 1's pivot is the second child's translation");
					check(std::abs(p1.rotation[0] + 0.1f) < 1e-6f && std::abs(p1.rotation[2] + 0.3f) < 1e-6f &&
							  p1.scale[0] == 0.5f && p1.scale[1] == 0.25f && p1.scale[2] == 2.0f,
						"a joint node's rotation comes back negated and its scale as the reciprocal");
					check(p0.indices.size() == 6 && p0.indices[0] == 0 && p0.indices[1] == 1 && p0.indices[2] == 2 &&
							  p0.indices[3] == 1 && p0.indices[4] == 3 && p0.indices[5] == 2,
						"the odd strip triangle's last two indices are swapped");
					check(p0.vertices.size() == 4 && p0.vertices[0].uv[0] == 0.0f && p0.vertices[0].uv[1] == 1.0f &&
							  p0.vertices[3].uv[0] == 0.3f,
						"UVs come through per vertex");
					const Texture* t0 = p0.texture;
					const Texture* t1 = p1.texture;
					check(t0 && t1 && t0 != t1 && t0->width == 16 && t0->height == 16 && t1->rgba.size() == 256,
						"two 16 x 16 pages");
					if (t0 && t1 && t0->rgba.size() == 256 && t1->rgba.size() == 256)
					{
						const auto doubled = [](u32 c) { return (c & 0x00FFFFFFu) | 0xFF000000u; };
						check(t0->rgba[1] == doubled(SyntheticClut(1, false)) &&
								  t0->rgba[2 * 16 + 3] == doubled(SyntheticClut(5, false)) &&
								  t0->rgba[12 * 16 + 5] == doubled(SyntheticClut(7, false)),
							"4-bit: low nibble first, the back-referenced rows, alpha 0x80 doubled to 255");
						bool swapped = true;
						for (u32 i = 0; i < 256; i++)
							swapped = swapped && (t1->rgba[i] == SyntheticClut(i, true));
						check(swapped && Swizzle8(8) == 16 && Swizzle8(40) == 48,
							"8-bit: the CSM1 CLUT order is undone (entries 8-15 and 16-23 of every 32)");
					}
				}
			}
			check(lib.textures.size() == 2, "each page is decoded once");
		}
		{
			// Cut short at every length, and every byte flipped two ways: nothing may crash, and whatever decodes keeps
			// the invariants.
			int broken_kept = 0;
			std::vector<std::string> errors;
			for (size_t len = 0; len < objs.size(); len++)
			{
				Library lib;
				errors.clear();
				DecodeObjs(objs.data(), len, "SELFTEST.NBD", lib, errors);
				for (const Model& m : lib.models)
					broken_kept += (CheckModel(m) != nullptr);
			}
			std::vector<u8> bent = objs;
			for (size_t i = 0; i < bent.size(); i++)
			{
				for (const u8 flip : {u8{0xFF}, u8{0x80}})
				{
					bent[i] ^= flip;
					Library lib;
					errors.clear();
					DecodeObjs(bent.data(), bent.size(), "SELFTEST.NBD", lib, errors);
					for (const Model& m : lib.models)
						broken_kept += (CheckModel(m) != nullptr);
					bent[i] ^= flip;
				}
			}
			check(broken_kept == 0, fmt::format("a cut or bent NBD never yields a model that breaks an invariant ({} did)",
										broken_kept));
		}
		{
			std::vector<u8> out;
			const u8 bad_reference[] = {0x00, 0x80, 0x01, 0x10}; // a back reference before the start
			check(!DecompressSld(bad_reference, sizeof(bad_reference), 1024, &out), "SLD refuses a reference before the start");
			const u8 long_run[] = {0x00, 0x80, 0x00, 0x00, 0xFF, 0x7F}; // 32767 zero words
			check(!DecompressSld(long_run, sizeof(long_run), 1024, &out), "SLD stops at its output cap");
		}

		// --- The disc's library -------------------------------------------------------------------------------------
		const Library* lib = s_loader.current.load(std::memory_order_acquire);
		if (lib)
		{
			// gamemodels_check.py on Alex's EN-patched images; another release may differ (run it on that disc).
			static constexpr u16 kMissing1[] = {2, 5, 6, 7, 12, 13, 15, 16, 17, 18, 24, 26, 30, 107, 109};
			static constexpr u16 kMissing2[] = {6, 16, 17, 18, 25, 26, 27, 31, 32};
			const size_t expect_models = lib->file1 ? 37 : 43;
			const u32 expect_files = lib->file1 ? 5 : 12;
			check(lib->models.size() == expect_models && lib->files == expect_files,
				fmt::format("{} weapon(s) from {} file(s), as gamemodels_check.py reads {} ({} from {})", lib->models.size(),
					lib->files, lib->file1 ? "File #1" : "File #2", expect_models, expect_files));
			std::vector<u16> missing;
			for (u32 i = 0; i < ItemData::Count(); i++)
			{
				const ItemData::Entry& item = ItemData::At(i);
				if (ItemData::IsWeapon(item.kind) && !lib->Has(item.id))
					missing.push_back(item.id);
			}
			const bool same_missing = lib->file1 ? std::equal(missing.begin(), missing.end(), std::begin(kMissing1), std::end(kMissing1)) :
			                                       std::equal(missing.begin(), missing.end(), std::begin(kMissing2), std::end(kMissing2));
			check(same_missing, "the missing ids are the ones gamemodels_check.py lists");
			for (const u16 id : {u16{6}, u16{16}, u16{17}, u16{18}, u16{26}})
			{
				if (lib->Has(id))
					Console.WriteLn(fmt::format("(VR) Game models self-test: item {} ({}) has a mesh after all, which the research did not find.",
						id, ItemData::Name(id)));
			}
			const Model* first_bad = nullptr;
			const char* why = nullptr;
			for (const Model& m : lib->models)
			{
				if (!ItemData::IsWeapon(m.kind) || std::strncmp(m.source, "OBJS", 4) != 0)
					why = "a model that is no weapon or names no OBJS file";
				else
					why = CheckModel(m);
				if (why)
				{
					first_bad = &m;
					break;
				}
			}
			check(!first_bad, first_bad ? fmt::format("item {}: {}", first_bad->item_id, why) :
			                              std::string("every model keeps the invariants (indices in range, finite UVs, power-of-two pages)"));
			struct ResearchCount
			{
				u16 id;
				u32 research;
			};
			static constexpr ResearchCount kResearch[] = {{0, 259}, {400, 299}, {11, 250}};
			std::string counts;
			for (const ResearchCount& r : kResearch)
			{
				const u16 id = r.id;
				const u32 research = r.research;
				const Model* m = nullptr;
				for (const Model& candidate : lib->models)
				{
					if (candidate.item_id == id)
						m = &candidate;
				}
				check(m && m->strip_triangles == research,
					fmt::format("item {} ({}) spells {} strip triangles, the research counted {}", id, ItemData::Name(id),
						m ? m->strip_triangles : 0, research));
				if (m)
				{
					counts += fmt::format("{}{} {} ({} kept: {} degenerate dropped, {} turned to their normals; research {})",
						counts.empty() ? "" : ", ", ItemData::Name(id), m->strip_triangles, m->triangles, m->degenerate,
						m->flipped, research);
				}
			}
			Console.WriteLn(fmt::format("(VR) Game models self-test: {}.", counts));
		}
		else
		{
			std::string summary;
			const State state = LoadState(&summary);
			Console.WriteLn(fmt::format("(VR) Game models self-test: no disc library to check ({}).", StateName(state)));
		}

		if (failures == 0)
			Console.WriteLn(fmt::format("(VR) Game models self-test: {} checks passed.", checks));
		else
			Console.Warning(fmt::format("(VR) Game models self-test: {} of {} checks failed.", failures, checks));
		return failures == 0;
	}
} // namespace VR::GameModels
