// For each xlive.dll import: every call site and its containing function, then decompile each caller.
// Also lists strings that look network/session related, with the functions that use them.
// usage (headless): -postScript XliveCallers.java <out dir>
//@category LP2Net
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.regex.Pattern;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.symbol.ExternalLocation;
import ghidra.program.model.symbol.ExternalLocationIterator;
import ghidra.program.model.symbol.ExternalManager;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

public class XliveCallers extends GhidraScript {
	private static final Pattern NET = Pattern.compile(
		"(?i)(xnet|xsession|xlive|xuser|xsocket|session|lobby|system ?link|syslink|matchmak|qos|host|join|" +
			"invite|online|offline|\\blan\\b|network|netw|connect|player ?match|ranked|party|search|\\bnat\\b)");

	private ReferenceManager rm;
	private FunctionManager fm;

	@Override
	public void run() throws Exception {
		File outDir = new File(getScriptArgs()[0]);
		outDir.mkdirs();
		String prog = currentProgram.getName();
		rm = currentProgram.getReferenceManager();
		fm = currentProgram.getFunctionManager();
		Comparator<Function> byEntry = Comparator.comparing(Function::getEntryPoint);
		Set<Function> callers = new TreeSet<>(byEntry);

		try (PrintWriter pw = new PrintWriter(new File(outDir, prog + ".xlive_callsites.txt"), "UTF-8")) {
			ExternalManager em = currentProgram.getExternalManager();
			for (String lib : em.getExternalLibraryNames()) {
				if (!lib.equalsIgnoreCase("xlive.dll")) {
					continue;
				}
				Map<String, List<Address>> sitesByName = new TreeMap<>();
				ExternalLocationIterator eli = em.getExternalLocations(lib);
				while (eli.hasNext()) {
					ExternalLocation loc = eli.next();
					Set<Address> sites = new TreeSet<>();
					collect(loc.getExternalSpaceAddress(), sites, 0);
					sitesByName.put(loc.getLabel(), new ArrayList<>(sites));
				}
				for (Map.Entry<String, List<Address>> e : sitesByName.entrySet()) {
					pw.println(e.getKey() + "  (" + e.getValue().size() + " sites)");
					for (Address a : e.getValue()) {
						Function f = fm.getFunctionContaining(a);
						pw.println("    " + a + "  in " + (f == null ? "?" : f.getEntryPoint() + " " + f.getName()));
						if (f != null) {
							callers.add(f);
						}
					}
				}
			}
		}

		Set<Function> stringUsers = new TreeSet<>(byEntry);
		try (PrintWriter pw = new PrintWriter(new File(outDir, prog + ".net_strings.txt"), "UTF-8")) {
			DataIterator di = currentProgram.getListing().getDefinedData(true);
			while (di.hasNext() && !monitor.isCancelled()) {
				Data d = di.next();
				if (!d.hasStringValue()) {
					continue;
				}
				String s = String.valueOf(d.getValue());
				if (!NET.matcher(s).find()) {
					continue;
				}
				StringBuilder users = new StringBuilder();
				for (Reference r : rm.getReferencesTo(d.getAddress())) {
					Function f = fm.getFunctionContaining(r.getFromAddress());
					if (f != null) {
						users.append(' ').append(f.getEntryPoint());
						stringUsers.add(f);
					}
				}
				pw.println(d.getAddress() + "  \"" + s.replace("\n", "\\n") + "\"  <-" + users);
			}
		}

		DecompInterface dec = new DecompInterface();
		dec.openProgram(currentProgram);
		decompileAll(dec, callers, new File(outDir, prog + ".xlive_callers.c"));
		stringUsers.removeAll(callers);
		decompileAll(dec, stringUsers, new File(outDir, prog + ".net_string_users.c"));
		dec.dispose();
	}

	// Follow references from IAT slots and thunks back to the real call sites.
	private void collect(Address target, Set<Address> out, int depth) {
		if (depth > 4) {
			return;
		}
		for (Reference r : rm.getReferencesTo(target)) {
			Address from = r.getFromAddress();
			Function f = fm.getFunctionContaining(from);
			if (f == null) {
				if (from.isMemoryAddress()) {
					collect(from, out, depth + 1);
				}
			}
			else if (f.isThunk()) {
				collect(f.getEntryPoint(), out, depth + 1);
			}
			else {
				out.add(from);
			}
		}
	}

	private void decompileAll(DecompInterface dec, Set<Function> fns, File out) throws Exception {
		try (PrintWriter pw = new PrintWriter(out, "UTF-8")) {
			for (Function f : fns) {
				if (monitor.isCancelled()) {
					break;
				}
				DecompileResults r = dec.decompileFunction(f, 90, monitor);
				pw.println("// ==== " + f.getEntryPoint() + " " + f.getName());
				pw.println(r.decompileCompleted() ? r.getDecompiledFunction().getC()
						: "// decompile failed: " + r.getErrorMessage());
			}
		}
	}
}
