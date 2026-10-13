# Scratch harness only (branch meshraster-quick): breaks MeshRaster.cpp one way at a time and checks that its self-test
# notices (exit code or crash), so the self-test is known to be able to fail.
import os
import subprocess
import sys

MUTATIONS = [
    ("no top-left bias", "return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1;", "return 0;"),
    ("every edge exclusive", "return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1;", "return -1;"),
    ("UVs not perspective-correct", "const float rw = 1.0f / (iw_row + piw.dx * fx);", "const float rw = 1.0f / iw_row;"),
    ("depth test reversed", "if (!(zf < drow[x]))", "if (!(zf > drow[x]))"),
    ("depth never written", "drow[x] = zf;", ""),
    ("no clipping", "if ((any & kClipMask) != 0)", "if (false)"),
    ("no guard-band clipping", "constexpr u32 kClipMask = kNear | kFar | kGuardLeft | kGuardRight | kGuardBottom | kGuardTop;", "constexpr u32 kClipMask = kNear | kFar;"),
    ("culling inverted", "const bool facing_away = (tri_det * front_sign) < 0.0f;", "const bool facing_away = (tri_det * front_sign) > 0.0f;"),
    ("mirror ignored", "const float mirror = (det < 0.0f) ? -1.0f : 1.0f;", "const float mirror = 1.0f;"),
    ("no alpha test", "if (static_cast<s32>(texel >> 24) < ss.cutoff)", "if (false)"),
    ("v flipped", "return s.texels[static_cast<size_t>(y) * s.tex_w + x];", "return s.texels[static_cast<size_t>(s.tex_h - 1 - y) * s.tex_w + x];"),
    ("bilinear weights wrong", "const u32 w10 = fx - w11;", "const u32 w10 = fx;"),
    ("world light mirrored", "ds.light[0] = ld[0];", "ds.light[0] = -ld[0];"),
    ("ambient ignored", "ds.ambient = IsFinite(lt.ambient) ? lt.ambient : 0.0f;", "ds.ambient = 0.0f;"),
    ("partial clear leaves depth", "std::fill(depth_row + m_dirty.x0, depth_row + m_dirty.x1, 1.0f);", "(void)depth_row;"),
    ("resolve drops alpha", "ag = ((ag + rounding) >> shift) & 0x00FF00FFu;", "ag = 0u;"),
    ("impostor camera on-axis", "(-half - ec_r) / de, (half - ec_r) / de", "(-half) / de, (half) / de"),
    ("fan skips a triangle", "for (int k = 1; k + 1 < count; k++)", "for (int k = 1; k + 2 < count; k++)"),
    ("front_ccw ignored", "const float front_sign = (mesh.front_ccw ? 1.0f : -1.0f) * mirror;", "const float front_sign = mirror;"),
    ("linear light ignored", "return ds.linear ? std::sqrt(l) : l;", "return l;"),
    ("base colour ignored", "ss.base[i] = Q256(material.base_colour[i]);", "ss.base[i] = 256u;"),
    ("rim ignored", "ss.rim_on = ds.lit && ds.rim > 0.0f;", "ss.rim_on = false;"),
    ("repeat taken as clamp", "ss.repeat_u = (tex->wrap_u == Texture::Wrap::Repeat);", "ss.repeat_u = false;"),
    ("bilinear without the half texel", "const s64 fu = TexelFixed(u, s.u_scale) - kSub / 2;", "const s64 fu = TexelFixed(u, s.u_scale);"),
    ("every texture taken as power of two", "ss.pot = (tex->width & (tex->width - 1)) == 0 && (tex->height & (tex->height - 1)) == 0;", "ss.pot = true;"),
    ("dirty rectangle a column short", "static_cast<u32>(fc.bounds.x1) + 1u, static_cast<u32>(fc.bounds.y1) + 1u", "static_cast<u32>(fc.bounds.x1), static_cast<u32>(fc.bounds.y1) + 1u"),
    ("two-sided light not flipped", "o.lit_b = LightFactor(ds, -ndl, -ndv);", "o.lit_b = LightFactor(ds, ndl, ndv);"),
    ("rim colour ignored", "ss.rim_colour[i] = Q256(lt.rim_colour[i]);", "ss.rim_colour[i] = 256u;"),
]


def main():
    src = open("pcsx2/VR/MeshRaster.cpp", encoding="utf-8").read()
    os.makedirs("out/mut", exist_ok=True)
    survived = []
    for index, (name, old, new) in enumerate(MUTATIONS):
        if old not in src:
            print("MUTATION %d (%s): the text to change is not in the source" % (index, name))
            survived.append(name + " (not applied)")
            continue
        directory = "out/mut/%d" % index
        os.makedirs(directory, exist_ok=True)
        path = directory + "/MeshRaster.cpp"
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(src.replace(old, new, 1))
        exe = directory + "/mut.exe"
        build = subprocess.run(
            ["cl", "/nologo", "/std:c++20", "/O2", "/Zc:__cplusplus", "/permissive-", "/Zc:preprocessor", "/utf-8", "/w", "/D_HAS_EXCEPTIONS=0",
             "/D_M_X86", "/D_CRT_SECURE_NO_WARNINGS", "/I", "tools/meshraster-quick/stub", "/I", ".", "/I", "pcsx2", path,
             "tools/meshraster-quick/main.cpp", "/Fe:" + exe, "/Fo:" + directory + "/"],
            capture_output=True, text=True)
        if build.returncode != 0:
            print("MUTATION %d (%s): does not compile:\n%s" % (index, name, build.stdout[-1500:]))
            survived.append(name + " (did not compile)")
            continue
        try:
            run = subprocess.run([exe, "selftest-only"], capture_output=True, text=True, timeout=120)
            code = run.returncode
            output = run.stdout
        except subprocess.TimeoutExpired:
            code = -1
            output = "timed out"
        fails = [line for line in output.splitlines() if "FAIL" in line]
        if code == 0:
            print("MUTATION %d (%s): SURVIVED (self-test passed)" % (index, name))
            survived.append(name)
        else:
            print("MUTATION %d (%s): killed (exit %d, %d failing checks) e.g. %s" % (index, name, code, len(fails), fails[0] if fails else output[-200:].strip()))
        sys.stdout.flush()
    print("survivors: %s" % (survived if survived else "none"))
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
