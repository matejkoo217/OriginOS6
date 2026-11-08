#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse
import glob
import os
import re
import struct
import subprocess
import sys

NODE = 'qcom,hv-haptics@f000'
HAPTIC_COMPAT = 'compatible = "qcom,hv-haptics"'
RTP_NODE = 'qti_minor_rtp_effects'
TEMPLATE_EFFECT = 309           # existing qti rtp entry used as a format donor
RING_IDS = range(501, 519)      # vivo's 18 ringtone patterns
DEFAULT_CAP = 0xe10             # 3600 mV
IQOO_DTS_DIR = 'your base dts dir'

def header(img):
    f = struct.unpack_from('>8I', img, 0)
    if f[0] != 0xd7b7ab1e:
        raise SystemExit('not a dtbo image (magic %#x)' % f[0])
    return dict(zip(('magic', 'total', 'hsize', 'esize', 'ecount', 'eoff', 'pgsz', 'ver'), f))

def entries(img):
    h = header(img)
    for i in range(h['ecount']):
        sz, off, eid, rev = struct.unpack_from('>4I', img, h['eoff'] + i * h['esize'])
        yield i, sz, off, eid, rev, bytes(img[off:off + sz])

def repack(h, blobs, entry_ids):
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
        eid, rev = entry_ids[i]
        struct.pack_into('>4I', out, h['eoff'] + i * h['esize'], len(b), offs[i], eid, rev)
        out[offs[i]:offs[i] + len(b)] = b
    return bytes(out)

def decompile(blob):
    r = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts'], input=blob, capture_output=True)
    if r.returncode:
        raise SystemExit('dtc -I dtb failed: ' + r.stderr.decode()[:300])
    return r.stdout.decode()

def compile_dts(txt):
    r = subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb'], input=txt.encode(), capture_output=True)
    if r.returncode:
        raise SystemExit('dtc -I dts failed: ' + r.stderr.decode()[:300])
    return r.stdout

def node_span(text, name, want_compat=None):
    for m in re.finditer(r'(?m)^([ \t]*)' + re.escape(name) + r' \{', text):
        i, d = m.end(), 1
        while i < len(text) and d:
            c = text[i]
            if c == '{':
                d += 1
            elif c == '}':
                d -= 1
            i += 1
        if want_compat is None or want_compat in text[m.end():i]:
            return m.end(), i - 1
    return None

def haptics_body(txt):
    return node_span(txt, NODE, HAPTIC_COMPAT)

def entry_cap(body):
    caps = [int(x, 0) for x in re.findall(r'qcom,wf-vmax-mv = <(0x[0-9a-fA-F]+|\d+)>', body)]
    return max(caps) if caps else DEFAULT_CAP

def scene_triples(raw):
    b = bytes(int(x, 16) for x in raw.split())
    if len(b) % 6:
        raise SystemExit('scene table length not a multiple of 6')
    return [(int.from_bytes(b[i:i + 2], 'big'),
             int.from_bytes(b[i + 2:i + 4], 'big'),
             int.from_bytes(b[i + 4:i + 6], 'big')) for i in range(0, len(b), 6)]

def clamp_scenes(text, cap, counter):
    def one(m):
        out = []
        for scene, eff, vmax in scene_triples(m.group(3)):
            if vmax > cap:
                counter[0] += 1
                vmax = cap
            out.append((scene, eff, vmax))
        b = bytearray()
        for s, e, v in out:
            b += s.to_bytes(2, 'big') + e.to_bytes(2, 'big') + v.to_bytes(2, 'big')
        return '%s%s = [%s];' % (m.group(1), m.group(2), ' '.join('%02x' % x for x in b))
    return re.sub(r'(?m)^([ \t]*)(base_scene|ext_scene) = \[([0-9a-fA-F \t]*)\];', one, text)

def template_node(text, eid):
    span = node_span(text, 'effect_%d' % eid)
    if span is None:
        return None
    start = text.rfind('\n', 0, span[0]) + 1
    open_idx = text.rfind('{', 0, span[0])
    line_start = text.rfind('\n', 0, open_idx) + 1
    indent = text[line_start:open_idx]
    return text[start:text.find('};', span[1]) + 2], indent

def make_node(tpl, indent, eid, rtp_name, rtp_len, cap):
    body = tpl
    body = re.sub(r'effect_\d+ \{', 'effect_%d {' % eid, body, count=1)
    body = re.sub(r'qcom,effect-id = <[^>]*>;', 'qcom,effect-id = <%#x>;' % eid, body, count=1)
    body = re.sub(r'qcom,rtp-name = "[^"]*";', 'qcom,rtp-name = "%s";' % rtp_name, body, count=1)
    body = re.sub(r'qcom,rtp-len = <[^>]*>;', 'qcom,rtp-len = <%#x>;' % rtp_len, body, count=1)
    body = re.sub(r'qcom,wf-vmax-mv = <[^>]*>;', 'qcom,wf-vmax-mv = <%#x>;' % cap, body, count=1)
    return '\n' + body

