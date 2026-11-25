#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse
import os
import re
import struct
import subprocess
import sys
IQOO_DTS_ENV = 'PATCH_DTBO_IQOO_DTS'
IQOO_DTS_NAMES = ('dts.2', 'dts', 'iqoo-dtbo.dts', 'iqoo.dts')
IQOO_DTS_MARKER = 'qti_ram_effects'
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
NODE = 'qcom,hv-haptics@f000'
CHILDREN = ('lra_scene_array', 'qti_ram_effects', 'qti_minor_rtp_effects')
REFS = ('scene_array', 'ram_effects', 'rtp_effects')
VIVO_PROPS = (
    ('lra_info', '0x257b'),
    ('vib_num', '0x01'),
    ('is_major_motor', None),
    ('richtap_type', '0x01'),
    ('resistance_min', '0x1388'),
    ('resistance_max', '0x4e20'),
    ('freq_min', '0x640'),            # 160.0 Hz == fuxi qcom,lra-f0-min
    ('freq_max', '0x73a'),            # 185.0 Hz == fuxi qcom,lra-f0-max
)
FALLBACK_CAP = 0xe10                  # fuxis default effect vmax (3600 mV)

# iQOO 11's DT only names 6 ringtone waveforms. iQOO 13 names every all.
IQOO13_ENV = 'PATCH_DTBO_IQOO13_DTS'
IQOO13_MARKER = 'qti_rtp_effects'
BAND_RANGES = {                       # effect-id ranges per waveform family
    'R': (301, 340),                  # ringtones
    'N': (401, 448),                  # notifications
    'A': (501, 518),                  # alarms
    'T': (690, 691),                  # long vibrate
    'S': (9001, 28003),               # system
    'G': (601, 26014),                # games
}
SCENE_FILL = ((89, 3, 3000), (90, 3, 3300))   # sound-vibration ladder gaps

def looks_like_iqoo_dts(path):
    try:
        with open(path, encoding='utf-8', errors='replace') as f:
            txt = f.read()
    except OSError:
        return False
    return IQOO_DTS_MARKER in txt and 'lra_scene_array' in txt
def find_iqoo_dts(stock):
    tried = []
    env = os.environ.get(IQOO_DTS_ENV)
    if env:
        if looks_like_iqoo_dts(env):
            return env
        tried.append(env + ' (from $%s, no %s)' % (IQOO_DTS_ENV, IQOO_DTS_MARKER))
    dirs = []
    for d in (os.path.dirname(os.path.abspath(stock)), os.getcwd(), SCRIPT_DIR,
              os.path.join(SCRIPT_DIR, os.pardir),
              os.path.join(SCRIPT_DIR, os.pardir, 'fuxi'),
              os.path.join(SCRIPT_DIR, os.pardir, 'bac')):
        d = os.path.normpath(d)
        if d not in dirs:
            dirs.append(d)
    for d in dirs:
        for n in IQOO_DTS_NAMES:
            p = os.path.join(d, n)
            if os.path.isfile(p):
                if looks_like_iqoo_dts(p):
                    return p
                tried.append(p + ' (no %s)' % IQOO_DTS_MARKER)
    raise SystemExit(
        'no iQOO decompiled dtbo dts found pass --iqoo-dts PATH or set $%s.\n'
        '  looked for [%s] in:\n    %s%s'
        % (IQOO_DTS_ENV, ', '.join(IQOO_DTS_NAMES), '\n    '.join(dirs),
           ('\n  rejected:\n    ' + '\n    '.join(tried)) if tried else ''))
def name_re(name):
    return re.compile(r'(?m)^(\t*)' + re.escape(name) + r' \{$')
def find_all_nodes(txt, name):
    found = []
    for m in name_re(name).finditer(txt):
        o = txt.index('{', m.start())
        depth = 0
        for j in range(o, len(txt)):
            if txt[j] == '{':
                depth += 1
            elif txt[j] == '}':
                depth -= 1
                if depth == 0:
                    found.append((m.start(), m.group(1), o, j))
                    break
    return found
def find_node(txt, name, start=0):
    for s, ind, o, c in find_all_nodes(txt, name):
        if s >= start:
            return ind, o, c
    raise ValueError('node not found: ' + name)
