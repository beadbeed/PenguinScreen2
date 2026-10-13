#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Patrick Carey <patrickfcarey@gmail.com>
# SPDX-License-Identifier: GPL-3.0
"""Reference decoder for pcsx2/VR/GameModels.cpp: reads a Resident Evil Outbreak disc image (File #1 SLPM-65428 or
File #2 SLPM-65692) read-only and prints the same summary the emulator logs, so the C++ result can be compared on the
same disc. It writes nothing: no extraction, no cache, and it never prints texture or mesh contents, only counts and
sizes.

    python -I tools/outbreak-vr/gamemodels_check.py "D:/Games/PS2/games/Biohazard Outbreak File 1 (SLPM-65428).iso"
    python -I tools/outbreak-vr/gamemodels_check.py --items IMAGE    # also one line per weapon

Flat 2048-byte-sector ISO images only (the emulator also reads CHD and CSO through PCSX2's own readers).

The rules are GameModels.cpp's, step for step: ISO9660 root -> NETBIO00.DAT (an AFS archive) -> every rXXX.afs in it
-> every OBJSxx.NBD in those -> per root node of the NBD's hierarchy (its groupId is the item id, mesh i belongs to
root i) the weapons VR::ItemData names (kinds Handgun, LongGun, Melee, Thrown, Spray, OtherWeapon); the first copy of
an item id wins. Strips are unrolled with alternating winding, degenerate triangles dropped, and each triangle turned
so its face normal agrees with its vertex normals. A vertex belongs to the part of its heaviest weight slot; a part is
one rigid piece (slot) with one texture page.

Format knowledge and the parts this follows: Fothsid's libthanatos (https://github.com/Fothsid/libthanatos: OBNbd.cpp,
OBSld.cpp, OBMesh.cpp, OBTStripList.cpp, OBNode.cpp, OBMaterial.cpp, OBTextureList.cpp) and OBTool
(https://github.com/Fothsid/OBTool: tim2.c, convert_to_fbx.cpp, afs.c), both GPL-3.0; the layout notes in
tools/outbreak-vr/research/2026-10-12-weapon-models-research.md. Item ids and kinds: pcsx2/VR/ItemData.cpp, which this
reads so the two stay one table.
"""
import argparse
import math
import os
import re
import struct
import sys
import time

SECTOR = 2048
SERIALS = ('SLPM-65428', 'SLPM-65692')
WEAPON_KINDS = ('Handgun', 'LongGun', 'Melee', 'Thrown', 'Spray', 'OtherWeapon')

# The same limits as GameModels.cpp: anything past them is a malformed or unexpected disc.
MAX_AFS_ENTRIES = 8192
MAX_NBD_BYTES = 16 << 20
MAX_TEX_BYTES = 4 << 20
MAX_TOTAL_READ = 64 << 20
MAX_TEXTURE_SIDE = 1024
MAX_LIBRARY_BYTES = 48 << 20
MAX_CHUNKS = 1 << 16
MAX_VERTICES = 65535
MAX_SLOTS = 16

FOURCC_TEX, FOURCC_AMO, FOURCC_AHI = 0x584554, 0x4F4D41, 0x494841

CHUNK_MODEL, CHUNK_MESHLIST, CHUNK_MESH, CHUNK_PRIMLISTS = 1, 2, 4, 5
CHUNK_MATERIALLIST, CHUNK_TEXTURELIST, CHUNK_TEXREF = 9, 0xA, 0
CHUNK_STRIP_FW, CHUNK_STRIP_PW = 0x30000, 0x40000
CHUNK_MATREFS, CHUNK_STRIPMATS = 0x50000, 0x60000
CHUNK_VERTICES, CHUNK_NORMALS, CHUNK_TEXCOORDS = 0x70000, 0x80000, 0xA0000
CHUNK_WEIGHTS, CHUNK_JOINTREFS = 0xC0000, 0x100000
MATERIAL_TYPES = (0x120000, 0x130000, 0x140000, 0x150000)
CHUNK_HIERARCHY, CHUNK_NODE_PLAIN, CHUNK_NODE_MESH, CHUNK_NODE_JOINT = 0xC0000000, 0x40000001, 0x40000002, 0x40000003
NODE_BYTES = 12 + 4 * 4 + 12 * 4 + 2 * 4 + 46 * 4
MATERIAL_BODY_BYTES = 3 * 16 + 4 + 4 + 50 * 4
TEXREF_BYTES = 12 + 4 * 4 + 60 * 4


