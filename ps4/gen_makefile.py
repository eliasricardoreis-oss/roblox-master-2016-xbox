#!/usr/bin/env python3
"""
gen_makefile.py - gera automaticamente o Makefile (OpenOrbis PS4) da engine.

Uso:
  python3 ps4/gen_makefile.py [--root .] [--out ps4] [--probe] [--title "Roblox 2016 PS4"]
                              [--title-id RBLX00001] [--modules zlib,g3d,log,base]

O que faz:
  1. Procura cada modulo da engine (zlib, g3d, Log, Base, CSG, Bullet, Network...)
     pelo arquivo-marcador, em qualquer lugar do repo (zips extraidos ou nao).
  2. Monta a lista de includes (-I) sozinho (inclui G3D -> g3d, boost/tbb se existirem).
  3. Com --probe, compila cada .c/.cpp de teste com o clang no alvo PS4 e
     MANTEM no Makefile so os arquivos que compilam (os outros viram comentario).
  4. Escreve ps4/Makefile (+ ps4/engine_modules.h com o que foi ligado).
"""
import argparse, os, re, subprocess, sys, shutil, tempfile
from concurrent.futures import ThreadPoolExecutor

SKIP_DIRS = re.compile(r'/(Win|win|win32|Win32|Xbox|Xbox360|Mac|Darwin|Android|iOS|Apple|Mobile|MSVC|Tests?|test|XCode|\.git|ps4|build_engine|obj)(/|$)', re.I)
SKIP_FILES = re.compile(r'(d3d|directx|\.xcodeproj|Win32|WinMain|DllMain|gzlib_win)', re.I)

# nome -> (arquivo-marcador, pasta-extra de include relativa ao modulo)
MODULES = {
    'zlib':   ('deflate.c',            []),
    'g3d':    ('Vector3.cpp',          ['include', '.']),
    'log':    ('FastLog.cpp',          ['include']),
    'base':   ('RbxAssert.cpp',        ['include']),
    'csg':    ('CSGKernel.cpp',        ['.']),
    'bullet': ('btDbvt.cpp',           ['.']),
    'network':('ClientReplicator.cpp', ['.']),
}
# modulos ligados por padrao no app (os demais so entram com --modules)
DEFAULT_LINK = ['zlib', 'bullet', 'g3d', 'log', 'base']

ROOT_DIR = '.'

def find_dirs(root, marker):
    hits = []
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if d not in ('.git', 'ps4', 'build_engine', 'obj', 'boost', 'libs', 'tools')]
        if marker in fn:
            hits.append(dp)
    return hits

def find_module(root, name):
    marker, _ = MODULES[name]
    hits = find_dirs(root, marker)
    if name == 'zlib':
        hits = [h for h in hits if os.path.exists(os.path.join(h, 'zlib.h')) and os.path.exists(os.path.join(h, 'crc32.c'))]
    if name == 'g3d':
        # Vector3.cpp fica em g3dcpp/; o include real e a pasta pai
        hits = [h for h in hits if os.path.basename(h) == 'g3dcpp'] or hits
    if name == 'bullet':
        # raiz do modulo = pasta que tem BulletCollision/ e LinearMath/
        hits = [os.path.dirname(os.path.dirname(h)) for h in hits]
    return sorted(set(hits), key=len)[0] if hits else None

def module_sources(name, d):
    out = []
    for dp, dn, fn in os.walk(d):
        rel = '/' + os.path.relpath(dp, d).replace('\\', '/') + '/'
        if SKIP_DIRS.search(rel):
            continue
        for f in fn:
            if f.endswith(('.c', '.cpp', '.cc')) and not SKIP_FILES.search(f):
                out.append(os.path.join(dp, f))
    return sorted(out)

def collect_includes(root, mods):
    inc = [root, os.path.join(root, 'ps4', 'compat')]
    for name, d in mods.items():
        inc.append(d)
        inc.append(os.path.dirname(d))
        inc.append(os.path.join(os.path.dirname(d), 'include'))
        for extra in MODULES[name][1]:
            inc.append(os.path.normpath(os.path.join(d, extra)))
        for dp, dn, fn in os.walk(d):
            if os.path.basename(dp) in ('include', 'ijg', 'gl', 'png', 'zip', 'Source'):
                inc.append(dp)
            dn[:] = [x for x in dn if x != '.git']
    # boost / tbb se estiverem no repo
    for dp, dn, fn in os.walk(root):
        dn[:] = [x for x in dn if x not in ('.git', 'ps4', 'build_engine', 'obj')]
        if os.path.basename(dp) == 'boost':
            if 'version.hpp' in fn: inc.append(os.path.dirname(dp))
            dn[:] = []
        if os.path.basename(dp) == 'tbb' and 'tbb.h' in fn:
            inc.append(os.path.dirname(dp))
    seen, res = set(), []
    for i in inc:
        i = os.path.normpath(i)
        if i not in seen:
            seen.add(i); res.append(i)
    return res

