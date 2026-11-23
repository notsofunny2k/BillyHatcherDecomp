// Import decomp-toolkit symbols.txt into the current program.
// Creates functions (with dtk's boundaries) and labels, and applies names.
// Generic names (fn_XXXXXXXX / lbl_XXXXXXXX) are applied too, so Ghidra and
// the dtk asm/objdiff output use the same identifiers.
//
// Headless:  -postScript ImportDtkSymbols.java <path/to/symbols.txt>
// GUI:       run from Script Manager; asks for the file.
//@category GameCube
//@author BillyHatcherDecomp

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class ImportDtkSymbols extends GhidraScript {

    private static final Pattern LINE = Pattern.compile(
        "^(\\S+) = (\\.?\\w+):0x([0-9A-Fa-f]+); // type:(\\w+)(?: size:0x([0-9A-Fa-f]+))?(.*)$");

    @Override
    protected void run() throws Exception {
        File file;
        String[] args = getScriptArgs();
        if (args.length > 0) {
            file = new File(args[0]);
        } else {
            file = askFile("symbols.txt de decomp-toolkit", "Importar");
        }

        int fnCreated = 0, fnRenamed = 0, labels = 0, failed = 0;
        try (BufferedReader br = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = br.readLine()) != null) {
                if (monitor.isCancelled()) {
                    break;
                }
                Matcher m = LINE.matcher(line);
                if (!m.matches()) {
                    continue;
                }
                String name = m.group(1);
                long addrVal = Long.parseLong(m.group(3), 16);
                String type = m.group(4);
                long size = m.group(5) != null ? Long.parseLong(m.group(5), 16) : 0;
                if (name.startsWith("\"") || name.contains("@")) {
                    // compiler-generated names (e.g. "@1234") aren't valid labels everywhere
                    name = name.replace("\"", "").replace("@", "at_");
                }
                Address addr = toAddr(addrVal);
                if (currentProgram.getMemory().getBlock(addr) == null) {
                    failed++;
                    continue;
                }
                try {
                    if (type.equals("function")) {
                        Function f = getFunctionAt(addr);
                        if (f == null) {
                            disassemble(addr);
                            if (size > 0) {
                                AddressSet body = new AddressSet(addr, addr.add(size - 1));
                                f = currentProgram.getFunctionManager()
                                    .createFunction(name, addr, body, SourceType.IMPORTED);
                            }
                            else {
                                f = createFunction(addr, name);
                            }
                            if (f != null) {
                                fnCreated++;
                            }
                            else {
                                failed++;
                            }
                        }
                        else if (!f.getName().equals(name)) {
                            f.setName(name, SourceType.IMPORTED);
                            fnRenamed++;
                        }
                    }
                    else {
                        Symbol s = getSymbolAt(addr);
                        if (s == null || !s.getName().equals(name)) {
                            createLabel(addr, name, true, SourceType.IMPORTED);
                            labels++;
                        }
                    }
                }
                catch (Exception e) {
                    failed++;
                }
            }
        }
        println(String.format(
            "ImportDtkSymbols: functions created=%d renamed=%d, labels=%d, failed=%d",
            fnCreated, fnRenamed, labels, failed));
    }
}
