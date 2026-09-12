// Ghidra headless post-script (Java GhidraScript).
// Decompiles our target J2534 exports plus any function referencing
// protocol-relevant strings, writing readable C output to disk.

import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.address.Address;
import ghidra.util.task.ConsoleTaskMonitor;

import java.io.File;
import java.io.FileWriter;
import java.util.HashSet;
import java.util.Set;

public class ExtractProtocol extends GhidraScript {

    private DecompInterface decomp;
    private File outDir;

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
        outDir = new File("/home/dgcarr/mongoose_driver/analysis/decompiled");
        if (!outDir.exists()) outDir.mkdirs();

        decomp = new DecompInterface();
        decomp.openProgram(currentProgram);

        FunctionManager fm = currentProgram.getFunctionManager();
        SymbolTable symTable = currentProgram.getSymbolTable();

        String[] targetNames = {
            "PassThruOpen", "PassThruClose", "PassThruConnect", "PassThruDisconnect",
            "PassThruReadMsgs", "PassThruWriteMsgs", "PassThruIoctl",
            "PassThruReadVersion", "PassThruReadDetails"
        };

        for (String name : targetNames) {
            for (Symbol sym : symTable.getSymbols(name)) {
                Function func = fm.getFunctionAt(sym.getAddress());
                if (func != null) {
                    decompileFunc(func, new File(outDir, name + ".c"));
                }
            }
        }

        String[] interesting = {
            "wireframe", "cWriteSerialNumber", "cJumpToFirmware", "DT_ISO_INIT_BAUD",
            "DT_SNIFF_MODE", "CHECKSUM_DISABLED", "5 Baud", "wire protocol", "BT Serial"
        };

        Set<Address> seen = new HashSet<>();
        int stringHits = 0;

        DataIterator dataIter = currentProgram.getListing().getDefinedData(true);
        while (dataIter.hasNext()) {
            Data d = dataIter.next();
            if (!d.hasStringValue()) continue;
            Object val = d.getValue();
            if (val == null) continue;
            String sval = val.toString();

            boolean matched = false;
            for (String token : interesting) {
                if (sval.contains(token)) { matched = true; break; }
            }
            if (!matched) continue;
            stringHits++;

            for (Reference ref : getReferencesTo(d.getAddress())) {
                Address fromAddr = ref.getFromAddress();
                Function func = fm.getFunctionContaining(fromAddr);
                if (func != null && !seen.contains(func.getEntryPoint())) {
                    seen.add(func.getEntryPoint());
                    String safeName = func.getName().replace("/", "_");
                    File outFile = new File(outDir, "strref_" + safeName + "_" + func.getEntryPoint() + ".c");
                    println("string \"" + sval.substring(0, Math.min(60, sval.length())) +
                        "\" -> function " + func.getName() + " @ " + func.getEntryPoint());
                    decompileFunc(func, outFile);
                }
            }
        }

        println("Matched " + stringHits + " interesting string occurrences, decompiled " +
            seen.size() + " referencing functions");
    }
}