def fix_g3d_case(root, mods):
    """Codigo inclui <G3D/...> mas a pasta e 'g3d' (Windows -> Linux)."""
    d = mods.get('g3d')
    if not d: return
    for cand in (os.path.dirname(d), d):
        g = os.path.join(cand, 'include', 'g3d')
        if os.path.isdir(g):
            link = os.path.join(cand, 'include', 'G3D')
            if not os.path.exists(link):
                os.symlink('g3d', link)

def probe(tc, flags_c, flags_cxx, incs, files, jobs):
    ok, bad = [], []
    tmp = tempfile.mkdtemp()
    def one(f):
        cxx = not f.endswith('.c')
        cmd = ['clang++' if cxx else 'clang', '--target=x86_64-pc-freebsd12-elf', '-fPIC',
               '-funwind-tables', '-c', '-w', '-DZ_HAVE_UNISTD_H', '-include', os.path.join(ROOT_DIR, 'ps4', 'compat', 'ps4_compat.h'),
               '-isysroot', tc, '-isystem', tc + '/include']
        cmd += (['-std=gnu++11', '-isystem', tc + '/include/c++/v1'] if cxx else ['-std=gnu99'])
        cmd += ['-I' + i for i in incs] + [f, '-o', os.path.join(tmp, str(abs(hash(f))) + '.o')]
        try:
            r = subprocess.run(cmd, capture_output=True, timeout=180)
            return f, r.returncode == 0
        except Exception:
            return f, False
    with ThreadPoolExecutor(jobs) as ex:
        for f, good in ex.map(one, files):
            (ok if good else bad).append(f)
    shutil.rmtree(tmp, ignore_errors=True)
    return ok, bad

MAKEFILE = r'''# =====================================================================
#  Makefile GERADO por ps4/gen_makefile.py  -  nao edite a mao
#  Rode:  python3 ps4/gen_makefile.py --probe   para regenerar
# =====================================================================
.RECIPEPREFIX := >

TITLE      ?= @TITLE@
TITLE_ID   ?= @TITLE_ID@
CONTENT_ID ?= IV0000-$(TITLE_ID)_00-ROBLOXPS4ENGINE0
VERSION    ?= 1.00
EXTRAFLAGS ?= -O2 -DNDEBUG

OO_PS4_TOOLCHAIN ?= $(error defina OO_PS4_TOOLCHAIN=/caminho/do/OpenOrbis/PS4Toolchain)
TC     := $(OO_PS4_TOOLCHAIN)
BIN    := $(TC)/bin/linux
ROOT   := @ROOT@
OBJDIR := obj

INCS := @INCS@

CFLAGS   := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c -w -DZ_HAVE_UNISTD_H -include $(ROOT)/ps4/compat/ps4_compat.h $(EXTRAFLAGS) -isysroot $(TC) -isystem $(TC)/include $(INCS)
CXXFLAGS := $(CFLAGS) -std=gnu++11 -isystem $(TC)/include/c++/v1

# ---- fontes do app (ps4/src) ----
APP_SRC := $(wildcard src/*.cpp) $(wildcard src/*.c)
APP_OBJ := $(patsubst src/%.cpp,$(OBJDIR)/app/%.o,$(filter %.cpp,$(APP_SRC))) $(patsubst src/%.c,$(OBJDIR)/app/%.o,$(filter %.c,$(APP_SRC)))

@MODBLOCKS@
ENGINE_LIBS := @ENGINELIBS@

LIBS    := --start-group $(ENGINE_LIBS) --end-group -lc -lkernel -lc++ -lSceVideoOut -lScePad -lSceUserService -lSceAudioOut
LDFLAGS := -m elf_x86_64 -pie --script $(TC)/link.x --eh-frame-hdr -L$(TC)/lib

PKGFILES := eboot.bin sce_sys/param.sfo sce_sys/icon0.png $(wildcard sce_sys/about/right.sprx) $(wildcard sce_module/*.prx)

all: $(CONTENT_ID).pkg

$(OBJDIR)/app/%.o: src/%.cpp
> @mkdir -p $(dir $@)
> clang++ $(CXXFLAGS) -DENGINE_MODULES_H -o $@ $<
$(OBJDIR)/app/%.o: src/%.c
> @mkdir -p $(dir $@)
> clang $(CFLAGS) -std=gnu99 -o $@ $<

app.elf: $(APP_OBJ) $(ENGINE_LIBS)
> ld.lld $(LDFLAGS) $(TC)/lib/crt1.o $(APP_OBJ) $(LIBS) -o $@

eboot.bin: app.elf
> $(BIN)/create-fself -in=app.elf -out=app.oelf --eboot "eboot.bin" --paid 0x3800000000000011

sce_sys/param.sfo: Makefile
> @mkdir -p sce_sys
> rm -f $@
> $(BIN)/PkgTool.Core sfo_new $@
> $(BIN)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
> $(BIN)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
> $(BIN)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
> $(BIN)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
> $(BIN)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
> $(BIN)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
> $(BIN)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
> $(BIN)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
> $(BIN)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
> $(BIN)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

$(CONTENT_ID).pkg: eboot.bin sce_sys/param.sfo sce_sys/icon0.png
> $(BIN)/create-gp4 -out pkg.gp4 --content-id=$(CONTENT_ID) --files "$(PKGFILES)"
> $(BIN)/PkgTool.Core pkg_build pkg.gp4 .

elf: app.elf
clean:
> rm -rf $(OBJDIR) app.elf app.oelf eboot.bin pkg.gp4 sce_sys/param.sfo *.pkg
regen:
> python3 gen_makefile.py --probe

.PHONY: all elf clean regen
'''



