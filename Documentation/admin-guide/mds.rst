MDS - Microarchitectural Data Sampling)
=======================================

Microarchitectural Data Sampling is a side channel vulnerability that
allows an attacker to sample data that has been earlier used during
program execution. Internal buffers in the CPU may keep old data
for some limited time, which can the later be determined by an attacker
with side channel analysis. MDS can be used to occasionaly observe
some values accessed earlier, but it cannot be used to observe values
not recently touched by other code running on the same core.

It is difficult to target particular data on a system using MDS,
but attackers may be able to infer secrets by collecting
and analyzing large amounts of data. MDS does not modify
memory.

MDS consists of multiple sub-vulnerabilities:
Microarchitectural Store Buffer Data Sampling (MSBDS) (CVE-2018-12126)
Microarchitectual Fill Buffer Data Sampling (MFBDS) (CVE-2018-12130)
Microarchitectual Load Port Data (MLPDS) (CVE-2018-12127),
with the first leaking store data, and the second loads and sometimes
store data, and the third load data.

The effects and mitigations are similar for all three, so the Linux
kernel handles and reports them all as a single vulnerability called
MDS. This also reduces the number of acronyms in use.

Affected processors
-------------------

This vulnerability affects a wide range of Intel processors.
Not all CPUs are affected by all of the sub vulnerabilities,
however the kernel handles it always the same.

The vulnerability is not present in

    - Some Atoms (Bonnell, Saltwell, Goldmont, GoldmontPlus)

The kernel will automatically detect future CPUs with hardware
mitigations for these issues and disable any workarounds.

The kernel reports if the current CPU is vulnerable and any
mitigations used in

/sys/devices/system/cpu/vulnerabilities/mds

Kernel mitigation
-----------------

By default, the kernel automatically ensures no data leakage between
different processes, or between kernel threads and interrupt handlers
and user processes, or from any cryptographic code in the kernel.

It does not isolate kernel code that only touches data of the
current process.  If protecting such kernel code is desired,
mds=full can be specified.

The mitigation is automatically enabled, but can be further controlled
with the command line options documented below.

The mitigation can be done either with microcode support (requiring
updated microcode), or through software sequences on some CPUs.
On Skylake based CPUs only mitigation through microcode is supported.
In general microcode mitigation is preferred.

The microcode should be loaded at early boot using the initrd. Hot
updating microcode will not enable the mitigations.

Virtual machine mitigation
--------------------------

The mitigation is enabled by default and controlled by the same options
as L1TF cache clearing. See l1tf.rst for more details. In the default
setting

To enable the mitigation in guests it may be also needed to update
VM configurations to include the "MB_CLEAR" CPUID bit. This will
communicate to the guest kernel that the host has the microcode
with mitigations applied.

Kernel command line options
---------------------------

Normally the kernel selects reasonable defaults and no special configuration
is needed. The default behavior can be overriden by the mds= kernel
command line options.

These options can be specified in the boot loader. Any changes require a reboot.

When the system only runs trusted code, MDS mitigation can be disabled with
mds=off.

By default the kernel only clears CPU data after execution
that is known or likely to have touched user data of other processes,
or cryptographic data. This relies on code audits done in the
mainline Linux kernel. When running unaudited large out of tree code,
or binary drivers, who might violate these constraints it is possible
to use mds=full to always flush the CPU data on each kernel exit.

By default the kernel automatically selects using microcode based ("VERW")
mitigation, or software based mitigations, or no mitigation, based on the
CPUID information reported by the CPU. When running virtualized
inside a guest the CPUID information might be incomplete, or report
a different system.

In this case, and when the VM configuration cannot be fixed,
the following options can be used to select the right mitigation:

   - mds=off      Disable workarounds if the CPU is not affected.
   - mds=swclear  Host CPU doesn't have updated microcode.
                  Use software sequence applicable for Nehalem to IvyBridge
   - mds=swclearhsw
                  Host CPU doesn't have updated microcode.
                  Use software sequence applicable to Haswell and Broadwell
   - mds=verw     Host CPU has updated microcode
                  Use microcode based ("VERW") mitigation.

TBD describe SMT

References
----------

Fore more details on the kernel internal implementation of the MDS mitigations,
please see Documentation/clearcpu.txt

TBD Add URL for Intel white paper

TBD add reference to microcodes
