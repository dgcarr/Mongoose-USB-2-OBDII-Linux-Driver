// List instructions that touch a struct offset, with the containing function, so a field's writers can be found.
// Usage: -postScript FindFieldWrites.java <output-file> <hex-offset>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.scalar.Scalar;
import java.io.PrintWriter;

public class FindFieldWrites extends GhidraScript {
    @Override
    protected void run() throws Exception {
        String[] args = getScriptArgs();
        long offset = Long.parseLong(args[1], 16);
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (Instruction ins : currentProgram.getListing().getInstructions(true)) {
                for (int op = 0; op < ins.getNumOperands(); ++op) {
                    for (Object o : ins.getOpObjects(op)) {
                        if (o instanceof Scalar && ((Scalar) o).getUnsignedValue() == offset) {
                            Function f = getFunctionContaining(ins.getAddress());
                            out.println(ins.getAddress() + "\t" + (f == null ? "-" : f.getEntryPoint()) + "\t" + ins);
                        }
                    }
                }
            }
        }
    }
}