def apply_patches(root):
    """Patches idempotentes no codigo da engine para o alvo PS4 (so onde o original nao conhece a plataforma)."""
    n = 0
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if d not in ('.git', 'ps4', 'build_engine', 'obj', 'boost', 'contribs')]
        if 'atomic.h' in fn and os.path.basename(dp) == 'rbx':
            f = os.path.join(dp, 'atomic.h')
            t = open(f, encoding='utf-8', errors='ignore').read()
            old = '#if defined(RBX_PLATFORM_IOS) || defined(__APPLE__) || __ANDROID__'
            if old in t and 'RBX_PLATFORM_PS4' not in t:
                open(f, 'w', encoding='utf-8').write(t.replace(old, old + ' || defined(RBX_PLATFORM_PS4)', 1))
                n += 1
    # pthread_t no OpenOrbis e inteiro (unsigned long): reinterpret_cast<long>(pthread_self()) nao compila
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if d not in ('.git', 'ps4', 'build_engine', 'obj', 'boost', 'contribs')]
        if 'Boost.hpp' in fn and os.path.basename(dp) == 'rbx':
            f = os.path.join(dp, 'Boost.hpp')
            t = open(f, encoding='utf-8', errors='ignore').read()
            if 'reinterpret_cast<long>(pthread_self())' in t:
                open(f, 'w', encoding='utf-8').write(t.replace('reinterpret_cast<long>(pthread_self())', '(long)(pthread_self())'))
                print('[gen] patch: GetCurrentThreadId (pthread_t inteiro no PS4)')
    # Windows e case-insensitive: o codigo inclui <rbx/boost.hpp> mas o arquivo e Boost.hpp -> symlinks minusculos
    k = 0
    for dp, dn, fn in os.walk(root):
        dn[:] = [d for d in dn if d not in ('.git', 'ps4', 'build_engine', 'obj', 'boost', 'contribs')]
        if os.path.basename(dp) == 'rbx':
            for f in fn:
                low = f.lower()
                if low != f and not os.path.exists(os.path.join(dp, low)):
                    os.symlink(f, os.path.join(dp, low)); k += 1
    if k: print(f'[gen] {k} symlink(s) de case criados em rbx/')
    if n: print(f'[gen] patch: atomic.h -> PS4')

