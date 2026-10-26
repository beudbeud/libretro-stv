#!/usr/bin/env python3
"""Convert a MAME cheat XML (one game, e.g. cotton2.xml from Pugsy's cheat collection) into a
RetroArch .cht file for the libretro-stv core.

    tools/mame2cht.py cotton2.xml > "Cotton 2 (JUET 970902 V1.000).cht"

The core applies each code as work-RAM writes after every frame, like a MAME "run" script; a
cheat with only "on"/"change" scripts ("... Now!") becomes a one-shot code (leading '!'), applied
once each time it is enabled.  Only plain writes (maincpu.pb/pw/pd@ADDR=VALUE, no condition) are
converted; a <parameter> block becomes one entry per selectable value.  Everything else is
listed on stderr.
"""
import re, sys, xml.etree.ElementTree as ET

ACTION = re.compile(r'^\s*(?:maincpu|slave)\.[pr]([bwd])@([0-9A-Fa-f]+)\s*=\s*(.+?)\s*$')
TEMP = re.compile(r'^\s*temp\d+\s*=')
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
        once = not run
        if once:
            run = [s for s in scripts if s.get('state') in ('on', 'change')]
        if not run:
            if desc.strip(): skipped.append((desc, 'no script'))
            continue
        writes = []
        for action in (a for s in run for a in s.findall('action')):
            if action.get('condition'):
                writes = None; break
            for expr in (action.text or '').split(','):   # one action may hold several writes
                if TEMP.match(expr): continue              # tempN = ... (saved for the "off" script)
                m = ACTION.match(expr)
                if not m:
                    writes = None; break
                writes.append((m.group(2).upper().rjust(8, '0'), WIDTH[m.group(1)], m.group(3).strip('() ')))
            if writes is None: break
        if not writes:
            skipped.append((desc, 'not a plain memory write')); continue
        param = cheat.find('parameter')
        if param is None:
            values = [(None, desc)]
        elif param.findall('item'):
            values = [(number(i.get('value')), '%s: %s' % (desc, (i.text or '').strip())) for i in param.findall('item')]
        else:
            lo, hi, step = (number(param.get(k, d)) for k, d in (('min', '0'), ('max', '0'), ('step', '1')))
            rng = list(range(lo, hi + 1, step or 1))
            if len(rng) > 20:   # keep the .cht readable: 20 evenly spaced values, min and max included
                rng = sorted(set(rng[int(i * (len(rng) - 1) / 19)] for i in range(20)))
            values = [(v, '%s: %d' % (desc, v)) for v in rng]
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
            if codes: entries.append((name, ('!' if once else '') + '+'.join(codes)))
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
