#!/bin/bash
# some tests for type resolution
set -e
PERF=${PERF:-perf}
CC=${CC:-gcc}

if ! $PERF check feature -q libcapstone ; then
	echo "SKIP: libcapstone is not supported"
	exit 0
fi
if ! $PERF check feature -q dwarf ; then
	echo "SKIP: dwarf is not supported"
	exit 0
fi

cleanup() {
	if [ -z "$TYPE_KEEP_OUTPUT" ] ; then
		rm -f type$$ type$$.{c,data,report,annotate,script} \
		       	type$$b.{data,report,annotate,script}
	fi
}

trap_cleanup() {
	echo FAILED
	cleanup
	exit 1
}

trap trap_cleanup EXIT TERM INT

cat >type$$.c <<EOL
int v;
int main()
{
	int i;
	for (i = 0; i < 4000000; i++) {
		v++;
		asm("" :: "r" (v) : "memory");
	}
}
EOL

$CC type$$.c -O2 -g -o type$$
$PERF record -c 10000 -q -o type$$.data -e cycles:u ./type$$
$PERF annotate --code-with-type -i type$$.data --stdio main > type$$.annotate
$PERF report --sort type,typeoff,typecln -i type$$.data > type$$.report
$PERF script -i type$$.data -F +disasm,+data_type > type$$.script

grep -q int type$$.report
grep -q "data-type: int" type$$.annotate

# XXX int for script

# needs more samples:
#grep -q "stack operation" type$$.script

echo "script disasm with data_type did not crash [Success]"

if $PERF record -q -c 10000 -o /dev/null -b true ; then
	$PERF record -q -o type$$b.data -e cycles:u -b ./type$$
	$PERF report --sort type,typeoff,typecln -i type$$b.data > type$$b.report
	$PERF annotate --code-with-type -i type$$b.data --stdio main > type$$b.annotate
	$PERF script -i type$$b.data -F +brstackdisasm,+data_type > type$$b.script

	# does not report anything?
	#grep -q int type$$b.report
	grep -q "data-type: int" type$$b.annotate

	echo "script brstackdisasm with data-type did not crash [Success]"
else
	echo "SKIP: branch recording is not supported"
fi

cleanup
trap - EXIT TERM INT
