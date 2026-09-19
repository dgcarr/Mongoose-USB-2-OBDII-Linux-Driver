// Decompile every function that references one of the given addresses (call sites of a sender
// such as 1000dd00 cGetValue or 1000de10 cSetValue), and list them. Read-only.
//
//   analyzeHeadless <project> MongooseJLR -process monpj432.dll -readOnly -noanalysis \
//     -scriptPath analysis -postScript ExtractCallers.java <output-directory> <addr> [<addr>...]

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.FileWriter;
import java.util.Set;
import java.util.TreeSet;

public class ExtractCallers extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: ExtractCallers.java <output-directory> <address> [<address>...]");
            return;
        }
        File outDir = new File(args[0]);
        outDir.mkdirs();
        DecompInterface decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        Set<String> done = new TreeSet<>();
        try (FileWriter index = new FileWriter(new File(outDir, "callers.txt"))) {
            for (int i = 1; i < args.length; ++i) {
                Address target = toAddr(args[i]);
                index.write("== callers of " + args[i] + "\n");
                for (Reference ref : getReferencesTo(target)) {
                    Function caller = getFunctionContaining(ref.getFromAddress());
                    String where = caller == null ? "(no function)" : caller.getEntryPoint().toString();
                    index.write(ref.getFromAddress() + " in " + where + " " + ref.getReferenceType() + "\n");
                    if (caller == null || !done.add(caller.getEntryPoint().toString())) continue;
                    DecompileResults res = decomp.decompileFunction(caller, 120, new ConsoleTaskMonitor());
                    File out = new File(outDir, caller.getEntryPoint() + ".c");
                    try (FileWriter fw = new FileWriter(out)) {
                        fw.write("// " + caller.getName() + " @ " + caller.getEntryPoint()
                                 + " calls " + args[i] + "\n\n");
                        if (res != null && res.decompileCompleted()) fw.write(res.getDecompiledFunction().getC());
                        else fw.write("// decompile failed: " + (res != null ? res.getErrorMessage() : "no result") + "\n");
                    }
                }
            }
        }
        println("wrote " + done.size() + " functions to " + outDir);
    }
}
