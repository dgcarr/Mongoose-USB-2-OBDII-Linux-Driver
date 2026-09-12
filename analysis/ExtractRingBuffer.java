// Deep-dive on the three still-unlabeled ring-buffer helper functions
// (FUN_1006b670 / FUN_1006b480 / FUN_1006b450) that likely contain the real
// frame serializer / checksum algorithm. Decompiles each, plus their callers
// (for invocation context: what buffer/length gets passed in) and their
// direct callees (in case the checksum math is one level deeper), plus a
// raw disassembly listing as a fallback for anything the decompiler mangles.

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.FileWriter;
import java.util.LinkedHashSet;
import java.util.Set;

public class ExtractRingBuffer extends GhidraScript {

    private DecompInterface decomp;
    private File outDir;

    private void decompileFunc(Function func, String tag) throws Exception {
        if (func == null) return;
        DecompileResults res = decomp.decompileFunction(func, 90, new ConsoleTaskMonitor());
        File outFile = new File(outDir, tag + "_" + func.getName() + "_" + func.getEntryPoint() + ".c");
        try (FileWriter fw = new FileWriter(outFile)) {
            fw.write("// " + func.getName() + " @ " + func.getEntryPoint() + " (" + tag + ")\n\n");
            if (res != null && res.decompileCompleted()) {
                fw.write(res.getDecompiledFunction().getC());
            } else {
                fw.write("// DECOMPILE FAILED: " + (res != null ? res.getErrorMessage() : "no result") + "\n");
            }
        }
        println("wrote " + outFile.getName());

        // raw disassembly fallback, always dumped alongside
        File asmFile = new File(outDir, tag + "_" + func.getName() + "_" + func.getEntryPoint() + ".asm");
        try (FileWriter fw = new FileWriter(asmFile)) {
            AddressSetView body = func.getBody();
            InstructionIterator it = currentProgram.getListing().getInstructions(body, true);
            while (it.hasNext()) {
                Instruction insn = it.next();
                fw.write(insn.getAddress() + "  " + insn.toString() + "\n");
            }
        }
        println("wrote " + asmFile.getName());
    }

    private Set<Function> callersOf(Function func) {
        Set<Function> callers = new LinkedHashSet<>();
        ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(func.getEntryPoint());
        while (refs.hasNext()) {
            Reference ref = refs.next();
            Function caller = currentProgram.getFunctionManager().getFunctionContaining(ref.getFromAddress());
            if (caller != null) callers.add(caller);
        }
        return callers;
    }

    private Set<Function> calleesOf(Function func) {
        Set<Function> callees = new LinkedHashSet<>();
        AddressSetView body = func.getBody();
        InstructionIterator it = currentProgram.getListing().getInstructions(body, true);
        while (it.hasNext()) {
            Instruction insn = it.next();
            for (Reference ref : insn.getReferencesFrom()) {
                if (ref.getReferenceType().isCall()) {
                    Function callee = currentProgram.getFunctionManager().getFunctionAt(ref.getToAddress());
                    if (callee != null) callees.add(callee);
                }
            }
        }
        return callees;
    }

    @Override
    protected void run() throws Exception {
        outDir = new File("/home/dgcarr/mongoose_driver/analysis/decompiled/ringbuffer");
        if (!outDir.exists()) outDir.mkdirs();

        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String[] targets = {"1006b670", "1006b480", "1006b450"};

        for (String a : targets) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
            Function func = currentProgram.getFunctionManager().getFunctionAt(addr);
            if (func == null) {
                println("no function at " + a);
                continue;
            }

            decompileFunc(func, "target");

            for (Function caller : callersOf(func)) {
                decompileFunc(caller, "caller_of_" + a);
            }
            for (Function callee : calleesOf(func)) {
                decompileFunc(callee, "callee_of_" + a);
            }
        }
    }
}
