// Dump exports (address -> every symbol name there) and decompiled C for every non-thunk function.
// usage (headless): -postScript ExportAll.java <out dir>
//@category LP2Net
import java.io.File;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;

public class ExportAll extends GhidraScript {
	@Override
	public void run() throws Exception {
		File outDir = new File(getScriptArgs()[0]);
		outDir.mkdirs();
		String prog = currentProgram.getName();
		SymbolTable st = currentProgram.getSymbolTable();

		try (PrintWriter pw = new PrintWriter(new File(outDir, prog + ".exports.txt"), "UTF-8")) {
			AddressIterator it = st.getExternalEntryPointIterator();
			while (it.hasNext()) {
				Address a = it.next();
				StringBuilder names = new StringBuilder();
				for (Symbol s : st.getSymbols(a)) {
					names.append(s.getName(true)).append(" | ");
				}
				Function f = getFunctionAt(a);
				pw.println(a + "  " + names + (f != null ? "fn=" + f.getName(true) + " sig=" + f.getSignature() : ""));
			}
		}

		DecompInterface di = new DecompInterface();
		di.openProgram(currentProgram);
		try (PrintWriter pw = new PrintWriter(new File(outDir, prog + ".decomp.c"), "UTF-8")) {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				if (monitor.isCancelled()) {
					break;
				}
				if (f.isThunk() || f.isExternal()) {
					continue;
				}
				DecompileResults r = di.decompileFunction(f, 60, monitor);
				pw.println("// ==== " + f.getEntryPoint() + " " + f.getName(true));
				pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC()
						: "// decompile failed: " + r.getErrorMessage());
			}
		}
		di.dispose();
	}
}
