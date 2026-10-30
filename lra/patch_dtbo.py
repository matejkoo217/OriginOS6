#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Patch fuxi's dtbo with the vivo (iQOO 11) haptics DT data, safely.

For every DTBO entry that carries a full `qcom,hv-haptics` node:
  * add the vivo props the driver reads in vhc_parse_dt()
  * add the scene table + effect tables lifted from the iQOO 11 dtbo
  * BUT keep fuxi's electrical envelope:
      - qcom,vmax-mv left untouched (1200 mV)
      - every copied qcom,wf-vmax-mv clamped to fuxi's own effect vmax (3600 mV)
      - freq_min/freq_max from fuxi's qcom,lra-f0-* (160..185 Hz -> 1600..1850)
      - fuxi's qcom,lra-period-us untouched
  * vib_num=1 + is_major_motor, so the driver registers /dev/vivo_haptic
    (motor 0, which vivo's HAL actually plays on) instead of _vice

usage: sudo-ish python3 patch_dtbo.py /path/fuxi-dtbo.img /path/out-dtbo.img
"""
import os
import re
import struct
import subprocess
import sys
import tempfile

IQOO_DTS = '/home/jkoo/mio/ota/dtbo/dts/dts.2'
FUJI_MAX_EFFECT_VMAX = 3600          # fuxi effect_N qcom,wf-vmax-mv (0xe10)
FUJI_FREQ_MIN = 1600                 # 160.0 Hz  (qcom,lra-f0-min  = 0xa0)
FUJI_FREQ_MAX = 1850                 # 185.0 Hz  (qcom,lra-f0-max  = 0xb9)
LRA_INFO = 0x257b                    # motor model id; kept from iQOO, see README


def extract_node(text, name):
    key = '\n\t\t\t%s {' % name
    i = text.index(key)
    start = text.index('{', i)
    depth = 0
    for j in range(start, len(text)):
        if text[j] == '{':
            depth += 1
        elif text[j] == '}':
            depth -= 1
            if depth == 0:
                body = text[start + 1:j]
                break
    body = '\n'.join(l for l in body.splitlines() if 'phandle = <' not in l)
    return body.rstrip()


def clamp_vmax(body):
    """Clamp qcom,wf-vmax-mv values to fuxi's envelope; report what was clamped."""
    clamped = []

    def repl(m):
        v = int(m.group(1), 0)
        if v > FUJI_MAX_EFFECT_VMAX:
            clamped.append(v)
            return 'qcom,wf-vmax-mv = <%#x>' % FUJI_MAX_EFFECT_VMAX
        return m.group(0)

    return re.sub(r'qcom,wf-vmax-mv = <(0x[0-9a-fA-F]+|\d+)>', repl, body), clamped


def vivo_props(p_scene, p_ram, p_rtp):
    return f"""
					lra_info = <{LRA_INFO:#x}>;
					vib_num = <0x01>;
					is_major_motor;
					richtap_type = <0x01>;
					resistance_min = <0x1388>;
					resistance_max = <0x4e20>;
					freq_min = <{FUJI_FREQ_MIN:#x}>;
					freq_max = <{FUJI_FREQ_MAX:#x}>;
					scene_array = <&lra_scene_array>;
					ram_effects = <&qti_ram_effects>;
					rtp_effects = <&qti_minor_rtp_effects>;
"""


def vivo_nodes(scene, ram, rtp, p_scene, p_ram, p_rtp):
    return f"""
					lra_scene_array: lra_scene_array {{
{scene}
					}};
					qti_ram_effects: qti_ram_effects {{
{ram}
					}};
					qti_minor_rtp_effects: qti_minor_rtp_effects {{
{rtp}
					}};"""


def patch_dts(text, scene, ram, rtp):
    """Insert the vivo block into every full haptics node in one decompiled DT."""
    pat = re.compile(r'(\n(\t+)qcom,hv-haptics@f000 \{\n)')
    out = text
    inserted = 0
    pos = 0
    while True:
        m = pat.search(out, pos)
        if not m:
            break
        # find the matching close brace for this node
        depth = 0
        start = out.index('{', m.start())
        for j in range(start, len(out)):
            if out[j] == '{':
                depth += 1
            elif out[j] == '}':
                depth -= 1
                if depth == 0:
                    end = j
                    break
        body = out[start:end]
        if 'compatible = "qcom,hv-haptics"' not in body:
            pos = end
            continue
        # fresh phandles: max existing + 1..3
        used = [int(x, 16) for x in re.findall(r'phandle = <0x([0-9a-fA-F]+)>', out)]
        base = (max(used) + 1) if used else 1
        p_scene, p_ram, p_rtp = base, base + 1, base + 2
        props = vivo_props(p_scene, p_ram, p_rtp)
        nodes = vivo_nodes(scene, ram, rtp, p_scene, p_ram, p_rtp)
        # properties must come before subnodes: props right after the open brace,
        # the new child nodes just before the closing brace
        open_end = out.index('{', m.start()) + 1
        out = out[:open_end] + props + out[open_end:end] + nodes + out[end:]
        inserted += 1
        pos = end + len(props) + len(nodes)
    return out, inserted


def main():
    src, dst = sys.argv[1], sys.argv[2]
    d = open(src, 'rb').read()
    magic, total, hsize, esize, ecount, eoff, pgsz, ver = struct.unpack_from('>8I', d, 0)
    assert magic == 0xd7b7ab1e, hex(magic)
    print(f'fuxi dtbo: {ecount} entries, total {total} bytes')

    iq = open(IQOO_DTS, encoding='utf-8', errors='replace').read()
    scene, ram, rtp = (extract_node(iq, 'lra_scene_array'),
                       extract_node(iq, 'qti_ram_effects'),
                       extract_node(iq, 'qti_minor_rtp_effects'))
    ram, c1 = clamp_vmax(ram)
    rtp, c2 = clamp_vmax(rtp)
    clamped = c1 + c2
    print(f'effect tables: {len(ram)}B ram, {len(rtp)}B rtp; clamped {len(clamped)} '
          f'wf-vmax-mv values to {FUJI_MAX_EFFECT_VMAX} mV '
          f'(max seen {max(clamped) if clamped else 0} mV -> {FUJI_MAX_EFFECT_VMAX})')

    tmp = tempfile.mkdtemp()
    blobs = []
    patched = 0
    for i in range(ecount):
        sz, off, id_, rev = struct.unpack_from('>4I', d, eoff + i * esize)
        blob = d[off:off + sz]
        dtb = os.path.join(tmp, f'{i}.dtb')
        open(dtb, 'wb').write(blob)
        dts = subprocess.run(['dtc', '-I', 'dtb', '-O', 'dts', dtb],
                             capture_output=True, text=True).stdout
        if 'compatible = "qcom,hv-haptics"' not in dts:
            blobs.append(blob)
            continue
        new, n = patch_dts(dts, scene, ram, rtp)
        patched += n
        src_dts = os.path.join(tmp, f'{i}.dts')
        open(src_dts, 'w').write(new)
        out_dtb = os.path.join(tmp, f'{i}-p.dtb')
        r = subprocess.run(['dtc', '-@', '-I', 'dts', '-O', 'dtb', '-o', out_dtb, src_dts],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'entry {i}: dtc failed:\n{r.stderr[:2000]}')
        blobs.append(open(out_dtb, 'rb').read())
    print(f'patched {patched} haptics node(s) across {ecount} entries')

    # rebuild the DTBO image (same layout: header + page-aligned blobs)
    new_total = hsize + ecount * esize
    offsets = []
    for b in blobs:
        new_total = (new_total + pgsz - 1) // pgsz * pgsz
        offsets.append(new_total)
        new_total += len(b)
    out = bytearray(new_total)
    struct.pack_into('>8I', out, 0, magic, new_total, hsize, esize, ecount, eoff, pgsz, ver)
    for i, b in enumerate(blobs):
        sz, _, id_, rev = struct.unpack_from('>4I', d, eoff + i * esize)
        struct.pack_into('>4I', out, eoff + i * esize, len(b), offsets[i], id_, rev)
        out[offsets[i]:offsets[i] + len(b)] = b
    open(dst, 'wb').write(out)
    print(f'wrote {dst} ({new_total} bytes, was {total})')


if __name__ == '__main__':
    main()
