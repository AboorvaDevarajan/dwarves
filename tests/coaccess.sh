#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Synthetic co-access profile: hot scalars + anonymous union alias resolution.

source test_lib.sh

outdir=$(make_tmpdir)
trap cleanup EXIT

title_log "Coaccess reorganization (synthetic edges + anonymous union)."

srcdir=$(dirname "$0")/data
obj=$(make_tmpobj)
coaccess=$(make_tmpfile)
pretty=$(make_tmpsrc)

gcc -g -c -o "$obj" "$srcdir/coaccess_hotstruct.c" || test_fail

cat > "$coaccess" <<'EOF'
hot_x hot_y 1000
donor hot_x 500
curr hot_y 500
cold_a cold_b 1
EOF

pahole --coaccess="$coaccess" --coaccess_top_pairs=0 -c 64 -C coaccess_test "$obj" > "$pretty" || test_fail

member_offset()
{
	sed -n "s/.*$1.*\/\* *\([0-9][0-9]*\).*/\1/p" "$pretty" | head -1
}

hot_x_off=$(member_offset hot_x)
hot_y_off=$(member_offset hot_y)
donor_off=$(member_offset donor)
cold_a_off=$(member_offset cold_a)

if [ "$hot_x_off" != "0" ]; then
	error_log "hot_x expected offset 0, got $hot_x_off"
	cat "$pretty"
	test_fail
fi
if [ "$hot_y_off" != "4" ]; then
	error_log "hot_y expected offset 4, got $hot_y_off"
	cat "$pretty"
	test_fail
fi
if [ "$donor_off" != "8" ]; then
	error_log "anonymous union (donor) expected offset 8, got $donor_off"
	cat "$pretty"
	test_fail
fi
if [ -z "$cold_a_off" ] || [ "$cold_a_off" -lt 12 ]; then
	error_log "cold_a should follow hot cluster (got offset $cold_a_off)"
	cat "$pretty"
	test_fail
fi

info_log "hot_x/hot_y/union on line 0; cold tail at +$cold_a_off"

# Scoring mechanism: with a 16-byte line the baseline splits the hot cluster
# (donor/curr land on line 1, away from hot_x/hot_y on line 0). The reorder
# must pull them together and the emitted co-access score must reflect that.
score=$(make_tmpsrc)
pahole --coaccess="$coaccess" --coaccess_top_pairs=0 -c 16 -C coaccess_test "$obj" > "$score" || test_fail

if ! grep -q "co-access reorder score" "$score"; then
	error_log "expected a co-access reorder score block"
	cat "$score"
	test_fail
fi

before_loc=$(sed -n 's/.*locality \([0-9.][0-9.]*\)% ->.*/\1/p' "$score" | head -1)
after_loc=$(sed -n 's/.*-> \([0-9.][0-9.]*\)%.*/\1/p' "$score" | head -1)

if [ -z "$before_loc" ] || [ -z "$after_loc" ]; then
	error_log "could not parse locality from score block"
	cat "$score"
	test_fail
fi

# after must be a strict improvement and reach full locality (all hot pairs
# on one 16-byte line).
improved=$(awk -v b="$before_loc" -v a="$after_loc" \
	'BEGIN { print (a > b && a >= 99.9) ? "yes" : "no" }')
if [ "$improved" != "yes" ]; then
	error_log "locality did not improve as expected ($before_loc% -> $after_loc%)"
	cat "$score"
	test_fail
fi

info_log "co-access locality $before_loc% -> $after_loc% at 16 B lines"
test_pass