def drop_nodes(txt, names):
    for n in names:
        while True:
            try:
                s, ind, o, c = find_node(txt, n)
            except ValueError:
                break
            e = c + 1
            if txt[e:e + 1] == ';':
                e += 1
            while txt[e:e + 1] in ('\n', '\t'):
                e += 1
                if txt[e - 1] == '\n':
                    break
            st = txt.rfind('\n', 0, s) + 1
            txt = txt[:st] + txt[e:]
    return txt
def reindent(body, indent):
    src = [l for l in body.splitlines() if l.strip()]
    if not src:
        return ''
    base = min(len(l) - len(l.lstrip('\t')) for l in src)
    out = []
    for line in src:
        depth = len(line) - len(line.lstrip('\t')) - base
        out.append('\n' + indent + '\t' * (depth + 1) + line.strip())
    return ''.join(out)
def iqoo_node(text, name):
    ind, o, c = find_node(text, name)
    body = text[o + 1:c]
    body = '\n'.join(l for l in body.splitlines() if 'phandle' not in l)
    return body.strip('\n')
def clamp_vmax(body, cap):
    n = [0]
    def f(m):
        v = int(m.group(1), 0)
        if v > cap:
            n[0] += 1
            return 'qcom,wf-vmax-mv = <%#x>' % cap
        return m.group(0)
    return re.sub(r'qcom,wf-vmax-mv = <(0x[0-9a-fA-F]+|\d+)>', f, body), n[0]
def strip_props(body):
    names = '(' + '|'.join(re.escape(n) for n, _ in VIVO_PROPS) + '|' + '|'.join(REFS) + ')'
    return re.sub(r'(?m)^\t*' + names + r'( = <[^>]*>)?;\n?', '', body)
def decompile(blob):
    r = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts'], input=blob, capture_output=True)
    if r.returncode:
        raise SystemExit('dtc -I dtb failed: ' + r.stderr.decode()[:300])
    return r.stdout.decode()
def compile_dts(txt):
    return subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb'], input=txt.encode(), capture_output=True)
def entries(img):
    magic, total, hsize, esize, ecount, eoff, pgsz, ver = struct.unpack_from('>8I', img, 0)
    if magic != 0xd7b7ab1e:
        raise SystemExit('not a dtbo image (magic %#x)' % magic)
    hdr = dict(magic=magic, total=total, hsize=hsize, esize=esize,
               ecount=ecount, eoff=eoff, pgsz=pgsz, ver=ver)
    for i in range(ecount):
        e = struct.unpack_from('>4I', img, eoff + i * esize)
        yield i, e, bytes(img[e[1]:e[1] + e[0]])
def header(img):
    f = struct.unpack_from('>8I', img, 0)
    return dict(zip(('magic', 'total', 'hsize', 'esize', 'ecount', 'eoff', 'pgsz', 'ver'), f))
def entry_table(img, i):
    h = header(img)
    return struct.unpack_from('>4I', img, h['eoff'] + i * h['esize'])