def add_rtp_effects(text, ring, stats):
    span = node_span(text, RTP_NODE)
    if span is None:
        return text, 0
    body = text[span[0]:span[1]]
    present = set(int(x) for x in re.findall(r'effect_(\d+) \{', body))
    todo = [r for r in ring if r[0] not in present]
    if not todo:
        return text, 0
    tpl, indent = template_node(text, TEMPLATE_EFFECT)
    if tpl is None:
        raise SystemExit('template effect_%d not found in this DTB' % TEMPLATE_EFFECT)
    cap = entry_cap(text[node_span(text, NODE, HAPTIC_COMPAT)[0]:node_span(text, NODE, HAPTIC_COMPAT)[1]])
    add = ''.join(make_node(tpl, indent, eid, name, ln, cap) for eid, name, ln in todo)
    stats['rtp_added'] += len(todo)
    return text[:span[1]] + add + '\n' + text[span[1]:], len(todo)

def vivo_ring(ids, fw_dir, dts_dir):
    src = None
    for p in sorted(glob.glob(os.path.join(dts_dir, 'dts.*'))):
        if 'aw_major_rtp_effects' in open(p, encoding='utf-8', errors='replace').read():
            src = p
            break
    if not src:
        raise SystemExit('no iQOO dts with aw_major_rtp_effects in %s' % dts_dir)
    txt = open(src, encoding='utf-8', errors='replace').read()
    span = node_span(txt, 'aw_major_rtp_effects')
    body = txt[span[0]:span[1]]
    out = {}
    for eid, chunk in re.findall(r'effect_(\d+) \{(.*?)\n[ \t]*\};', body, re.S):
        eid = int(eid)
        if eid not in ids:
            continue
        f = re.search(r'awinic,rtp-file = "([^"]+)"', chunk)
        ln = re.search(r'awinic,wf-length = <(0x[0-9a-fA-F]+|\d+)>', chunk)
        if not f or not ln:
            continue
        name, length = f.group(1), int(ln.group(1), 0)
        path = os.path.join(fw_dir, name)
        if not os.path.exists(path):
            raise SystemExit('missing waveform %s' % path)
        if os.path.getsize(path) < length:
            raise SystemExit('%s is %d bytes but vivo says len %d'
                             % (name, os.path.getsize(path), length))
        out[eid] = (name, length)
    return src, out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('image')
    ap.add_argument('-o', '--out', required=True)
    ap.add_argument('--iqoo-dts', default=IQOO_DTS_DIR)
    ap.add_argument('--firmware', default='X''your_base')
    ap.add_argument('--report', action='store_true')
    a = ap.parse_args()
    src, ring = vivo_ring(set(RING_IDS), a.firmware, a.iqoo_dts)
    missing = sorted(set(RING_IDS) - set(ring))
    print('vivo source : %s' % src)
    print('ring band   : %d effects resolved%s'
          % (len(ring), (' (missing %s)' % missing) if missing else ''))
    if a.report:
        for eid in sorted(ring):
            print('   %d  %-32s len %d' % (eid, ring[eid][0], ring[eid][1]))
        return
    img = open(a.image, 'rb').read()
    h = header(img)
    blobs, ids = [], []
    stats = dict(rtp_added=0, scene_clamped=0, touched=0)
    counter = [0]
    for i, sz, off, eid, rev, blob in entries(img):
        ids.append((eid, rev))
        txt = decompile(blob)
        span = haptics_body(txt)
        if span is None:
            blobs.append(blob)
            continue
        cap = entry_cap(txt[span[0]:span[1]])
        counter[0] = 0
        txt = clamp_scenes(txt, cap, counter)
        stats['scene_clamped'] += counter[0]
        txt, n = add_rtp_effects(txt, [(e, v[0], v[1]) for e, v in sorted(ring.items())], stats)
        blobs.append(compile_dts(txt))
        stats['touched'] += 1
        print('entry %2d  cap=%4d  scene-vmax clamped=%3d  rtp nodes added=%d'
              % (i, cap, counter[0], n))
    out = repack(h, blobs, ids)
    open(a.out, 'wb').write(out)
    print('\n%d entries touched, %d scene vmax clamped, %d rtp nodes added'
          % (stats['touched'], stats['scene_clamped'], stats['rtp_added']))
    print('wrote %s (%d bytes, was %d)' % (a.out, len(out), len(img)))

if __name__ == '__main__':
    main()
