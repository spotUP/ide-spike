import re, sys
keep = {"A68k","Asm","Bash","CPP","JSON","Lua","Make","Markdown","Python"}
out = []
for l in open(sys.argv[1]):
    m = re.match(r'\s*(?:extern const LexerModule |&)lm(\w+)[;,]\s*$', l)
    if m and m.group(1) not in keep:
        continue
    out.append(l)
open(sys.argv[2], "w").write("".join(out))
