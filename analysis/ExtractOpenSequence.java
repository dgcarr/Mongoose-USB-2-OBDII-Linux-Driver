// Export selected functions and their call sites without modifying the program.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.File;
import java.io.FileWriter;

public class ExtractOpenSequence extends GhidraScript {
    public void run() throws Exception {
        File directory = new File(getScriptArgs()[0]);
        directory.mkdirs();
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (FileWriter index = new FileWriter(new File(directory, "references.txt"))) {
            for (int i = 1; i < getScriptArgs().length; i++) {
                Function function = getFunctionContaining(toAddr(getScriptArgs()[i]));
                if (function == null) { println("No function: " + getScriptArgs()[i]); continue; }
                index.write(function.getEntryPoint() + " " + function.getName() + "\n");
                for (Reference reference : getReferencesTo(function.getEntryPoint())) {
                    Function caller = getFunctionContaining(reference.getFromAddress());
                    index.write("  reference " + reference.getFromAddress() + " " + reference.getReferenceType()
                        + " caller " + (caller == null ? "data" : caller.getEntryPoint()) + "\n");
                }
                DecompileResults result = decompiler.decompileFunction(function, 90, monitor);
                if (!result.decompileCompleted()) throw new Exception(result.getErrorMessage());
                try (FileWriter output = new FileWriter(new File(directory, function.getEntryPoint() + ".c"))) {
                    output.write(result.getDecompiledFunction().getC());
                }
                println("Exported " + function.getEntryPoint());
            }
        } finally { decompiler.dispose(); }
    }
}
