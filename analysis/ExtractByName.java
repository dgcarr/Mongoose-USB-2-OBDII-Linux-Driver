// Decompile functions by symbol name (for example an export such as PassThruStopPeriodicMsg) and their
// direct callees, and list the callees. Read-only.
//
//   analyzeHeadless <project> MongooseJLR -process monpj432.dll -readOnly -noanalysis \
//     -scriptPath analysis -postScript ExtractByName.java <output-directory> <name> [<name>...]

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.FileWriter;
import java.util.Set;
import java.util.TreeSet;

public class ExtractByName extends GhidraScript {
    private DecompInterface decomp;
    private File outDir;
    private final Set<String> done = new TreeSet<>();

    private void dump(Function function, String why, FileWriter index) throws Exception {
        if (function == null || function.isThunk() && function.getThunkedFunction(true) == null) return;
        Function target = function.isThunk() ? function.getThunkedFunction(true) : function;
        if (!done.add(target.getEntryPoint().toString())) return;
        DecompileResults res = decomp.decompileFunction(target, 120, new ConsoleTaskMonitor());
        File out = new File(outDir, target.getEntryPoint() + ".c");
        try (FileWriter fw = new FileWriter(out)) {
            fw.write("// " + target.getName() + " @ " + target.getEntryPoint() + " (" + why + ")\n\n");
            if (res != null && res.decompileCompleted()) fw.write(res.getDecompiledFunction().getC());
            else fw.write("// decompile failed: " + (res != null ? res.getErrorMessage() : "no result") + "\n");
        }
        index.write(target.getEntryPoint() + " " + target.getName() + " <- " + why + "\n");
    }

    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 2) {
            println("usage: ExtractByName.java <output-directory> <name> [<name>...]");
            return;
        }
        outDir = new File(args[0]);
        outDir.mkdirs();
        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);
        try (FileWriter index = new FileWriter(new File(outDir, "index.txt"), true)) {
            for (int i = 1; i < args.length; ++i) {
                boolean found = false;
                // An eight-digit hex argument is an address rather than a symbol name.
                java.util.List<Function> targets = new java.util.ArrayList<>();
                if (args[i].matches("[0-9a-fA-F]{8}")) {
                    Function at = getFunctionAt(toAddr(args[i]));
                    if (at != null) targets.add(at);
                } else {
                    for (Symbol symbol : currentProgram.getSymbolTable().getSymbols(args[i])) {
                        Function at = getFunctionAt(symbol.getAddress());
                        if (at != null) targets.add(at);
                    }
                }
                for (Function function : targets) {
                    found = true;
                    dump(function, "named " + args[i], index);
                    Function target = function.isThunk() ? function.getThunkedFunction(true) : function;
                    if (target == null) continue;
                    int callees = 0;
                    for (Function callee : target.getCalledFunctions(new ConsoleTaskMonitor())) {
                        if (callees++ >= 40) break;
                        dump(callee, "callee of " + args[i], index);
                    }
                }
                if (!found) index.write("NOT FOUND: " + args[i] + "\n");
            }
        }
        println("wrote " + done.size() + " functions to " + outDir);
    }
}
