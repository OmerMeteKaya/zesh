#!/usr/bin/env bash
# Regenerate the seed corpus for the Zesh fuzz targets.
# Idempotent: safe to re-run. Invoked by `make corpus`.
set -eu

cd "$(dirname "$0")"

LEX=corpus/lexer
PAR=corpus/parser
EXP=corpus/expand
mkdir -p "$LEX" "$EXP"

# ---- lexer / parser seeds: representative shell snippets ----------------
write() { printf '%s' "$2" > "$LEX/$1"; }

write 001_simple.sh    'echo hello'$'\n'
write 002_pipe.sh      'echo foo | cat'$'\n'
write 003_if.sh        'if true; then echo hi; fi'$'\n'
write 004_vars.sh      'X=hello; echo $X'$'\n'
write 005_subshell.sh  '(echo hi)'$'\n'
write 006_heredoc.sh   'cat <<EOF'$'\n''hello'$'\n''EOF'$'\n'
write 007_arith.sh     'echo $((1+2))'$'\n'
write 008_special.sh   'echo ${X:-default}'$'\n'
write 009_glob.sh      'echo *.c'$'\n'
write 010_quotes.sh    'echo "hello $X" '\''literal'\'''$'\n'
write 011_ansi.sh      'echo $'\''\n\t\x41'\'''$'\n'
write 012_coproc.sh    'coproc { cat; }'$'\n'
write 013_while.sh     'while read L; do echo $L; done'$'\n'
write 014_for.sh       'for x in a b c; do echo $x; done'$'\n'
write 015_case.sh      'case $X in a) echo a;; esac'$'\n'
write 016_func.sh      'f() { echo hi; }; f'$'\n'
write 017_array.sh     'A=(1 2 3); echo ${A[1]}'$'\n'
write 018_redir.sh     'echo hi > /dev/null'$'\n'
write 019_trap.sh      'trap '\''echo bye'\'' EXIT'$'\n'
: > "$LEX/020_empty.sh"   # intentionally empty

# The parser eats the same grammar as the lexer; share the seeds.
rm -rf "$PAR"
cp -r "$LEX" "$PAR"

# ---- expand_word seeds --------------------------------------------------
writee() { printf '%s' "$2" > "$EXP/$1"; }

writee 001.txt '$VAR'
writee 002.txt '${VAR:-default}'
writee 003.txt '$(echo hello)'
writee 004.txt '$((1+2*3))'
writee 005.txt '${#VAR}'
writee 006.txt '$'\''\n\t\x41'\'''
writee 007.txt '~root'
writee 008.txt '${VAR@U}'
writee 009.txt '${VAR:1:3}'
writee 010.txt '${VAR:?error}'

echo "Corpus written:"
echo "  $LEX  ($(ls -1 "$LEX" | wc -l | tr -d ' ') files)"
echo "  $PAR  ($(ls -1 "$PAR" | wc -l | tr -d ' ') files)"
echo "  $EXP  ($(ls -1 "$EXP" | wc -l | tr -d ' ') files)"
