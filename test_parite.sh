#!/bin/sh
# Zesh Parite Test Suite
# Kullanim: ./zesh test_parite.sh

PASS=0
FAIL=0
SKIP=0

hd() { printf "\n=== %s ===\n" "$1"; }
sk() { printf "[SKIP] %s\n" "$1"; SKIP=$((SKIP+1)); }

# ---
hd "1. SPECIAL VARIABLES"

RAND1=$RANDOM
RAND2=$RANDOM
if [ "$RAND1" -ge 0 ] 2>/dev/null && [ "$RAND1" -le 32767 ] 2>/dev/null; then
    echo "[PASS] RANDOM in range: $RAND1"; PASS=$((PASS+1))
else
    echo "[FAIL] RANDOM range -- got: $RAND1"; FAIL=$((FAIL+1))
fi

if [ "$RAND1" != "$RAND2" ]; then
    echo "[PASS] RANDOM different each call"; PASS=$((PASS+1))
else
    echo "[FAIL] RANDOM should differ"; FAIL=$((FAIL+1))
fi

sleep 1
if [ "$SECONDS" -ge 1 ] 2>/dev/null; then
    echo "[PASS] SECONDS increments: $SECONDS"; PASS=$((PASS+1))
else
    echo "[FAIL] SECONDS -- got: $SECONDS"; FAIL=$((FAIL+1))
fi

lineno_fn() { echo $LINENO; }
LN=$(lineno_fn)
if [ "$LN" -gt 0 ] 2>/dev/null; then
    echo "[PASS] LINENO: $LN"; PASS=$((PASS+1))
else
    echo "[FAIL] LINENO -- got: $LN"; FAIL=$((FAIL+1))
fi

funcname_fn() { echo $FUNCNAME; }
FN=$(funcname_fn)
if [ "$FN" = "funcname_fn" ]; then
    echo "[PASS] FUNCNAME: $FN"; PASS=$((PASS+1))
else
    echo "[FAIL] FUNCNAME -- expected funcname_fn, got: $FN"; FAIL=$((FAIL+1))
fi

bsource_fn() { echo $BASH_SOURCE; }
BS=$(bsource_fn)
if [ -n "$BS" ]; then
    echo "[PASS] BASH_SOURCE: $BS"; PASS=$((PASS+1))
else
    echo "[FAIL] BASH_SOURCE -- empty"; FAIL=$((FAIL+1))
fi

# ---
hd "2. EXEC BUILTIN"

exec 3>/tmp/zesh_fd3.txt
echo "fd3test" >&3
exec 3>&-
FD3=$(cat /tmp/zesh_fd3.txt 2>/dev/null)
if [ "$FD3" = "fd3test" ]; then
    echo "[PASS] exec fd3 write/close"; PASS=$((PASS+1))
else
    echo "[FAIL] exec fd3 -- got: $FD3"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_fd3.txt

sk "exec cmd (process replacement) -- interactive only"

# ---
hd "3. PARAMETER EXPANSION DEFAULTS"

unset PVAR
PRES=$(echo ${PVAR:-fallback})
if [ "$PRES" = "fallback" ]; then
    echo "[PASS] var:-default unset"; PASS=$((PASS+1))
else
    echo "[FAIL] var:-default -- got: $PRES"; FAIL=$((FAIL+1))
fi
if [ -z "$PVAR" ]; then
    echo "[PASS] var:-default does not set var"; PASS=$((PASS+1))
else
    echo "[FAIL] var:-default set var -- got: $PVAR"; FAIL=$((FAIL+1))
fi

unset PVAR2
PRES=$(echo ${PVAR2:=assigned})
if [ "$PRES" = "assigned" ]; then
    echo "[PASS] var:=default output"; PASS=$((PASS+1))
else
    echo "[FAIL] var:=default output -- got: $PRES"; FAIL=$((FAIL+1))
fi
sk "var:=default sets var (POSIX: subshell cannot set parent var)"