def patch_dts(txt, scene, ram, rtp, stats):
    fixups_at = txt.find('__local_fixups__ {')
    nodes = [n for n in find_all_nodes(txt, NODE) if n[0] != fixups_at]
    for s, ind, o, c in reversed(nodes):
        body = txt[o + 1:c]
        if 'compatible = "qcom,hv-haptics"' not in body:
            continue
        caps = [int(x, 16) for x in re.findall(r'qcom,wf-vmax-mv = <(?:0x)?([0-9a-fA-F]+)>', body)]
        cap = max(caps) if caps else FALLBACK_CAP
        vm = re.search(r'qcom,vmax-mv = <(?:0x)?([0-9a-fA-F]+)>', body)
        vmax = vm.group(1) if vm else None
        used = [int(x, 16) for x in re.findall(r'phandle = <(?:0x)?([0-9a-fA-F]+)>', txt)]
        base = (max(used) + 1) if used else 1
        ph = (base, base + 1, base + 2)
        r_body, n1 = clamp_vmax(ram, cap)
        t_body, n2 = clamp_vmax(rtp, cap)
        clean = strip_props(body)
        clean = drop_nodes(clean, CHILDREN).rstrip('\n')
        ci = ind + '\t'
        props = ''
        for name, val in VIVO_PROPS:
            props += '\n%s%s%s;' % (ci, name, ' = <%s>' % val if val else '')
        for name, p in zip(REFS, ph):
            props += '\n%s%s = <%#x>;' % (ci, name, p)
        m = re.search(r'(?m)^\t*[^\n;{}]+ \{\s*$', clean)
        if m:
            head, kids = clean[:m.start()], clean[m.start():].rstrip('\n')
        else:
            head, kids = clean, ''
        tables = ''
        for name, p, sub in (('lra_scene_array', ph[0], scene),
                             ('qti_ram_effects', ph[1], r_body),
                             ('qti_minor_rtp_effects', ph[2], t_body)):
            tables += '\n%s%s {\n%s\tphandle = <%#x>;%s\n%s};' % (
                ci, name, ci, p, reindent(sub, ci), ci)
        txt = txt[:o + 1] + head + props + '\n' + kids + tables + '\n' + ind + txt[c:]
        stats['entries'].append(dict(cap=cap, vmax=vmax, ph=ph,
                                     clamped=n1 + n2,
                                     src_caps=sorted(set(caps)) if caps else []))
    fixups_at = txt.find('__local_fixups__ {')
    if fixups_at >= 0:
        try:
            ind, o, c = find_node(txt, NODE, fixups_at)
        except ValueError:
            ind = o = c = None
        if o is not None:
            body = txt[o + 1:c]
            body = strip_props(body).rstrip('\n')
            ci = ind + '\t'
            add = ''.join('\n%s%s = <0x00>;' % (ci, r) for r in REFS)
            if 'nvmem = <0x00>;' in body:
                body = re.sub(r'(?m)^(\t*)nvmem = <0x00>;$',
                              lambda m: m.group(0) + add, body, count=1)
            else:
                stats['nofixup_nvmem'] = True
                body += add
            txt = txt[:o + 1] + body + '\n' + ind + txt[c:]

    return txt
def donor_rtp(path):
    """{effect_id: (bin_name, rtp_len)} from an iQOO 13 style qti_rtp_effects."""
    txt = open(path, encoding='utf-8', errors='replace').read()
    try:
        _, o, c = find_node(txt, 'qti_rtp_effects')
    except ValueError:
        _, o, c = find_node(txt, 'qti_minor_rtp_effects')
    out = {}
    for m in re.finditer(r'effect_(\d+)\s*\{(.*?)\n\s*\};', txt[o + 1:c], re.S):
        eid, blk = int(m.group(1)), m.group(2)
        n = re.search(r'qcom,rtp-name = "([^"]+)"', blk)
        l = re.search(r'qcom,rtp-len = <(0x[0-9a-fA-F]+|\d+)>', blk)
        if n and l:
            out[eid] = (n.group(1), int(l.group(1), 0))
    return out


def select_bands(donor, bands):
    want = set(x.strip() for x in bands.split(',') if x.strip())
    sel = {}
    for eid, (name, ln) in donor.items():
        fam = name.split('_')[0]
        if fam not in want:
            continue
        rng = BAND_RANGES.get(fam)
        if rng and not (rng[0] <= eid <= rng[1]):
            continue
        sel[eid] = (name, ln)
    return sel


def add_rtp_blocks(txt, sel, cap):
    """Append any donor effect block this entry is missing, vmax clamped to cap."""
    try:
        tind, to, tc = find_node(txt, 'qti_minor_rtp_effects')
    except ValueError:
        return txt, 0
    present = set(int(x) for x in re.findall(r'effect_(\d+) \{', txt[to + 1:tc]))
    todo = [e for e in sorted(sel) if e not in present]
    if not todo:
        return txt, 0
    tpl = None
    for t in [309] + sorted(present):
        m = re.search(r'(?m)^([ \t]*)effect_%d \{.*?\n\1\};' % t, txt, re.S)
        if m:
            tpl = m
            break
    if tpl is None:
        return txt, 0
    ci = tind + '\t'
    add = ''
    for eid in todo:
        name, ln = sel[eid]
        b = re.sub(r'effect_\d+ \{', 'effect_%d {' % eid, tpl.group(0), count=1)
        b = re.sub(r'qcom,effect-id = <[^>]*>;', 'qcom,effect-id = <%#x>;' % eid, b, count=1)
        b = re.sub(r'qcom,rtp-name = "[^"]*";', 'qcom,rtp-name = "%s";' % name, b, count=1)
        b = re.sub(r'qcom,rtp-len = <[^>]*>;', 'qcom,rtp-len = <%#x>;' % ln, b, count=1)
        b = re.sub(r'qcom,wf-vmax-mv = <[^>]*>;', 'qcom,wf-vmax-mv = <%#x>;' % cap, b, count=1)
        add += '\n' + b
    return txt[:tc] + add + '\n' + txt[tc:], len(todo)


