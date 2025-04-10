#!/bin/sh
set -e
PERF=${PERF:-perf}
CC=${CC:-gcc}

if ! $PERF check feature -q libcapstone ; then exit 0 ; fi
if ! $PERF check feature -q dwarf ; then exit 0 ; fi

cleanup() {
	rm -f ttype$$.c ttype$$ type$$.data type$$.script type$$b.script type$$b.data type$$.report
}

cat >ttype$$.c <<EOL
volatile int v;
int main()
{
	int i;
	for (i = 0; i < 100000; i++)
		v++;
}
EOL

$CC ttype$$.c -O2 -g -o ttype$$
$PERF record -q -o type$$.data -e cycles:u ./ttype$$
$PERF annotate --code-with-type -i type$$.data --stdio > type$$.report
$PERF script -i type$$.data -F +disasm,+data_type > type$$.script

# XXX some output checking

if $PERF record -q -o /dev/null -b true ; then
	$PERF record -q -o type$$b.data -e cycles:u -b ./ttype$$
	$PERF script -i type$$b.data -F +brstackdisasm,+data_type > type$$b.script
fi

cleanup