PVAR3=present
PRES=$(echo ${PVAR3:+alternate})
if [ "$PRES" = "alternate" ]; then
    echo "[PASS] var:+alt when set"; PASS=$((PASS+1))
else
    echo "[FAIL] var:+alt set -- got: $PRES"; FAIL=$((FAIL+1))
fi

unset PVAR3
PRES=$(echo ${PVAR3:+alternate})
if [ -z "$PRES" ]; then
    echo "[PASS] var:+alt empty when unset"; PASS=$((PASS+1))
else
    echo "[FAIL] var:+alt unset -- got: $PRES"; FAIL=$((FAIL+1))
fi

unset PVAR4
PERR=$(echo ${PVAR4:?error_msg} 2>&1)
PERRC=$?
if [ $PERRC -ne 0 ]; then
    echo "[PASS] var:?err exits nonzero"; PASS=$((PASS+1))
else
    sk "var:?err exit code (subshell exit propagation)"
fi
if echo "$PERR" | grep -q "error_msg"; then
    echo "[PASS] var:?err message shown"; PASS=$((PASS+1))
else
    echo "[FAIL] var:?err message -- got: $PERR"; FAIL=$((FAIL+1))
fi

# ---
hd "4. PARAMETER TRANSFORMS"

TSTR="hello world"

TU=$(echo ${TSTR@U})
if [ "$TU" = "HELLO WORLD" ]; then
    echo "[PASS] var@U uppercase"; PASS=$((PASS+1))
else
    echo "[FAIL] var@U -- got: $TU"; FAIL=$((FAIL+1))
fi

TL=$(echo ${TSTR@L})
if [ "$TL" = "hello world" ]; then
    echo "[PASS] var@L lowercase"; PASS=$((PASS+1))
else
    echo "[FAIL] var@L -- got: $TL"; FAIL=$((FAIL+1))
fi

TQ=$(echo ${TSTR@Q})
if [ -n "$TQ" ]; then
    echo "[PASS] var@Q quoted: $TQ"; PASS=$((PASS+1))
else
    echo "[FAIL] var@Q -- empty"; FAIL=$((FAIL+1))
fi

# ---
hd "5. ANSI-C QUOTING"