class ParseError(Exception):
	pass


def need(cond, what):
	if not cond:
		raise ParseError(what)


def u32(buf, off):
	need(0 <= off and off + 4 <= len(buf), 'read past the end')
	return struct.unpack_from('<I', buf, off)[0]


def s32(buf, off):
	need(0 <= off and off + 4 <= len(buf), 'read past the end')
	return struct.unpack_from('<i', buf, off)[0]


def floats(buf, off, n):
	need(0 <= off and off + 4 * n <= len(buf), 'read past the end')
	return struct.unpack_from('<%df' % n, buf, off)


class Disc:
	"""Read-only sector access with an account of what was read (each sector counted once)."""

	def __init__(self, path):
		self.f = open(path, 'rb')
		self.f.seek(0, 2)
		self.sectors = self.f.tell() // SECTOR
		self.read_sectors = set()
		self.read_bytes = 0

	def read(self, lba, count):
		need(count >= 0 and lba >= 0 and lba + count <= self.sectors, 'sector range off the disc')
		need(self.read_bytes + count * SECTOR <= MAX_TOTAL_READ, 'read budget spent')
		self.read_bytes += count * SECTOR
		self.read_sectors.update(range(lba, lba + count))
		self.f.seek(lba * SECTOR)
		data = self.f.read(count * SECTOR)
		need(len(data) == count * SECTOR, 'short read')
		return data

	def read_range(self, byte_off, size):
		"""size bytes at byte_off (from the disc start), reading whole sectors."""
		first = byte_off // SECTOR
		last = (byte_off + size + SECTOR - 1) // SECTOR
		data = self.read(first, last - first)
		start = byte_off - first * SECTOR
		return data[start:start + size]