def fill_scenes(txt, adds):
    """Insert missing base_scene triples ({scene, effect, vmax}, 6 bytes BE)."""
    m = re.search(r'(?m)^([ \t]*)base_scene = \[([0-9a-fA-F \t]*)\];', txt)
    if not m:
        return txt, 0
    raw = bytes(int(x, 16) for x in m.group(2).split())
    if len(raw) % 6:
        return txt, 0
    ents = [(int.from_bytes(raw[i:i + 2], 'big'),
             int.from_bytes(raw[i + 2:i + 4], 'big'),
             int.from_bytes(raw[i + 4:i + 6], 'big')) for i in range(0, len(raw), 6)]
    have = {s for s, _, _ in ents}
    new = [(s, e, v) for s, e, v in adds if s not in have]
    if not new:
        return txt, 0
    out = bytearray()
    for s, e, v in sorted(ents + new):
        out += s.to_bytes(2, 'big') + e.to_bytes(2, 'big') + v.to_bytes(2, 'big')
    return (txt[:m.start(2)] + ' '.join('%02x' % x for x in out) + txt[m.end(2):],
            len(new))


def clamp_scenes(txt, cap):
    """Clamp every base_scene/ext_scene vmax to the entry's own envelope."""
    n = [0]

    def one(m):
        raw = bytes(int(x, 16) for x in m.group(3).split())
        if len(raw) % 6:
            return m.group(0)
        out = bytearray()
        for i in range(0, len(raw), 6):
            s = int.from_bytes(raw[i:i + 2], 'big')
            e = int.from_bytes(raw[i + 2:i + 4], 'big')
            v = int.from_bytes(raw[i + 4:i + 6], 'big')
            if v > cap:
                v = cap
                n[0] += 1
            out += s.to_bytes(2, 'big') + e.to_bytes(2, 'big') + v.to_bytes(2, 'big')
        return '%s%s = [%s];' % (m.group(1), m.group(2), ' '.join('%02x' % x for x in out))
    return re.sub(r'(?m)^([ \t]*)(base_scene|ext_scene) = \[([0-9a-fA-F \t]*)\];',
                  one, txt), n[0]