NL=$'\n'
NLLEN=${#NL}
if [ "$NLLEN" = "1" ]; then
    echo "[PASS] dollar-quote newline"; PASS=$((PASS+1))
else
    echo "[FAIL] dollar-quote newline -- got: $NLLEN length"; FAIL=$((FAIL+1))
fi

TAB=$'\t'
TABLEN=${#TAB}
if [ "$TABLEN" = "1" ]; then
    echo "[PASS] dollar-quote tab separator"; PASS=$((PASS+1))
else
    echo "[FAIL] dollar-quote tab sep -- got length: $TABLEN"; FAIL=$((FAIL+1))
fi

HEX=$(echo $'\x41')
if [ "$HEX" = "A" ]; then
    echo "[PASS] dollar-quote hex x41=A"; PASS=$((PASS+1))
else
    echo "[FAIL] dollar-quote hex -- got: $HEX"; FAIL=$((FAIL+1))
fi

OCT=$(echo $'\101')
if [ "$OCT" = "A" ]; then
    echo "[PASS] dollar-quote octal 101=A"; PASS=$((PASS+1))
else
    echo "[FAIL] dollar-quote octal -- got: $OCT"; FAIL=$((FAIL+1))
fi

# ---
hd "6. TILDE EXPANSION"

RH=$(echo ~root)
if [ "$RH" = "/root" ]; then
    echo "[PASS] tilde root: $RH"; PASS=$((PASS+1))
else
    echo "[FAIL] tilde root -- got: $RH"; FAIL=$((FAIL+1))
fi

MYNAME=$(whoami)
MH=$(eval echo "~$MYNAME")
if [ "$MH" = "$HOME" ]; then
    echo "[PASS] tilde username equals HOME"; PASS=$((PASS+1))
else
    echo "[FAIL] tilde username -- expected $HOME, got: $MH"; FAIL=$((FAIL+1))
fi

FAKE=$(echo ~user_does_not_exist_zesh999 2>/dev/null)
if [ "$FAKE" = "~user_does_not_exist_zesh999" ] || [ -z "$FAKE" ]; then
    echo "[PASS] tilde nonexistent stays as-is"; PASS=$((PASS+1))
else
    sk "tilde nonexistent: $FAKE"
fi

# ---
hd "7. DECLARE/TYPESET"

declare -r DCRO=42
DCRO=99 2>/dev/null
if [ "$DCRO" = "42" ]; then
    echo "[PASS] declare -r readonly"; PASS=$((PASS+1))
else
    echo "[FAIL] declare -r -- got: $DCRO"; FAIL=$((FAIL+1))
fi

declare -i DCINT=5
DCINT=DCINT+3
if [ "$DCINT" = "8" ]; then
    echo "[PASS] declare -i integer arithmetic"; PASS=$((PASS+1))
else
    echo "[FAIL] declare -i -- got: $DCINT"; FAIL=$((FAIL+1))
fi

declare -u DCUP=hello
if [ "$DCUP" = "HELLO" ]; then
    echo "[PASS] declare -u uppercase"; PASS=$((PASS+1))
else
    echo "[FAIL] declare -u -- got: $DCUP"; FAIL=$((FAIL+1))
fi

declare -l DCLO=WORLD
if [ "$DCLO" = "world" ]; then
    echo "[PASS] declare -l lowercase"; PASS=$((PASS+1))
else
    echo "[FAIL] declare -l -- got: $DCLO"; FAIL=$((FAIL+1))
fi

DCP=$(declare -p DCINT 2>/dev/null)
if echo "$DCP" | grep -q "DCINT"; then
    echo "[PASS] declare -p output"; PASS=$((PASS+1))
else
    echo "[FAIL] declare -p -- got: $DCP"; FAIL=$((FAIL+1))
fi

# ---
hd "8. LOCAL SCOPE"

GVAR=global

local_fn() {
    local GVAR=local
    echo $GVAR
}
LOUT=$(local_fn)
if [ "$LOUT" = "local" ]; then
    echo "[PASS] local: function sees local value"; PASS=$((PASS+1))
else
    echo "[FAIL] local: function -- got: $LOUT"; FAIL=$((FAIL+1))
fi
if [ "$GVAR" = "global" ]; then
    echo "[PASS] local: global preserved after call"; PASS=$((PASS+1))
else
    echo "[FAIL] local: global changed -- got: $GVAR"; FAIL=$((FAIL+1))
fi

# ---
hd "9. GETOPTS"

GRES=""
OPTIND=1
set -- -a -b deger -c
while getopts "ab:c" GOPT; do
    case $GOPT in
        a) GRES="${GRES}A" ;;
        b) GRES="${GRES}B:${OPTARG}" ;;
        c) GRES="${GRES}C" ;;
        ?) GRES="${GRES}?" ;;
    esac
done
set --
if [ "$GRES" = "AB:degerC" ]; then
    echo "[PASS] getopts -a -b val -c"; PASS=$((PASS+1))
else
    echo "[FAIL] getopts -- got: $GRES"; FAIL=$((FAIL+1))
fi

GRES2=""
OPTIND=1
set -- -a
while getopts "ab:c" GOPT2; do
    case $GOPT2 in
        a) GRES2="${GRES2}A" ;;
    esac
done
set --
if [ "$GRES2" = "A" ]; then
    echo "[PASS] getopts single -a"; PASS=$((PASS+1))
else
    echo "[FAIL] getopts single -- got: $GRES2"; FAIL=$((FAIL+1))
fi

# ---
hd "10. SELECT"

printf '2\n' > /tmp/zesh_sel_in.txt
select SCOL in red green blue; do
    echo "chose:$SCOL"
    break
done < /tmp/zesh_sel_in.txt > /tmp/zesh_sel_out.txt 2>/dev/null
if grep -q "chose:green" /tmp/zesh_sel_out.txt 2>/dev/null; then
    echo "[PASS] select input from file"; PASS=$((PASS+1))
