# NT 3.1 multiprocessor experiment: four-CPU desktop and XMM test verified

After the earlier deferral, the user requested another attempt on a VHD copy.
The missing NT 3.1 sparse-PFN guard was added to the supplied GUI patcher. Its
rebuilt executable ran twice on a fresh VHD, installed the supplied loaders,
retained original loader backups, and generated a kernel that boots to the
NT 3.1 desktop. GetSystemInfo reports four processors, active mask 0xF; a native
worker thread completes. The generated kernel also passes 320 PFN guard cases.

QEMU uses TCG thread=multi, Pentium III + SSE2, four CPUs and 128 MB. VAPIC ROM
rewriting is disabled with -global apic.vapic=false. The old loader ignored the
custom kernel/HAL selection. The newer loaders select them correctly, but the
unguarded kernel faults on a sparse PFN record for local-APIC MMIO. Both issues
are documented in the separate NTCPUPAE fix package.

The XMM research adapter then passed four-worker migration testing on the
GUI-generated kernel: 342 affinity rounds, zero affinity errors and zero
XMM/MXCSR/x87 mismatches. The fixed CPU-coverage metric observes all four CPUs.
Raw log: nt31-gui-kernel-xmm-migration.txt. Earlier standalone-guard testing
passed 227 rounds; its log is retained as nt31-four-cpu-xmm-migration.txt.

A further 640-worker-lifetime test passed with zero mismatches. Driver metrics
showed 24 active slots, peak 41, retired 647 and CPU mask 0xF afterwards.

This establishes the reported bounded QEMU experiment, not general SMP driver
correctness. The hook installation rendezvous, finite table and other OS adapters
remain production blockers. YMM/ZMM preservation is not implemented.