def prepare_assets(out, tc):
    """icon0.png (gerado), libc.prx/libSceFios2.prx/right.sprx (copiados do toolchain)."""
    import zlib, struct
    os.makedirs(os.path.join(out, 'sce_sys', 'about'), exist_ok=True)
    os.makedirs(os.path.join(out, 'sce_module'), exist_ok=True)
    icon = os.path.join(out, 'sce_sys', 'icon0.png')
    if not os.path.exists(icon):
        W = H = 512
        BLUE, WHITE = (0, 162, 255, 255), (255, 255, 255, 255)
        raw = bytearray()
        for y in range(H):
            raw.append(0)
            for x in range(W):
                if 150 <= x < 362 and 150 <= y < 362:
                    raw.extend(BLUE if (205 <= x < 307 and 205 <= y < 307) else WHITE)
                else:
                    raw.extend(BLUE)
        def chunk(t, d):
            c = struct.pack('>I', len(d)) + t + d
            return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
        open(icon, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
        print('[gen] icon0.png criado')
    if not tc:
        return
    def find(name):
        for dp, dn, fn in os.walk(tc):
            if name in fn: return os.path.join(dp, name)
    for name, dst in (('libc.prx', 'sce_module'), ('libSceFios2.prx', 'sce_module'), ('right.sprx', os.path.join('sce_sys', 'about'))):
        src = find(name)
        if src: shutil.copy(src, os.path.join(out, dst, name))
        else: print('[gen] AVISO:', name, 'nao encontrado no toolchain')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--out', default='ps4')
    ap.add_argument('--probe', action='store_true')
    ap.add_argument('--title', default='Roblox 2016 PS4')
    ap.add_argument('--title-id', default='RBLX00001')
    ap.add_argument('--modules', default=','.join(DEFAULT_LINK))
    ap.add_argument('--toolchain', default=os.environ.get('OO_PS4_TOOLCHAIN', ''))
    ap.add_argument('-j', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    global ROOT_DIR
    root = os.path.abspath(a.root)
    ROOT_DIR = root
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    os.makedirs(os.path.join(out, 'src'), exist_ok=True)
    prepare_assets(out, a.toolchain)
    want = [m for m in a.modules.split(',') if m]

    found = {}
    for n in MODULES:
        d = find_module(root, n)
        if d: found[n] = d
        print(f'[gen] {n:8s} -> {os.path.relpath(d, root) if d else "NAO ENCONTRADO"}')
    apply_patches(root)
    fix_g3d_case(root, found)
    incs = collect_includes(root, found)

    rel = lambda p: os.path.relpath(p, out).replace('\\', '/')
    srcs = {}
    for n in want:
        if n not in found: continue
        files = module_sources(n, found[n])
        if a.probe:
            if not a.toolchain:
                sys.exit('--probe precisa de OO_PS4_TOOLCHAIN ou --toolchain')
            ok, bad = probe(a.toolchain, None, None, incs, files, a.j)
            print(f'[probe] {n}: {len(ok)} OK, {len(bad)} falharam')
            srcs[n] = (ok, bad)
        else:
            srcs[n] = (files, [])

    blocks, libs = [], []
    for n, (ok, bad) in srcs.items():
        if not ok: continue
        var = n.upper()
        lines = [f'# ---- modulo {n} ({len(ok)} arquivos) ----', f'{var}_SRC := \\']
        lines += [f'  $(ROOT)/{os.path.relpath(f, root).replace(os.sep, "/")} \\' for f in ok]
        lines[-1] = lines[-1].rstrip(' \\')
        lines += [
            f'{var}_OBJ := $(patsubst $(ROOT)/%,$(OBJDIR)/{n}/%.o,$({var}_SRC))',
            f'$(OBJDIR)/{n}/%.c.o: $(ROOT)/%.c',
            '> @mkdir -p $(dir $@)',
            '> clang $(CFLAGS) -std=gnu99 -o $@ $<',
            f'$(OBJDIR)/{n}/%.cpp.o: $(ROOT)/%.cpp',
            '> @mkdir -p $(dir $@)',
            '> clang++ $(CXXFLAGS) -o $@ $<',
            f'$(OBJDIR)/lib{n}ps4.a: $({var}_OBJ)',
            '> rm -f $@',
            '> ar rcs $@ $^',
        ]
        if bad:
            lines.append('# pulados (nao compilam no PS4): ' + ', '.join(os.path.basename(b) for b in bad[:25]) + (' ...' if len(bad) > 25 else ''))
        blocks.append('\n'.join(lines) + '\n')
        libs.append(f'$(OBJDIR)/lib{n}ps4.a')

    mk = (MAKEFILE.replace('@TITLE@', a.title).replace('@TITLE_ID@', a.title_id)
          .replace('@ROOT@', rel(root) if rel(root) != '.' else '.')
          .replace('@INCS@', ' '.join('-I$(ROOT)/' + os.path.relpath(i, root).replace(os.sep, '/') if i != root else '-I$(ROOT)' for i in incs))
          .replace('@MODBLOCKS@', '\n'.join(blocks)).replace('@ENGINELIBS@', ' '.join(libs)))
    open(os.path.join(out, 'Makefile'), 'w').write(mk)

    with open(os.path.join(out, 'src', 'engine_modules.h'), 'w') if os.path.isdir(os.path.join(out, 'src')) else open(os.devnull, 'w') as h:
        h.write('/* GERADO por gen_makefile.py */\n#pragma once\n')
        for n in MODULES:
            h.write(f'#define ENGINE_HAS_{n.upper()} {1 if n in srcs and srcs[n][0] else 0}\n')
        h.write('static const char *const ENGINE_REPORT[] = {\n')
        for n in want:
            ok, bad = srcs.get(n, ([], []))
            h.write(f'  "{n.upper()} {len(ok)} OK {len(bad)} FALHA",\n')
        h.write('};\nstatic const int ENGINE_REPORT_N = %d;\n' % len(want))
    print('[gen] Makefile escrito em', os.path.join(out, 'Makefile'))

if __name__ == '__main__':
    main()
