// Map the binary's 396-entry KMDF table using Microsoft's published function indexes.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.SourceType;
import java.nio.file.*;
import java.util.regex.*;
public class LabelKernelWdf extends GhidraScript {
    public void run() throws Exception {
        Pattern pattern = Pattern.compile("(Wdf\\w+)TableIndex\\s*=\\s*(\\d+)");
        for (String line : Files.readAllLines(Path.of(getScriptArgs()[0]))) {
            Matcher match = pattern.matcher(line);
            if (match.find()) {
                int index = Integer.parseInt(match.group(2));
                if (index < 396) currentProgram.getSymbolTable().createLabel(
                    toAddr(0x141c0L + 8L * index), match.group(1), SourceType.USER_DEFINED).setPrimary();
            }
        }
        getFunctionAt(toAddr("11870")).setName("EvtIoRead", SourceType.USER_DEFINED);
        getFunctionAt(toAddr("11df4")).setName("EvtIoWrite", SourceType.USER_DEFINED);
    }
}