def facts(t):
    try:
        ind, o, c = find_node(t, NODE)
    except ValueError:
        return None
    b = t[o + 1:c]
    ci = ind + '\t'
    props = []
    for name, _ in VIVO_PROPS:
        m = re.search(r'(?m)^' + ci + re.escape(name) + r'( = <([^>]*)>)?;$', b)
        props.append((name, m.group(2) if m else None))
    for name in REFS:
        m = re.search(r'(?m)^' + ci + name + r' = <([^>]*)>;$', b)
        props.append((name, m.group(1) if m else None))
    vm = re.search(r'qcom,vmax-mv = <(?:0x)?([0-9a-fA-F]+)>', b)
    refs = []
    for name in REFS:
        m = re.search(r'(?m)^' + ci + name + r' = <(?:0x)?([0-9a-fA-F]+)>;$', b)
        refs.append(m.group(1) if m else None)
    phs = []
    effects = {}
    try:
        for name in CHILDREN:
            cind, co, cc = find_node(b, name)
            cb = b[co + 1:cc]
            m = re.search(r'phandle = <(?:0x)?([0-9a-fA-F]+)>', cb)
            phs.append(m.group(1) if m else None)
            effects[name] = (cb.count('effect_'),
                             tuple(sorted(set(re.findall(
                                 r'qcom,wf-vmax-mv = <(?:0x)?([0-9a-fA-F]+)>', cb)))))
    except ValueError:
        phs = [None, None, None]
        effects = {'missing': True}
    fx = set()
    if '__local_fixups__' in t:
        tail = t[t.index('__local_fixups__'):]
        try:
            fi, fo, fc = find_node(tail, NODE)
            fb = tail[fo + 1:fc]
            fx = set(re.findall(r'(\w+) = <0x00>;', fb))
        except ValueError:
            pass
    return dict(props=props, vmax=vm.group(1) if vm else None,
                effects=effects, refs=refs, phandles=phs,
                refs_ok=all(a == b_ for a, b_ in zip(refs, phs)),
                fixups=sorted(fx))
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('stock')
    ap.add_argument('out')
    ap.add_argument('--iqoo-dts', default=None, metavar='PATH',
                    help='iQOO 11 decompiled dtbo dts (default: auto-discover, '
                         'or $%s)' % IQOO_DTS_ENV)
    ap.add_argument('--compare', default=None, metavar='IMG',
                    help='known-good dtbo.img to compare against (read-only)')
    ap.add_argument('--iqoo13-dts', default=None, metavar='PATH',
                    help='iQOO 13 decompiled dtbo dts (full RTP table); '
                         'default $%s. Without it only the iQOO 11 tables are used, '
                         'so most ringtone/notification scenes stay unresolved.' % IQOO13_ENV)
    ap.add_argument('--bands', default='R,N,A,T,S,G',
                    help='waveform families wired from the iQOO 13 table '
                         '(default: R,N,A,T,S,G)')
    ap.add_argument('--firmware', default=None, metavar='DIR',
                    help='firmware dir; donor blocks whose .bin is absent are skipped')
    ap.add_argument('--no-scene-fill', action='store_true',
                    help='skip filling missing base_scene ids (%s)'
                         % ','.join(str(s) for s, _, _ in SCENE_FILL))
    a = ap.parse_args()
    dts = a.iqoo_dts or find_iqoo_dts(a.stock)
    if not os.path.isfile(dts):
        sys.exit('iqoo dts not found: %s' % dts)
    print('iqoo  : %s' % dts)
    iq = open(dts, encoding='utf-8', errors='replace').read()
    scene = iqoo_node(iq, 'lra_scene_array')
    ram = iqoo_node(iq, 'qti_ram_effects')
    rtp = iqoo_node(iq, 'qti_minor_rtp_effects')

    d13 = a.iqoo13_dts or os.environ.get(IQOO13_ENV)
    sel = {}
    if d13:
        if not os.path.isfile(d13) or IQOO13_MARKER not in open(
                d13, encoding='utf-8', errors='replace').read():
            sys.exit('iqoo13 dts unusable (needs %s): %s' % (IQOO13_MARKER, d13))
        sel = select_bands(donor_rtp(d13), a.bands)
        if a.firmware:
            miss = [e for e, (n, _) in sel.items()
                    if not os.path.exists(os.path.join(a.firmware, n))]
            for e in miss:
                del sel[e]
            if miss:
                print('iqoo13: skipped %d blocks with no .bin (%s%s)'
                      % (len(miss), ','.join(str(x) for x in miss[:6]),
                         '...' if len(miss) > 6 else ''))
        print('iqoo13: %s  (%d waveform blocks: %s)' % (d13, len(sel), a.bands))
    else:
        print('iqoo13: not given -> ringtone/notification wiring skipped '
              '(pass --iqoo13-dts or $%s)' % IQOO13_ENV)
    src = open(a.stock, 'rb').read()
    h = header(src)
    print('stock : %d entries, %d bytes  (%s)' % (h['ecount'], h['total'], a.stock))
    blobs, stats_all, bad = [], [], []
    for i, (sz, off, eid, rev), blob in entries(src):
        txt = decompile(blob)
        if 'compatible = "qcom,hv-haptics"' not in txt:
            blobs.append(blob)
            continue
        stats = dict(entries=[], nofixup_nvmem=False)
        new = patch_dts(txt, scene, ram, rtp, stats)
        cap = stats['entries'][0]['cap']
        n_rtp = 0
        if sel:
            new, n_rtp = add_rtp_blocks(new, sel, cap)
        n_sc = 0
        if not a.no_scene_fill:
            new, n_sc = fill_scenes(new, SCENE_FILL)
        new, n_cl = clamp_scenes(new, cap)
        stats['entries'][0]['rtp_added'] = n_rtp
        stats['entries'][0]['scenes_added'] = n_sc
        stats['entries'][0]['scene_clamped'] = n_cl
        r = compile_dts(new)
        if r.returncode:
            bad.append((i, 'dtc: ' + r.stderr.decode().splitlines()[0][:120]))
            blobs.append(blob)
            continue
        f = facts(decompile(r.stdout))
        st = stats['entries'][0]
        prob = None
        if f is None:
            prob = 'no haptics node after patch'
        elif not f['refs_ok']:
            prob = 'ref/phandle mismatch %s vs %s' % (f['refs'], f['phandles'])
        elif set(f['fixups']) - {'nvmem'} != set(REFS):
            prob = '__local_fixups__ = %s' % f['fixups']
        elif f['vmax'] != st['vmax']:
            prob = 'vmax changed %s -> %s' % (st['vmax'], f['vmax'])
        else:
            for name in CHILDREN:
                caps = [int(x, 16) for x in f['effects'][name][1]]
                if name == 'lra_scene_array':
                    continue          # scene table maps scene->effect, no vmax
                if caps and max(caps) > st['cap']:
                    prob = '%s cap %#x > entry cap %#x' % (name, max(caps), st['cap'])
                    break
                expect = sorted({min(x, st['cap']) for x in st['src_caps']})
                if caps != expect:
                    prob = '%s caps %s != expected %s' % (
                        name, [hex(x) for x in caps], [hex(x) for x in expect])
                    break
        if prob:
            bad.append((i, prob))
            blobs.append(blob)
            continue
        stats_all.append((i, st, f))
        blobs.append(r.stdout)
    print('patched + validated: %d/%d entries with a full haptics node'
          % (len(stats_all), h['ecount']))
    if bad:
        for i, prob in bad:
            print('  entry %-3d %s' % (i, prob))
        sys.exit('aborting: validation failed, nothing written')
    total = h['hsize'] + h['ecount'] * h['esize']
    offs = []
    for b in blobs:
        total = (total + h['pgsz'] - 1) // h['pgsz'] * h['pgsz']
        offs.append(total)
        total += len(b)
    out = bytearray(total)
    struct.pack_into('>8I', out, 0, h['magic'], total, h['hsize'], h['esize'],
                     h['ecount'], h['eoff'], h['pgsz'], h['ver'])
    for i, b in enumerate(blobs):
        _, _, eid, rev = entry_table(src, i)
        struct.pack_into('>4I', out, h['eoff'] + i * h['esize'], len(b), offs[i], eid, rev)
        out[offs[i]:offs[i] + len(b)] = b
    open(a.out, 'wb').write(out)
    print('wrote %s (%d bytes)' % (a.out, total))
    print('\nentry  node-vmax  entry-cap  clamp  rtp  scenes  phandles')
    t_rtp = t_sc = 0
    for i, st, f in stats_all:
        t_rtp += st.get('rtp_added', 0)
        t_sc += st.get('scenes_added', 0)
        print('%5d  %9s  %9s  %5d  %3d  %6d  %s'
              % (i, '0x' + str(st['vmax']), hex(st['cap']), st['clamped'],
                 st.get('rtp_added', 0), st.get('scenes_added', 0),
                 ','.join('0x%x' % p for p in st['ph'])))
    print('total: %d waveform blocks wired from iQOO 13, %d scene ids filled'
          % (t_rtp, t_sc))
    if not a.compare:
        return
    print('\nCOMPARE against %s' % a.compare)
    ref = open(a.compare, 'rb').read()
    rh = header(ref)
    same_hdr = (rh['magic'] == h['magic'] and rh['ecount'] == h['ecount']
                and rh['pgsz'] == h['pgsz'])
    print('  header: %d entries, page %d  %s'
          % (rh['ecount'], rh['pgsz'], 'OK' if same_hdr else 'DIFF'))
    diffs = []
    for i in range(min(rh['ecount'], h['ecount'])):
        rsz, roff, _, _ = entry_table(ref, i)
        cr = facts(decompile(ref[roff:roff + rsz]))
        osz, ooff, _, _ = entry_table(out, i)
        ours = facts(decompile(bytes(out[ooff:ooff + osz])))
        if cr is None and ours is None:
            continue
        if cr is None or ours is None:
            diffs.append((i, 'one side has no haptics node'))
            continue
        for k in ('props', 'vmax', 'effects', 'refs_ok', 'fixups'):
            if cr[k] != ours[k]:
                diffs.append((i, '%s: working=%s ours=%s' % (k, cr[k], ours[k])))
                break
        else:
            if cr['refs'] != ours['refs']:
                diffs.append((i, 'INFO refs differ (fine, per-tree): '
                                 'working=%s ours=%s' % (cr['refs'], ours['refs'])))
    hard = [d for d in diffs if not d[1].startswith('INFO')]
    for i, d in diffs[:12]:
        print('  entry %-3d %s' % (i, d))
    if not hard:
        print('  all entries: props, node vmax, effect caps, refs->phandles, '
              'fixups identical ✔')
    else:
        print('  %d entries differ in content' % len(hard))
if __name__ == '__main__':
    main()
