#!/usr/bin/env python3
"""Convert a MAME cheat XML (one game, e.g. cotton2.xml from Pugsy's cheat collection) into a
RetroArch .cht file for the libretro-stv core.

    tools/mame2cht.py cotton2.xml > "Cotton 2 (JUET 970902 V1.000).cht"

The core applies each code as work-RAM writes after every frame, like a MAME "run" script, so
only cheats made of plain writes (maincpu.pb/pw/pd@ADDR=VALUE, no condition) are converted; a
<parameter> block becomes one entry per selectable value.  Everything else is listed on stderr.
"""
import re, sys, xml.etree.ElementTree as ET

ACTION = re.compile(r'^\s*maincpu\.p([bwd])@([0-9A-Fa-f]+)\s*=\s*(.+?)\s*$')
WIDTH = {'b': 2, 'w': 4, 'd': 8}


def number(text):
    # MAME expressions: hexadecimal by default, '#' prefix for decimal.
    text = text.strip()
    return int(text[1:], 10) if text.startswith('#') else int(text, 16)


def convert(path):
    entries, skipped = [], []
    for cheat in ET.parse(path).getroot().iter('cheat'):
        desc = cheat.get('desc', '?')
        scripts = cheat.findall('script')
        run = [s for s in scripts if s.get('state') == 'run']
        if not run:
            skipped.append((desc, 'no "run" script')); continue
        writes = []
        for action in (a for s in run for a in s.findall('action')):
            m = ACTION.match(action.text or '')
            if action.get('condition') or not m:
                writes = None; break
            writes.append((m.group(2).upper().rjust(8, '0'), WIDTH[m.group(1)], m.group(3)))
        if not writes:
            skipped.append((desc, 'not a plain memory write')); continue
        param = cheat.find('parameter')
        if param is None:
            values = [(None, desc)]
        elif param.findall('item'):
            values = [(number(i.get('value')), '%s: %s' % (desc, (i.text or '').strip())) for i in param.findall('item')]
        else:
            lo, hi, step = (number(param.get(k, d)) for k, d in (('min', '0'), ('max', '0'), ('step', '1')))
            values = [(v, '%s: %d' % (desc, v)) for v in range(lo, hi + 1, step or 1)]
        for value, name in values:
            codes = []
            for addr, width, expr in writes:
                if expr.strip() == 'param':
                    if value is None: codes = None; break
                    v = value
                else:
                    try: v = number(expr)
                    except ValueError: codes = None; break
                codes.append('%s:%0*X' % (addr, width, v & ((1 << (width * 4)) - 1)))
            if codes: entries.append((name, '+'.join(codes)))
            else: skipped.append((name, 'value is an expression')); break
    return entries, skipped


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    entries, skipped = convert(sys.argv[1])
    print('cheats = %d' % len(entries))
    for i, (name, code) in enumerate(entries):
        print('cheat%d_desc = "%s"\ncheat%d_code = "%s"\ncheat%d_enable = false' % (i, name.replace('"', "'"), i, code, i))
    for name, why in skipped:
        print('skipped: %s (%s)' % (name, why), file=sys.stderr)


if __name__ == '__main__':
    main()
