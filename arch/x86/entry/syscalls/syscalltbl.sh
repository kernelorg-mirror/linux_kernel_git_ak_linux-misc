#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

in="$1"
out="$2"

syscall_macro() {
    abi="$1"
    nr="$2"
    entry="$3"
    num="$4"

    # Entry can be either just a function name or "function/qualifier"
    real_entry="${entry%%/*}"
    if [ "$entry" = "$real_entry" ]; then
        qualifier=
    else
        qualifier=${entry#*/}
    fi

    echo "__SYSCALL_${abi}($nr, $real_entry, $qualifier, $num)"
}

emit() {
    abi="$1"
    nr="$2"
    entry="$3"
    compat="$4"
    num="$5"

    if [ "$abi" = "64" -a -n "$compat" ]; then
	echo "a compat entry for a 64-bit syscall makes no sense" >&2
	exit 1
    fi

    if [ -z "$compat" ]; then
	if [ -n "$entry" ]; then
	    syscall_macro "$abi" "$nr" "$entry" "$num"
	fi
    else
	echo "#ifdef CONFIG_X86_32"
	if [ -n "$entry" ]; then
	    syscall_macro "$abi" "$nr" "$entry" "$num"
	fi
	echo "#else"
	syscall_macro "$abi" "$nr" "$compat" "$num"
	echo "#endif"
    fi
}

grep '^[0-9]' "$in" | sort -n | (
    while read nr abi name entry compat num; do
	case "$compat" in
	[0-9]*) num="$compat" ; compat="" ;
	esac

	abi=`echo "$abi" | tr '[a-z]' '[A-Z]'`
	if [ "$abi" = "COMMON" -o "$abi" = "64" ]; then
	    # COMMON is the same as 64, except that we don't expect X32
	    # programs to use it.  Our expectation has nothing to do with
	    # any generated code, so treat them the same.
	    emit 64 "$nr" "$entry" "$compat" "$num"
	elif [ "$abi" = "X32" ]; then
	    # X32 is equivalent to 64 on an X32-compatible kernel.
	    echo "#ifdef CONFIG_X86_X32_ABI"
	    emit 64 "$nr" "$entry" "$compat" "$num"
	    echo "#endif"
	elif [ "$abi" = "I386" ]; then
	    emit "$abi" "$nr" "$entry" "$compat" "$num"
	else
	    echo "Unknown abi $abi" >&2
	    exit 1
	fi
    done
) > "$out"
