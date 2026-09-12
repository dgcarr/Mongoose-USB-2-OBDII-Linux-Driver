// Decompile specific functions by address (for following call chains discovered during analysis).

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.Address;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.FileWriter;

public class ExtractByAddress extends GhidraScript {

    private DecompInterface decomp;

    private void decompileFunc(Function func, File outFile) throws Exception {
        DecompileResults res = decomp.decompileFunction(func, 60, new ConsoleTaskMonitor());
        if (res != null && res.decompileCompleted()) {
            try (FileWriter fw = new FileWriter(outFile)) {
                fw.write("// " + func.getName() + " @ " + func.getEntryPoint() + "\n\n");
                fw.write(res.getDecompiledFunction().getC());
            }
            println("wrote " + outFile.getAbsolutePath());
        } else {
            println("FAILED to decompile " + func.getName() + ": " +
                (res != null ? res.getErrorMessage() : "no result"));
        }
    }

    @Override
    protected void run() throws Exception {
        File outDir = new File("/home/dgcarr/mongoose_driver/analysis/decompiled");
        if (!outDir.exists()) outDir.mkdirs();

        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        String[] addrs = {
            "1006c6e0", // frame reader called by wire-protocol response matcher
            "1006d9b0", // sibling of the response-too-small-for-wireframe checker
            "1002a700", // 5 Baud Init
            "1003b080", // cJumpToFirmware
            "1001b800", // large strref match, likely core wireframe class method
        };

        for (String a : addrs) {
            Address addr = currentProgram.getAddressFactory().getDefaultAddressSpace().getAddress(a);
            Function func = currentProgram.getFunctionManager().getFunctionAt(addr);
            if (func == null) {
                println("no function at " + a);
                continue;
            }
            decompileFunc(func, new File(outDir, "byaddr_" + a + "_" + func.getName().replace("/", "_") + ".c"));
        }
    }
}