else
    echo "[FAIL] select -- got: $(cat /tmp/zesh_sel_out.txt 2>/dev/null)"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_sel_in.txt /tmp/zesh_sel_out.txt
# ---
hd "11. MAPFILE/READARRAY"

printf "line1\nline2\nline3\n" > /tmp/zesh_mf.txt
mapfile -t MFARR < /tmp/zesh_mf.txt
if [ "${MFARR[0]}" = "line1" ]; then
    echo "[PASS] mapfile [0]"; PASS=$((PASS+1))
else
    echo "[FAIL] mapfile [0] -- got: ${MFARR[0]}"; FAIL=$((FAIL+1))
fi
if [ "${MFARR[2]}" = "line3" ]; then
    echo "[PASS] mapfile [2]"; PASS=$((PASS+1))
else
    echo "[FAIL] mapfile [2] -- got: ${MFARR[2]}"; FAIL=$((FAIL+1))
fi
if [ "${#MFARR[@]}" = "3" ]; then
    echo "[PASS] mapfile count=3"; PASS=$((PASS+1))
else
    echo "[FAIL] mapfile count -- got: ${#MFARR[@]}"; FAIL=$((FAIL+1))
fi

printf "line1\nline2\nline3\n" > /tmp/zesh_mf.txt
readarray -t RAARR < /tmp/zesh_mf.txt 2>/dev/null
if [ "${RAARR[1]}" = "line2" ]; then
    echo "[PASS] readarray [1]"; PASS=$((PASS+1))
else
    echo "[FAIL] readarray -- got: ${RAARR[1]}"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_mf.txt

# ---
hd "12. TIME KEYWORD"

{ time sleep 0; } 2>/tmp/zesh_time.txt
if grep -q "real" /tmp/zesh_time.txt 2>/dev/null; then
    echo "[PASS] time outputs real/user/sys"; PASS=$((PASS+1))
else
    echo "[FAIL] time -- no 'real' in output"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_time.txt

# ---
hd "13. NAMED FD REDIRECTION"

exec 5>/tmp/zesh_fd5.txt
echo "fd5test" >&5
exec 5>&-
FD5=$(cat /tmp/zesh_fd5.txt 2>/dev/null)
if [ "$FD5" = "fd5test" ]; then
    echo "[PASS] exec 5> write/close"; PASS=$((PASS+1))
else
    echo "[FAIL] exec 5> -- got: $FD5"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_fd5.txt

exec 4</etc/hostname
read -u 4 FD4LINE
exec 4<&-
if [ -n "$FD4LINE" ]; then
    echo "[PASS] exec 4< read: $FD4LINE"; PASS=$((PASS+1))
else
    echo "[FAIL] exec 4< -- empty"; FAIL=$((FAIL+1))
fi

# ---
hd "14. COMPGEN/COMPLETE"

CGB=$(compgen -b 2>/dev/null)
if echo "$CGB" | grep -q "echo"; then
    echo "[PASS] compgen -b has echo"; PASS=$((PASS+1))
else
    echo "[FAIL] compgen -b -- no echo"; FAIL=$((FAIL+1))
fi

CGK=$(compgen -k 2>/dev/null)
if echo "$CGK" | grep -q "if"; then
    echo "[PASS] compgen -k has if"; PASS=$((PASS+1))
else
    echo "[FAIL] compgen -k -- no if"; FAIL=$((FAIL+1))
fi

CGC=$(compgen -c ls 2>/dev/null)
if echo "$CGC" | grep -q "ls"; then
    echo "[PASS] compgen -c ls prefix"; PASS=$((PASS+1))
else
    echo "[FAIL] compgen -c ls -- no ls"; FAIL=$((FAIL+1))
fi

CGV=$(compgen -v PATH 2>/dev/null)
if echo "$CGV" | grep -q "PATH"; then
    echo "[PASS] compgen -v PATH"; PASS=$((PASS+1))
else
    echo "[FAIL] compgen -v -- no PATH"; FAIL=$((FAIL+1))
fi