def iso_find_root_file(disc, name):
	pvd = disc.read(16, 1)
	need(pvd[0] == 1 and pvd[1:6] == b'CD001', 'no ISO9660 primary volume descriptor')
	root = pvd[156:156 + 34]
	lba, size = struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0]
	need(0 < size <= 64 * SECTOR, 'root directory size')
	data = disc.read(lba, (size + SECTOR - 1) // SECTOR)
	pos = 0
	while pos < size:
		reclen = data[pos]
		if reclen == 0:
			pos = (pos // SECTOR + 1) * SECTOR
			continue
		need(reclen >= 34 and pos + reclen <= len(data), 'directory record')
		namelen = data[pos + 32]
		need(33 + namelen <= reclen, 'directory record name')
		ident = data[pos + 33:pos + 33 + namelen].decode('ascii', 'replace').split(';')[0]
		if ident.upper() == name.upper() and not (data[pos + 25] & 2):
			return struct.unpack_from('<I', data, pos + 2)[0], struct.unpack_from('<I', data, pos + 10)[0]
		pos += reclen
	raise ParseError('%s not in the root directory' % name)


def afs_entries(disc, base_lba, total_size):
	"""[(name, lba, size)] of an AFS archive at base_lba (total_size bytes long). Names are '' when the archive has no
	usable name table."""
	head = disc.read(base_lba, 1)
	need(head[:4] == b'AFS\0', 'not an AFS archive')
	count = u32(head, 4)
	need(0 < count <= MAX_AFS_ENTRIES, 'AFS entry count')
	table_bytes = 8 + count * 8 + 8
	need(table_bytes <= total_size, 'an AFS table past the archive')
	table = disc.read_range(base_lba * SECTOR, table_bytes)
	entries = []
	first = None
	for i in range(count):
		off, size = struct.unpack_from('<II', table, 8 + i * 8)
		# One bad entry (File #2's r010.afs has a size of 0xFFFFF801) makes only that entry unusable.
		if size == 0 or off % SECTOR != 0 or off < table_bytes - 8 or off + size > total_size:
			entries.append((0, 0))
			continue
		entries.append((off, size))
		first = off if first is None else min(first, off)

	def name_table_at(off, size):
		return off != 0 and size >= count * 48 and off % 4 == 0 and off + count * 48 <= total_size

	noff, nsize = struct.unpack_from('<II', table, 8 + count * 8)
	if not name_table_at(noff, nsize) and first is not None and first >= 8:
		ptr = disc.read_range(base_lba * SECTOR + first - 8, 8)
		noff, nsize = struct.unpack_from('<II', ptr, 0)
	names = [''] * count
	if name_table_at(noff, nsize):
		nt = disc.read_range(base_lba * SECTOR + noff, count * 48)
		names = [nt[i * 48:i * 48 + 32].split(b'\0')[0].decode('ascii', 'replace') for i in range(count)]
	return [(names[i], base_lba + off // SECTOR, size) for i, (off, size) in enumerate(entries)]


def sld_decompress(src, out_cap):
	"""libthanatos OBSld::decompress, bounded: 16-bit words; a flag word read MSB first; 0 = one literal word, 1 = a
	code: count = code >> 11 (0: the next word is the count), offset = code & 0x7FF words back (0: a run of zeros)."""
	n = len(src) // 2
	s = struct.unpack('<%dH' % n, src[:n * 2])
	cap = out_cap // 2
	d = []
	sp = 0
	bits = 0
	flag = 0
	while sp < n:
		if bits == 0:
			flag = s[sp]
			sp += 1
			bits = 16
			if sp >= n:
				break
		if flag & 0x8000:
			code = s[sp]
			sp += 1
			cnt = code >> 11
			if cnt == 0:
				need(sp < n, 'SLD code past the end')
				cnt = s[sp]
				sp += 1
			back = code & 0x7FF
			need(len(d) + cnt <= cap, 'SLD output too large')
			if back == 0:
				d.extend([0] * cnt)
			else:
				need(back <= len(d), 'SLD back reference before the start')
				p = len(d) - back
				for i in range(cnt):
					d.append(d[p + i])
		else:
			need(len(d) < cap, 'SLD output too large')
			d.append(s[sp])
			sp += 1
		flag = (flag << 1) & 0xFFFF
		bits -= 1
	return struct.pack('<%dH' % len(d), *d)


def is_pow2(v):
	return v > 0 and (v & (v - 1)) == 0


def decode_tim2(raw):
	"""First picture of an Outbreak TIM2 -> (width, height, bpp, cluts, [RGBA u32 = R | G<<8 | B<<16 | A<<24]).
	4/8-bit indexed with a 16/24/32-bit CLUT; an 8-bit CLUT in CSM1 order has entries 8-15 and 16-23 of every 32
	swapped (OBTool tim2.c); alpha on the PS2's 0-0x80 scale is doubled."""
	need(len(raw) >= 16 and raw[:4] == b'TIM2', 'not TIM2')
	need(struct.unpack_from('<H', raw, 6)[0] >= 1, 'TIM2 without pictures')
	ph = 0x80 if raw[5] == 1 else 0x10
	need(ph + 48 <= len(raw), 'TIM2 picture header')
	total, clut_size, image_size, header_size, clut_colors = struct.unpack_from('<IIIHH', raw, ph)
	mips, clut_type, image_type, w, h = raw[ph + 17], raw[ph + 18], raw[ph + 19], *struct.unpack_from('<HH', raw, ph + 20)
	need(image_type in (4, 5), 'TIM2 image type %d (only 4/8-bit indexed)' % image_type)
	need(is_pow2(w) and is_pow2(h) and w <= MAX_TEXTURE_SIDE and h <= MAX_TEXTURE_SIDE, 'TIM2 size %dx%d' % (w, h))
	need(mips >= 1 and header_size >= 48, 'TIM2 header')
	bpp = 4 if image_type == 4 else 8
	pixels_bytes = w * h // 2 if bpp == 4 else w * h
	img_off = ph + header_size
	clut_off = img_off + image_size
	need(image_size >= pixels_bytes and img_off + pixels_bytes <= len(raw), 'TIM2 pixels')
	entry_bytes = {1: 2, 2: 3, 3: 4}.get(clut_type & 0x3F)
	need(entry_bytes is not None, 'TIM2 CLUT type %#x' % clut_type)
	colours = 16 if bpp == 4 else 256
	need(clut_colors >= colours and clut_off + colours * entry_bytes <= len(raw), 'TIM2 CLUT')
	palette = []
	for i in range(colours):
		e = clut_off + i * entry_bytes
		if entry_bytes == 2:
			v = struct.unpack_from('<H', raw, e)[0]
			r, g, b, a = (v & 0x1F) << 3, ((v >> 5) & 0x1F) << 3, ((v >> 10) & 0x1F) << 3, 0x80 if v & 0x8000 else 0
		elif entry_bytes == 3:
			r, g, b, a = raw[e], raw[e + 1], raw[e + 2], 0x80
		else:
			r, g, b, a = raw[e], raw[e + 1], raw[e + 2], raw[e + 3]
		palette.append([r, g, b, a])
	if bpp == 8 and not (clut_type & 0x80):
		for base in range(0, 256, 32):
			palette[base + 8:base + 16], palette[base + 16:base + 24] = palette[base + 16:base + 24], palette[base + 8:base + 16]
	if max(p[3] for p in palette) <= 0x80:
		for p in palette:
			p[3] = min(255, p[3] * 2)
	packed = [p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24) for p in palette]
	px = raw[img_off:img_off + pixels_bytes]
	if bpp == 8:
		rgba = [packed[i] for i in px]
	else:
		rgba = []
		for byte in px:
			rgba.append(packed[byte & 0xF])
			rgba.append(packed[byte >> 4])
	return w, h, bpp, clut_colors // colours, rgba


def chunk_header(buf, off, end):
	need(off + 12 <= end, 'chunk header past its parent')
	t, c, s = struct.unpack_from('<III', buf, off)
	need(s >= 12 and off + s <= end, 'chunk size %#x at %#x' % (s, off))
	return t, c, s


def parse_strip_list(buf, off, size, count):
	strips = []
	p = off + 12
	end = off + size
	need(count <= MAX_CHUNKS, 'strip count')
	for _ in range(count):
		n = u32(buf, p)
		flag = bool(n & 0x80000000)
		n &= 0x7FFFFFFF
		need(p + 4 + n * 4 <= end, 'strip past its list')
		strips.append((flag, struct.unpack_from('<%dI' % n, buf, p + 4)))
		p += 4 + n * 4
	return strips


def parse_mesh(buf, off, size):
	mesh = {'strips': [], 'matrefs': (), 'stripmats': (), 'pos': (), 'nrm': (), 'uv': (), 'weights': [], 'joints': ()}
	t, count, _ = chunk_header(buf, off, off + size)
	need(t == CHUNK_MESH, 'mesh chunk')
	p = off + 12
	end = off + size
	# The subchunk count can be larger than the chunk holds (File #1 OBJS; libthanatos warns): stop at its size.
	for _ in range(min(count, 64)):
		if p + 12 > end:
			break
		st, sc, ss = chunk_header(buf, p, end)
		if st == CHUNK_PRIMLISTS:
			need(sc <= 2, 'more than two strip lists')
			q = p + 12
			for _l in range(sc):
				lt, lc, ls = chunk_header(buf, q, p + ss)
				need(lt in (CHUNK_STRIP_FW, CHUNK_STRIP_PW), 'strip list type %#x' % lt)
				mesh['strips'].extend(parse_strip_list(buf, q, ls, lc))
				q += ls
		elif st in (CHUNK_MATREFS, CHUNK_STRIPMATS, CHUNK_JOINTREFS):
			need(12 + sc * 4 <= ss, 'int list size')
			vals = struct.unpack_from('<%di' % sc, buf, p + 12)
			mesh[{CHUNK_MATREFS: 'matrefs', CHUNK_STRIPMATS: 'stripmats', CHUNK_JOINTREFS: 'joints'}[st]] = vals
		elif st in (CHUNK_VERTICES, CHUNK_NORMALS):
			need(sc <= MAX_VERTICES and 12 + sc * 12 <= ss, 'vertex list size')
			mesh['pos' if st == CHUNK_VERTICES else 'nrm'] = floats(buf, p + 12, sc * 3)
		elif st == CHUNK_TEXCOORDS:
			need(sc <= MAX_VERTICES and 12 + sc * 8 <= ss, 'texcoord list size')
			mesh['uv'] = floats(buf, p + 12, sc * 2)
		elif st == CHUNK_WEIGHTS:
			need(sc <= MAX_VERTICES, 'weight list size')
			q = p + 12
			for _v in range(sc):
				n = u32(buf, q)
				need(n <= 16 and q + 4 + n * 8 <= p + ss, 'weights past their list')
				best = (0, -1.0)
				for k in range(n):
					slot, w = struct.unpack_from('<If', buf, q + 4 + k * 8)
					if w > best[1]:
						best = (slot, w)
				mesh['weights'].append((best[0], n))
				q += 4 + n * 8
		p += ss
	return mesh


def parse_amo(buf, off, size):
	end = off + size
	t, count, msize = chunk_header(buf, off, end)
	need(t == CHUNK_MODEL, 'AMO model chunk')
	end = off + msize
	meshes, materials, texrefs = [], [], []
	p = off + 12
	for _ in range(min(count, 16)):
		if p + 12 > end:
			break
		ct, cc, cs = chunk_header(buf, p, end)
		need(cc <= MAX_CHUNKS, 'AMO list count')
		if ct == CHUNK_MESHLIST:
			q = p + 12
			for _m in range(cc):
				_, _, ms = chunk_header(buf, q, p + cs)
				meshes.append((q, ms))
				q += ms
		elif ct == CHUNK_MATERIALLIST:
			q = p + 12
			for _m in range(cc):
				mt, _, ms = chunk_header(buf, q, p + cs)
				need(mt in MATERIAL_TYPES and ms >= 12 + MATERIAL_BODY_BYTES, 'material chunk')
				ntex = s32(buf, q + 12 + MATERIAL_BODY_BYTES - 4 - 200)
				need(0 <= ntex <= 16 and ms >= 12 + MATERIAL_BODY_BYTES + ntex * 4, 'material texture count')
				materials.append(struct.unpack_from('<%di' % ntex, buf, q + 12 + MATERIAL_BODY_BYTES))
				q += ms
		elif ct == CHUNK_TEXTURELIST:
			need(12 + cc * TEXREF_BYTES <= cs, 'texture list size')
			for k in range(cc):
				q = p + 12 + k * TEXREF_BYTES
				rt, _, rs = struct.unpack_from('<III', buf, q)
				need(rt == CHUNK_TEXREF and rs == TEXREF_BYTES, 'texture reference')
				texrefs.append(struct.unpack_from('<4i', buf, q + 12))
		p += cs
	return meshes, materials, texrefs


def parse_ahi(buf, off, size):
	end = off + size
	t, count, hsize = chunk_header(buf, off, end)
	need(t == CHUNK_HIERARCHY and count >= 1, 'AHI hierarchy chunk')
	end = off + hsize
	rt, rc, rs = chunk_header(buf, off + 12, end)
	need(rt in (0, 0x80000000) and 12 + rc * 4 <= rs and rc <= MAX_CHUNKS, 'AHI root list')
	roots = struct.unpack_from('<%di' % rc, buf, off + 24)
	nodes = []
	p = off + 12 + rs
	for _ in range(count - 1):
		nt, nc, ns = chunk_header(buf, p, end)
		need(nt in (CHUNK_NODE_PLAIN, CHUNK_NODE_MESH, CHUNK_NODE_JOINT) and ns == NODE_BYTES, 'AHI node')
		nid, parent, child, nxt = struct.unpack_from('<4i', buf, p + 12)
		trs = floats(buf, p + 28, 12)
		mesh_id, group = struct.unpack_from('<2i', buf, p + 76)
		scale, rot = list(trs[0:3]), list(trs[4:7])
		if nt == CHUNK_NODE_JOINT:
			# libthanatos OBNode::read: joints store the rotation negated and the scale as its reciprocal.
			rot = [-r for r in rot]
			scale = [1.0 / s if s != 0.0 else 0.0 for s in scale]
		nodes.append({'parent': parent, 'child': child, 'next': nxt, 'scale': scale, 'rot': rot,
		              'trans': list(trs[8:11]), 'group': group})
		p += ns
	for r in roots:
		need(0 <= r < len(nodes), 'AHI root id')
	return roots, nodes


def sub(a, b):
	return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(a, b):
	return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def dot(a, b):
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def build_model(item_id, mesh, materials, texrefs, ntex, root, nodes, stats):
	"""The item's parts: {(slot, tex): {'verts': [orig index], 'tris': [(a, b, c) local]}} plus counts."""
	pos, nrm, uv = mesh['pos'], mesh['nrm'], mesh['uv']
	nv = len(pos) // 3
	need(nv > 0 and len(nrm) // 3 == nv and len(uv) // 2 == nv, 'per-vertex arrays differ in length')
	weights = mesh['weights']
	need(len(weights) in (0, nv), 'weight list length')
	need(len(mesh['stripmats']) >= len(mesh['strips']), 'strip material list too short')
	for f in pos + nrm + uv:
		need(math.isfinite(f), 'non-finite vertex data')
	need(all(w[0] < MAX_SLOTS for w in weights), 'a weight slot past 15')
	parts = {}
	strip_tris = degenerate = flipped = mixed = 0
	for k, (flag, idx) in enumerate(mesh['strips']):
		mref = mesh['stripmats'][k]
		need(0 <= mref < len(mesh['matrefs']), 'strip material ref')
		mat = mesh['matrefs'][mref]
		need(0 <= mat < len(materials), 'material index')
		tex = -1
		if materials[mat]:
			tref = materials[mat][0]
			need(0 <= tref < len(texrefs), 'material texture ref')
			tex = texrefs[tref][0]
			need(0 <= tex < ntex, 'texture id past the NBD texture list')
		for t in range(len(idx) - 2):
			strip_tris += 1
			a, b, c = (idx[t], idx[t + 1], idx[t + 2]) if (t & 1) == 0 else (idx[t], idx[t + 2], idx[t + 1])
			need(a < nv and b < nv and c < nv, 'strip index out of range')
			if a == b or b == c or a == c:
				degenerate += 1
				continue
			pa, pb, pc = (pos[3 * a:3 * a + 3], pos[3 * b:3 * b + 3], pos[3 * c:3 * c + 3])
			face = cross(sub(pb, pa), sub(pc, pa))
			if dot(face, face) <= 1e-12:
				degenerate += 1
				continue
			# a + (b + c) sums the same whichever way b and c are turned (GameModels.cpp does the same).
			ns = [nrm[3 * a + i] + (nrm[3 * b + i] + nrm[3 * c + i]) for i in range(3)]
			if dot(face, ns) < 0.0:
				b, c = c, b
				flipped += 1
			slots = [weights[v][0] if weights else 0 for v in (a, b, c)]
			if slots[0] != slots[1] or slots[0] != slots[2]:
				mixed += 1
			# The majority keeps a triangle split between slots, else its first vertex.
			slot = slots[1] if slots[1] == slots[2] else slots[0]
			part = parts.setdefault((slot, tex), {'map': {}, 'verts': [], 'tris': []})
			local = []
			for v in (a, b, c):
				if v not in part['map']:
					part['map'][v] = len(part['verts'])
					part['verts'].append(v)
				local.append(part['map'][v])
			part['tris'].append(tuple(local))
	# Each weight slot's pivot: the root node's children in order (child, then next), slot k = child k.
	children = []
	n = nodes[root]['child']
	while 0 <= n < len(nodes) and len(children) < 16:
		children.append(n)
		n = nodes[n]['next']
	kept = sum(len(p['tris']) for p in parts.values())
	need(kept > 0, 'no triangles')
	lo = [min(pos[i::3]) for i in range(3)]
	hi = [max(pos[i::3]) for i in range(3)]
	stats.update(strip_tris=strip_tris, degenerate=degenerate, flipped=flipped, mixed=mixed)
	return {'id': item_id, 'parts': parts, 'tris': kept, 'pivots': [nodes[c] for c in children], 'root': nodes[root],
	        'lo': lo, 'hi': hi, 'joints': mesh['joints'], 'uv': uv}


def load_item_table():
	here = os.path.dirname(os.path.abspath(__file__))
	path = os.path.join(here, '..', '..', 'pcsx2', 'VR', 'ItemData.cpp')
	table = {}
	with open(path, 'r', encoding='utf-8') as f:
		for m in re.finditer(r'\{(\d+), Kind::(\w+), "([^"]*)"\}', f.read()):
			table[int(m.group(1))] = (m.group(2), m.group(3))
	return table


def main():
	ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
	ap.add_argument('image')
	ap.add_argument('--items', action='store_true', help='one line per weapon: id, name, triangles, parts, size')
	args = ap.parse_args()

	items = load_item_table()
	weapon_ids = sorted(i for i, (kind, _) in items.items() if kind in WEAPON_KINDS)
	disc = Disc(args.image)
	serial = '?'
	try:
		cnf_lba, cnf_size = iso_find_root_file(disc, 'SYSTEM.CNF')
		cnf = disc.read(cnf_lba, 1)[:cnf_size].decode('ascii', 'replace')
		m = re.search(r'(S[LC][PEU][SMN])_(\d{3})\.(\d{2})', cnf)
		if m:
			serial = '%s-%s%s' % (m.group(1), m.group(2), m.group(3))
	except ParseError:
		pass
	if serial not in SERIALS:
		print('(VR) Game models: not loaded: serial %s is not Outbreak File #1 or #2.' % serial)
		return 1
	label = 'File #1' if serial == SERIALS[0] else 'File #2'
	# The emulator knows the serial already: count only what it reads too.
	disc.read_bytes = 0
	disc.read_sectors.clear()
	t0 = time.perf_counter()

	dat_lba, dat_size = iso_find_root_file(disc, 'NETBIO00.DAT')
	library = {}
	files_parsed = 0
	files_seen = []
	tex_cache = {}
	lib_bytes = 0
	failures = []
	detail = {}
	for aname, alba, asize in afs_entries(disc, dat_lba, dat_size):
		if not re.fullmatch(r'r[0-9a-f]{3}\.afs', aname, re.I) or asize < SECTOR:
			continue
		try:
			inner = afs_entries(disc, alba, asize)
		except ParseError as e:
			failures.append('%s: %s' % (aname, e))
			continue
		for name, lba, size in inner:
			if not re.fullmatch(r'OBJS\d\d\.NBD', name, re.I):
				continue
			files_seen.append(name)
			try:
				need(0x40 <= size <= MAX_NBD_BYTES, 'NBD size')
				buf = disc.read_range(lba * SECTOR, size)
				ents = {}
				for k in range(4):
					fourcc, off, sz, cnt = struct.unpack_from('<4I', buf, k * 16)
					if fourcc in (FOURCC_TEX, FOURCC_AMO, FOURCC_AHI) and fourcc not in ents:
						need(off >= 0x40 and off + sz <= size, 'NBD entry range')
						ents[fourcc] = (off, sz, cnt)
				need(len(ents) == 3, 'NBD without TEX, AMO and AHI')
				meshes, materials, texrefs = parse_amo(buf, *ents[FOURCC_AMO][:2])
				roots, nodes = parse_ahi(buf, *ents[FOURCC_AHI][:2])
				need(len(roots) == len(meshes), 'roots (%d) and meshes (%d) differ' % (len(roots), len(meshes)))
				# TEX: texCount x (u32 compressed size, data).
				toff, tsize, tcount = ents[FOURCC_TEX]
				need(tcount <= 256, 'TEX count')
				tex_spans = []
				p = toff
				for _ in range(tcount):
					csize = u32(buf, p)
					need(p + 4 + csize <= toff + tsize, 'TEX entry past its block')
					tex_spans.append((p + 4, csize))
					p += 4 + csize
			except ParseError as e:
				failures.append('%s: %s' % (name, e))
				continue
			files_parsed += 1
			for i, root in enumerate(roots):
				item_id = nodes[root]['group']
				if item_id not in weapon_ids or item_id in library:
					continue
				if lib_bytes > MAX_LIBRARY_BYTES:
					failures.append('%s: the library reached %d MB; the rest is left out' % (name, MAX_LIBRARY_BYTES >> 20))
					break
				stats = {}
				try:
					model = build_model(item_id, parse_mesh(buf, *meshes[i]), materials, texrefs, len(tex_spans),
					                    root, nodes, stats)
					textures = {}
					for (slot, tex) in model['parts']:
						if tex < 0 or tex in textures:
							continue
						key = (name, tex)
						if key not in tex_cache:
							s_off, s_size = tex_spans[tex]
							raw = sld_decompress(buf[s_off:s_off + s_size], MAX_TEX_BYTES)
							tex_cache[key] = decode_tim2(raw)
							lib_bytes += tex_cache[key][0] * tex_cache[key][1] * 4
						textures[tex] = tex_cache[key]
				except ParseError as e:
					failures.append('%s item %d: %s' % (name, item_id, e))
					continue
				lib_bytes += sum(len(p['verts']) * 32 + len(p['tris']) * 6 for p in model['parts'].values())
				model['textures'] = textures
				model['file'] = name
				library[item_id] = model
				detail[item_id] = stats
	elapsed_ms = (time.perf_counter() - t0) * 1000.0
	missing = [i for i in weapon_ids if i not in library]

	for f in failures:
		print('(VR) Game models: %s' % f)
	# The emulator's summary line, word for word but for the time (this reads faster or slower than PCSX2's readers).
	print('(VR) Game models: %d weapon(s) from %d file(s) in %.0f ms (%s, %.1f MB read, %.1f MB decoded); missing ids: %s.' % (
		len(library), files_parsed, elapsed_ms, label, disc.read_bytes / 1048576.0, lib_bytes / 1048576.0,
		', '.join(str(i) for i in missing) or 'none'))
	print('  OBJS files: %s' % ', '.join(files_seen))
	print('  %d distinct sectors read (%.2f MB), %d texture page(s) decoded' % (
		len(disc.read_sectors), len(disc.read_sectors) * SECTOR / 1048576.0, len(tex_cache)))

	if args.items:
		for item_id in sorted(library):
			m = library[item_id]
			st = detail[item_id]
			dims = [m['hi'][k] - m['lo'][k] for k in range(3)]
			texs = ' '.join('%dx%d/%dbpp%s' % (t[0], t[1], t[2], '' if t[3] == 1 else '(%d cluts)' % t[3])
			                for _, t in sorted(m['textures'].items()))
			slots = sorted(set(s for s, _ in m['parts']))
			pivots = ' '.join('(%.1f %.1f %.1f)' % tuple(n['trans']) for n in m['pivots'])
			print('  %5d %-24s tris %3d (strips %3d, degenerate %2d, flipped %3d, mixed %d)  parts %d slots %s'
			      ' pivots %s  %.1f x %.1f x %.1f cm  tex %s  [%s]' % (
				item_id, items[item_id][1], m['tris'], st['strip_tris'], st['degenerate'], st['flipped'], st['mixed'],
				len(m['parts']), slots, pivots, dims[0], dims[1], dims[2], texs, m['file']))

	# The self-test's invariants, on this decode: what PCSX2_VR_MODELS_SELFTEST checks.
	problems = []
	for item_id, m in library.items():
		for (slot, tex), p in m['parts'].items():
			nv = len(p['verts'])
			if not all(0 <= i < nv for t in p['tris'] for i in t):
				problems.append('%d: triangle index out of range' % item_id)
			if tex >= 0:
				w, h = m['textures'][tex][0], m['textures'][tex][1]
				if not (is_pow2(w) and is_pow2(h)):
					problems.append('%d: texture %dx%d' % (item_id, w, h))
		if not all(math.isfinite(f) for f in m['uv']):
			problems.append('%d: UV not finite' % item_id)
	for item_id, expect in ((0, 259), (400, 299), (11, 250)):
		if item_id in library:
			st = detail[item_id]
			print('  check: %s strip triangles %d (research %d), kept %d after %d degenerate' % (
				items[item_id][1], st['strip_tris'], expect, library[item_id]['tris'], st['degenerate']))
	print('  invariants: %s' % ('all hold' if not problems else '; '.join(problems[:8])))
	return 0 if not problems and not failures else 2


if __name__ == '__main__':
	sys.exit(main())
