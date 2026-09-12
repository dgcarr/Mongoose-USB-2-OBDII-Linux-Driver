// Inspect channel virtual callbacks used by the inbound dispatcher.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import java.io.*;
import java.util.*;
public class ExtractInbound extends GhidraScript {
    public void run() throws Exception {
        File directory = new File(getScriptArgs()[0]); directory.mkdirs();
        Set<Function> functions = new LinkedHashSet<>();
        try (FileWriter index = new FileWriter(new File(directory, "vtables.txt"))) {
            for (Symbol symbol : currentProgram.getSymbolTable().getAllSymbols(true)) {
                String name = symbol.getName(true);
                if (!name.endsWith("::vftable") || !name.matches("(?i).*(channel|CAN|ISO|J1850).*")) continue;
                index.write(name + " " + symbol.getAddress() + "\n");
                for (int offset : new int[]{0x8c, 0x90}) {
                    Function function = getFunctionAt(toAddr(Integer.toUnsignedLong(getInt(symbol.getAddress().add(offset)))));
                    index.write("  +" + Integer.toHexString(offset) + " " + (function == null ? "unresolved" : function.getEntryPoint()) + "\n");
                    if (function != null) functions.add(function);
                }
            }
        }
        DecompInterface decompiler = new DecompInterface(); decompiler.openProgram(currentProgram);
        try {
            for (Function function : functions) {
                var result = decompiler.decompileFunction(function, 60, monitor);
                if (!result.decompileCompleted()) continue;
                try (FileWriter output = new FileWriter(new File(directory, function.getEntryPoint() + ".c"))) {
                    output.write(result.getDecompiledFunction().getC());
                }
            }
        } finally { decompiler.dispose(); }
    }
}