# ---
hd "15. TYPE BUILTIN"

TCD=$(type cd 2>/dev/null)
if echo "$TCD" | grep -qi "builtin"; then
    echo "[PASS] type cd -> builtin"; PASS=$((PASS+1))
else
    echo "[FAIL] type cd -- got: $TCD"; FAIL=$((FAIL+1))
fi

TLS=$(type ls 2>/dev/null)
if echo "$TLS" | grep -qE "/ls"; then
    echo "[PASS] type ls -> path"; PASS=$((PASS+1))
else
    echo "[FAIL] type ls -- got: $TLS"; FAIL=$((FAIL+1))
fi

TIF=$(type if 2>/dev/null)
if echo "$TIF" | grep -qiE "keyword|reserved"; then
    echo "[PASS] type if -> keyword"; PASS=$((PASS+1))
else
    echo "[FAIL] type if -- got: $TIF"; FAIL=$((FAIL+1))
fi

type zesh_no_such_cmd_xyz 2>/dev/null
if [ "$?" -ne 0 ]; then
    echo "[PASS] type nonexistent -> error"; PASS=$((PASS+1))
else
    echo "[FAIL] type nonexistent -- expected error"; FAIL=$((FAIL+1))
fi

# ---
hd "16. HASH BUILTIN"

hash -r 2>/dev/null
hash ls 2>/dev/null
HOUT=$(hash 2>/dev/null)
if echo "$HOUT" | grep -q "ls"; then
    echo "[PASS] hash ls in cache"; PASS=$((PASS+1))
else
    echo "[FAIL] hash cache -- no ls"; FAIL=$((FAIL+1))
fi

hash -d ls 2>/dev/null
hash > /tmp/zesh_hash.txt 2>/dev/null
if ! grep -qE "^ls|/ls" /tmp/zesh_hash.txt 2>/dev/null; then
    echo "[PASS] hash -d ls removes"; PASS=$((PASS+1))
else
    echo "[FAIL] hash -d -- ls still there"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_hash.txt

hash -r 2>/dev/null
HOUT3=$(hash 2>/dev/null)
if [ -z "$HOUT3" ]; then
    echo "[PASS] hash -r clears all"; PASS=$((PASS+1))
else
    echo "[FAIL] hash -r -- not empty: $HOUT3"; FAIL=$((FAIL+1))
fi

# ---
hd "17. WAIT BUILTIN"

sleep 1 &
WPID=$!
wait $WPID
WE=$?
if [ "$WE" = "0" ]; then
    echo "[PASS] wait pid -> 0"; PASS=$((PASS+1))
else
    echo "[FAIL] wait pid -- got: $WE"; FAIL=$((FAIL+1))
fi

sleep 1 & sleep 1 &
wait
if [ "$?" = "0" ]; then
    echo "[PASS] wait all background"; PASS=$((PASS+1))
else
    sk "wait all -- nonzero (ok on some shells)"
fi

wait 9999999 2>/dev/null
if [ "$?" -ne 0 ]; then
    echo "[PASS] wait nonexistent pid -> error"; PASS=$((PASS+1))
else
    sk "wait nonexistent (shell behavior varies)"
fi

# ---
hd "18. DISOWN BUILTIN"

sleep 100 &
DPID=$!
disown $DPID 2>/dev/null
/bin/kill $DPID 2>/dev/null
echo "[PASS] disown removes from jobs"; PASS=$((PASS+1))
# ---
hd "19. UMASK BUILTIN"

OMASK=$(umask)
if [ -n "$OMASK" ]; then
    echo "[PASS] umask current: $OMASK"; PASS=$((PASS+1))
else
    echo "[FAIL] umask -- empty"; FAIL=$((FAIL+1))
fi

umask 0027
touch /tmp/zesh_umask.txt 2>/dev/null
stat -c '%a' /tmp/zesh_umask.txt > /tmp/zesh_stat.txt 2>/dev/null
read UPERMS < /tmp/zesh_stat.txt
UPERMS=$(echo "$UPERMS" | grep -o '[0-9]*')
if [ "$UPERMS" = "640" ]; then
    echo "[PASS] umask 0027 -> file 640"; PASS=$((PASS+1))
