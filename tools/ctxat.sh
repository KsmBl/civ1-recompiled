#!/bin/sh
# ctxat.sh MOD SEG:OFF [before] [after]  -- context of an instruction
mod=$1; so=$2; b=${3:-10}; a=${4:-6}
seg=${so%:*}; off=${so#*:}
if [ "$mod" = root ]; then f=build/analysis/root.asm; else f=build/analysis/ovl.asm; fi
func=$(awk -v s="$seg" -v o="$off" -v m="$mod" '/^; ---- /{f=$3} $0 ~ "^. "o" " {
   if (m=="root" && index(f, "f_" s "_")!=1) next; if (m!="root" && index(f, m "_")!=1) next; print f; exit}' $f)
echo "## $mod $so in $func"
tools/py tools/ctx.py $f $func $off $b $a
