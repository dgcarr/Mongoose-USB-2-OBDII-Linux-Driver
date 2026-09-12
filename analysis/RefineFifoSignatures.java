// Apply assembly-derived register signatures in memory; run with -readOnly.
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.SourceType;
import java.io.*;
import java.util.*;

public class RefineFifoSignatures extends GhidraScript {
    private Parameter reg(String name, DataType type, String register) throws Exception {
        return new ParameterImpl(name, type, new VariableStorage(currentProgram, currentProgram.getRegister(register)), currentProgram);
    }
    private Parameter stack(String name, DataType type, int offset) throws Exception {
        return new ParameterImpl(name, type, offset, currentProgram);
    }
    private void signature(String address, String name, DataType result, Parameter... parameters) throws Exception {
        Function function = getFunctionAt(toAddr(address));
        function.setName(name, SourceType.USER_DEFINED);
        function.updateFunction(null, new ReturnParameterImpl(result, currentProgram),
            Arrays.asList(parameters), Function.FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
    }
    public void run() throws Exception {
        DataType ptr = new PointerDataType(DWordDataType.dataType, 4);
        DataType word = DWordDataType.dataType;
        signature("1006b480", "fifo_cursor_add", ptr,
            reg("cursor", ptr, "EAX"), reg("count", word, "ECX"), reg("result", ptr, "EBX"));
        signature("1006b450", "fifo_consume", VoidDataType.dataType,
            reg("fifo", ptr, "EAX"), reg("count", word, "EDX"));
        signature("1006c660", "fifo_append_record", VoidDataType.dataType,
            reg("length", word, "EAX"), reg("queue", ptr, "EDI"), stack("body", ptr, 4));
        signature("1006b5f0", "fifo_insert_range", VoidDataType.dataType,
            reg("fifo", ptr, "EAX"), reg("source_end", ptr, "ECX"),
            stack("position", QWordDataType.dataType, 4), stack("source_begin", ptr, 12));
        File directory = new File(getScriptArgs()[0]);
        directory.mkdirs();
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);
        try {
            for (String address : new String[]{"1006b480", "1006c660", "1006c6e0", "1006b5f0", "1006c5e0"}) {
                var result = decompiler.decompileFunction(getFunctionAt(toAddr(address)), 60, monitor);
                if (!result.decompileCompleted()) throw new Exception(result.getErrorMessage());
                try (FileWriter output = new FileWriter(new File(directory, address + ".c"))) {
                    output.write(result.getDecompiledFunction().getC());
                }
            }
        } finally { decompiler.dispose(); }
    }
}