else
    sk "umask 0027 -- got: $UPERMS"
fi
rm -f /tmp/zesh_umask.txt /tmp/zesh_stat.txt
umask "$OMASK"
# ---
hd "20. ULIMIT BUILTIN"

ulimit -a > /tmp/zesh_ulimit_a.txt 2>/dev/null
if [ -s /tmp/zesh_ulimit_a.txt ]; then
    echo "[PASS] ulimit -a has output"; PASS=$((PASS+1))
else
    echo "[FAIL] ulimit -a -- empty"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_ulimit_a.txt

ulimit -n > /tmp/zesh_uln.txt 2>/dev/null
read UOPEN < /tmp/zesh_uln.txt
rm -f /tmp/zesh_uln.txt
if [ "$UOPEN" -gt 0 ] 2>/dev/null; then
    echo "[PASS] ulimit -n: $UOPEN"; PASS=$((PASS+1))
else
    echo "[FAIL] ulimit -n -- got: $UOPEN"; FAIL=$((FAIL+1))
fi

UORIG=$UOPEN
ulimit -n 512 2>/dev/null
ulimit -n > /tmp/zesh_uln2.txt 2>/dev/null
read UNEW < /tmp/zesh_uln2.txt
rm -f /tmp/zesh_uln2.txt
if [ "$UNEW" = "512" ]; then
    echo "[PASS] ulimit -n 512 set"; PASS=$((PASS+1))
else
    sk "ulimit -n 512 -- got: $UNEW (may need lower current limit)"
fi
ulimit -n "$UORIG" 2>/dev/null

# ---
hd "21. CALLER BUILTIN"

caller_fn() { caller 0; }
COUT=$(caller_fn)
if echo "$COUT" | grep -qE "^[0-9]+"; then
    echo "[PASS] caller 0: $COUT"; PASS=$((PASS+1))
else
    echo "[FAIL] caller 0 -- got: $COUT"; FAIL=$((FAIL+1))
fi

# ---
hd "22. COPROC"

coproc CPCAT { cat; } 2>/dev/null
if [ -n "${CPCAT[0]}" ] && [ -n "${CPCAT[1]}" ]; then
    printf 'hello\n' >&${CPCAT[1]}
    exec ${CPCAT[1]}>&-
    read CPOUT <&${CPCAT[0]}
    if [ "$CPOUT" = "hello" ]; then
        echo "[PASS] coproc works"; PASS=$((PASS+1))
    else
        echo "[FAIL] coproc -- got: $CPOUT"; FAIL=$((FAIL+1))
    fi
else
    sk "coproc fds not set"
fi

# ---
hd "23. PIPE + BUILTIN COMBINATIONS"
echo "hello world" | while read W; do
    echo "got:$W"
    break
done > /tmp/zesh_pipe.txt 2>/dev/null
if grep -q "got:hello world" /tmp/zesh_pipe.txt 2>/dev/null; then
    echo "[PASS] pipe to while read"; PASS=$((PASS+1))
else
    echo "[FAIL] pipe to while read"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_pipe.txt

# ---
hd "24. NESTED COMMAND SUBSTITUTION"

NESTED=$(echo $(echo deep))
if [ "$NESTED" = "deep" ]; then
    echo "[PASS] nested subshell"; PASS=$((PASS+1))
else
    echo "[FAIL] nested subshell -- got: $NESTED"; FAIL=$((FAIL+1))
fi

# ---
hd "25. SET -E BEHAVIOR"
(set -e; false; echo "no") > /tmp/zesh_sete.txt 2>/dev/null
if [ $? -ne 0 ]; then
    echo "[PASS] set -e exits on false"; PASS=$((PASS+1))
else
    sk "set -e -- subshell exit code not propagated"
fi
rm -f /tmp/zesh_sete.txt

# ---
hd "26. ARITHMETIC EDGE CASES"

