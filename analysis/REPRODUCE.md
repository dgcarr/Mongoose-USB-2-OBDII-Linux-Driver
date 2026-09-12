# Research reproduction

Run from the repository root. Ghidra 12.1.2 was used through the Snap headless launcher.

The Ghidra project is **not** stored in this repository: it is a ~20 MB regenerable
database. Recreate it once from the hash-pinned vendor DLL below, then run the
extraction commands against it. `analysis/ghidra_project/` and `analysis/ghidra_*.log`
are gitignored so a re-run does not commit them back.

```sh
mkdir -p analysis/ghidra_project
/snap/bin/ghidra.analyzeHeadless analysis/ghidra_project MongooseJLR -import vendor/driver/monpj432.dll
```

```sh
/snap/bin/ghidra.analyzeHeadless analysis/ghidra_project MongooseJLR -process monpj432.dll -readOnly -noanalysis -scriptPath "$PWD/analysis" -postScript ExtractSenders.java "$PWD/analysis/decompiled/senders"
/snap/bin/ghidra.analyzeHeadless analysis/ghidra_project MongooseJLR -process monpj432.dll -readOnly -noanalysis -scriptPath "$PWD/analysis" -postScript RefineFifoSignatures.java "$PWD/analysis/decompiled/refined_fifo"
```

Kernel project: `/home/dgcarr/snap/ghidra/common/mongoose-kernel-research/KernelTransport.gpr`.
It contains an analyzed import of dtmonpro.sys. Run LabelKernelWdf.java with the header path,
then ExtractKernel.java with an output directory. Use -readOnly to keep labels temporary.

Re-export labeled kernel functions from that existing project:

```sh
/snap/bin/ghidra.analyzeHeadless /home/dgcarr/snap/ghidra/common/mongoose-kernel-research KernelTransport -process dtmonpro.sys -readOnly -noanalysis -scriptPath "$PWD/analysis" -postScript LabelKernelWdf.java "$PWD/analysis/wdffuncenum-kmdf-1.15.h" -postScript ExtractKernel.java "$PWD/analysis/decompiled/kernel_labeled"
```

To recreate it on another machine, first create an empty project directory, then run
`analyzeHeadless <project-directory> KernelTransport -import <absolute-path-to-dtmonpro.sys>`.
The subsequent labeling/export command must use that project directory. This import analyzes
an on-disk binary; it does not load a kernel driver.

For other DLL exports, use `ExtractOpenSequence.java <output-directory> <address> ...`.
`ExtractInbound.java <output-directory>` extracts candidate channel callbacks. These scripts
run as post-scripts with the same read-only DLL command above. Vtable candidates require
call-site verification because of multiple inheritance.

WDF header source: https://raw.githubusercontent.com/microsoft/Windows-Driver-Frameworks/main/src/publicinc/wdf/kmdf/1.15/wdffuncenum.h
Retrieved 2026-09-13; original license retained. Relevant table entries were checked against binary call sites.

## SHA-256

- `vendor/driver/monpj432.dll`: `2710726fdd174f32b5e703e310cfce047a663451edbb3e35ca0c8eeff6929413`
- `vendor/driver/dtmonpro.sys`: `c3a5b6dbda2eb38cf4aa46c2c1660148f4ff0b7f51d96e1bcfe6875e29c12b4a`
- `analysis/wdffuncenum-kmdf-1.15.h`: `2042f92c5a6114d9a2d5443a2509e23e1ef0caebc52f5eacad37e25e1a0d4be7`

The Snap logs emit a filesystem cache free-space warning; extraction completion and saved outputs
were checked separately. No hardware is accessed by these scripts. Decompiled prototypes may remain
incomplete: consult assembly before treating parameter names or widths as definitive.
