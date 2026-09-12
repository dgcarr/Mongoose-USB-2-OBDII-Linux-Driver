// Export functions that directly call the control-frame sender, with assembly evidence.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import java.io.File;
import java.io.FileWriter;
import java.util.LinkedHashSet;

public class ExtractSenders extends GhidraScript {
    public void run() throws Exception {
        File directory = new File(getScriptArgs()[0]);
        directory.mkdirs();
        LinkedHashSet<Function> functions = new LinkedHashSet<>();
        for (Reference reference : getReferencesTo(toAddr("1006e180"))) {
            Function function = getFunctionContaining(reference.getFromAddress());
            if (function != null) functions.add(function);
        }
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try {
            for (Function function : functions) {
                monitor.checkCancelled();
                DecompileResults result = decompiler.decompileFunction(function, 30, monitor);
                if (result.decompileCompleted()) {
                    try (FileWriter output = new FileWriter(new File(directory, function.getEntryPoint() + ".c"))) {
                        output.write(result.getDecompiledFunction().getC());
                    }
                } else println("FAILED " + function.getEntryPoint());
                try (FileWriter output = new FileWriter(new File(directory, function.getEntryPoint() + ".asm"))) {
                    InstructionIterator instructions = currentProgram.getListing().getInstructions(function.getBody(), true);
                    while (instructions.hasNext()) {
                        Instruction instruction = instructions.next();
                        output.write(instruction.getAddress() + " " + instruction + "\n");
                    }
                }
            }
            println("Exported " + functions.size() + " senders");
        } finally { decompiler.dispose(); }
    }
}