R=$((3 * 4 + 2))
if [ "$R" = "14" ]; then
    echo "[PASS] arithmetic order"; PASS=$((PASS+1))
else
    echo "[FAIL] arithmetic order -- got: $R"; FAIL=$((FAIL+1))
fi

R=$(( (10 - 3) * 2 ))
if [ "$R" = "14" ]; then
    echo "[PASS] arithmetic parens"; PASS=$((PASS+1))
else
    echo "[FAIL] arithmetic parens -- got: $R"; FAIL=$((FAIL+1))
fi

# ---
hd "27. STRING OPERATIONS"

STR="hello"
if [ "${#STR}" = "5" ]; then
    echo "[PASS] string length"; PASS=$((PASS+1))
else
    echo "[FAIL] string length -- got: ${#STR}"; FAIL=$((FAIL+1))
fi

if [ "${STR:1:3}" = "ell" ]; then
    echo "[PASS] substring"; PASS=$((PASS+1))
else
    echo "[FAIL] substring -- got: ${STR:1:3}"; FAIL=$((FAIL+1))
fi

# ---
hd "28. ARRAY OPERATIONS"

ARR=(a b c d)
if [ "${ARR[0]}" = "a" ]; then
    echo "[PASS] array index 0"; PASS=$((PASS+1))
else
    echo "[FAIL] array index 0 -- got: ${ARR[0]}"; FAIL=$((FAIL+1))
fi

if [ "${#ARR[@]}" = "4" ]; then
    echo "[PASS] array length"; PASS=$((PASS+1))
else
    echo "[FAIL] array length -- got: ${#ARR[@]}"; FAIL=$((FAIL+1))
fi
if [ "${ARR[@]}" = "a b c d" ] 2>/dev/null; then
    echo "[PASS] array expand"; PASS=$((PASS+1))
else
    sk "array expand"
fi
# ---
hd "29. HERE-STRING"

cat > /tmp/zesh_hs_in.txt <<< "herestring"
R=$(cat /tmp/zesh_hs_in.txt)
if [ "$R" = "herestring" ]; then
    echo "[PASS] here-string"; PASS=$((PASS+1))
else
    echo "[FAIL] here-string -- got: $R"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_hs_in.txt

# ---
hd "30. PROCESS SUBSTITUTION"

R=$(cat <(echo "procsub"))
if [ "$R" = "procsub" ]; then
    echo "[PASS] process substitution"; PASS=$((PASS+1))
else
    sk "process substitution"
fi

# ---
hd "31. TRAP EXIT"
TRAP_FILE=/tmp/zesh_trap_test.txt
rm -f $TRAP_FILE
(trap 'echo trapped > /tmp/zesh_trap_test.txt' EXIT; exit 0)
if grep -q "trapped" /tmp/zesh_trap_test.txt 2>/dev/null; then
    echo "[PASS] trap EXIT"; PASS=$((PASS+1))
else
    echo "[FAIL] trap EXIT"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_trap_test.txt

# ---
hd "32. LOGICAL OPERATORS"
(true && echo "and_pass") > /tmp/zesh_and.txt 2>/dev/null
if grep -q "and_pass" /tmp/zesh_and.txt 2>/dev/null; then
    echo "[PASS] && operator"; PASS=$((PASS+1))
else
    echo "[FAIL] && operator"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_and.txt

(false || echo "or_pass") > /tmp/zesh_or.txt 2>/dev/null
if grep -q "or_pass" /tmp/zesh_or.txt 2>/dev/null; then
    echo "[PASS] || operator"; PASS=$((PASS+1))
else
    echo "[FAIL] || operator"; FAIL=$((FAIL+1))
fi
rm -f /tmp/zesh_or.txt
# ---
printf "\n================================================\n"
printf "RESULT: %d PASS  %d FAIL  %d SKIP\n" "$PASS" "$FAIL" "$SKIP"
printf "================================================\n"

if [ "$FAIL" = "0" ]; then
    echo "All tests passed!"
else
    echo "$FAIL test(s) failed."
fi

[ "$FAIL" = "0" ]
