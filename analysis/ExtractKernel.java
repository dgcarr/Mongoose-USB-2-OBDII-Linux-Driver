// Export the small kernel driver's analyzed functions for transport inspection.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.program.model.listing.Function;
import java.io.*;

public class ExtractKernel extends GhidraScript {
    public void run() throws Exception {
        File directory = new File(getScriptArgs()[0]);
        directory.mkdirs();
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try (FileWriter index = new FileWriter(new File(directory, "functions.txt"))) {
            for (Function function : currentProgram.getFunctionManager().getFunctions(true)) {
                if (function.isExternal() || function.isThunk()) continue;
                index.write(function.getEntryPoint() + " " + function.getName() + "\n");
                var result = decompiler.decompileFunction(function, 20, monitor);
                if (!result.decompileCompleted()) { println("FAILED " + function.getEntryPoint()); continue; }
                try (FileWriter output = new FileWriter(new File(directory, function.getEntryPoint() + ".c"))) {
                    output.write(result.getDecompiledFunction().getC());
                }
            }
        } finally { decompiler.dispose(); }
    }
}
