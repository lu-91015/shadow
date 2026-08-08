import os, glob, re

kw = re.compile(r'^\s*(pub\s+)?(kimo|let|const|struct|enum|alias|trait)\s+([A-Za-z_][A-Za-z0-9_]*)')

def scan(root):
    bydir = {}
    for path in glob.glob(root + '/**/*.shadow', recursive=True):
        if path.endswith('.sig') or path.endswith('.lu'):
            continue
        d = os.path.dirname(path)
        bydir.setdefault(d, [])
        depth = 0
        try:
            for line in open(path, encoding='utf-8', errors='ignore'):
                instr = inch = 0
                for c in line:
                    if instr:
                        if c == '\\':
                            continue
                        if c == '"':
                            instr = 0
                        continue
                    if inch:
                        if c == '\\':
                            continue
                        if c == "'":
                            inch = 0
                        continue
                    if c == '"':
                        instr = 1
                        continue
                    if c == "'":
                        inch = 1
                        continue
                    if c == '{':
                        depth += 1
                    elif c == '}':
                        depth -= 1
                m = kw.match(line)
                if m and depth == 0 and not line.lstrip().startswith('//'):
                    is_pub = m.group(1) is not None
                    bydir[d].append((m.group(3), is_pub, os.path.basename(path)))
        except Exception:
            pass
    print('==== DIR SCAN:', root, '====')
    anyc = False
    for d in sorted(bydir):
        names = {}
        for nm, pub, fn in bydir[d]:
            if not pub:
                names.setdefault(nm, []).append(fn)
        dup = {k: v for k, v in names.items() if len(v) > 1}
        if dup:
            anyc = True
            print('  COLLISION', d, '->', dup)
    if not anyc:
        print('  (no same-dir unexported name collision)')

for r in ['src', 'test/cases/migrated_03']:
    scan(r)
