#!/bin/sh
# Regression test: m68k-amigaos g++ must save callee-saved registers it writes,
# exactly as gcc does for the same source (gcc16 C++ callee-save bug, 2026-10-19).
# Usage: test_csr_cxx.sh [compiler-prefix]   (default ~/opt/amiga16/bin/m68k-amigaos-)
P=${1:-$HOME/opt/amiga16/bin/m68k-amigaos-}
D=$(mktemp -d "${TMPDIR:-/tmp}/csr.XXXXXX"); trap 'rm -rf "$D"' EXIT
cat > "$D/t.c" <<'SRC'
struct T { int tab; int start; };
int h(const struct T*p,int x){if(p->tab<=0)return x+1; int a=x-p->start+p->tab; return a-a%p->tab+p->start;}
SRC
fail=0
analyse() { # $1 = asm file -> prints "saved=<regs> written=<regs>"
  awk '
    function addregs(s, arr,   n,i,parts,r,a,b,k) {
      n=split(s,parts,"/")
      for(i=1;i<=n;i++){ r=parts[i]
        if (r ~ /^[ad][0-7]-[ad][0-7]$/) { a=substr(r,2,1)+0; b=substr(r,5,1)+0
          for(k=a;k<=b;k++) arr[substr(r,1,1) k]=1 } else arr[r]=1 }
    }
    /^\t(movem\.l|move\.l)[ \t]+[ad][^,]*,-\(sp\)$/ { split($2,o,","); addregs(o[1],sv) }
    /^\t[a-z]/ { n=split($0,f,","); last=f[n]; gsub(/[ \t]/,"",last)
      if (last ~ /^[da][0-7]$/) wr[last]=1
      if ($1 ~ /^div[su]l/) { m=split($2,q,":"); if (m==2) { wr[q[1]]=1; sub(/.*,/,"",q[1]); wr[q[1]]=1 } } }
    END { s=""; w=""
      for (r in sv) if (r ~ /^d[2-7]$|^a[2-5]$/) s=s r " "
      for (r in wr) if (r ~ /^d[2-7]$|^a[2-5]$/) { w=w r " "; if (!(r in sv)) bad=bad r " " }
      printf "saved=[%s] written=[%s] unsaved=[%s]\n", s, w, bad }' "$1"
}
for lang in c c++; do
  for opt in -O1 -O2; do
    if [ $lang = c ]; then CC=${P}gcc; else CC=${P}g++; fi
    "$CC" -x $lang -m68040 $opt -S -o "$D/$lang$opt.s" "$D/t.c" || { echo "[ERROR] compile $lang $opt"; exit 2; }
    r=$(analyse "$D/$lang$opt.s"); echo "$lang $opt: $r"
    case "$r" in *"unsaved=[]"*) ;; *) echo "[FAIL] $lang $opt writes callee-saved register without saving it"; fail=1;; esac
    eval "res_$(echo $lang | tr + x)_${opt#-}=\"\${r%% written*}\""
  done
done
for opt in O1 O2; do
  eval "a=\$res_c_$opt; b=\$res_cxx_$opt"
  [ "$a" = "$b" ] || { echo "[FAIL] -$opt save mask differs: C $a vs C++ $b"; fail=1; }
done
[ $fail = 0 ] && echo "[OK] C and C++ callee-save masks agree" ; exit $fail
