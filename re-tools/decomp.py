import sys, logging, time
import angr
for n in ("angr","cle","pyvex","claripy"):
    logging.getLogger(n).setLevel(logging.ERROR)

BIN   = sys.argv[1]
TARGET= int(sys.argv[2], 16)
RSTART= int(sys.argv[3], 16) if len(sys.argv)>3 else TARGET-0x800
REND  = int(sys.argv[4], 16) if len(sys.argv)>4 else TARGET+0x9000

t0=time.time()
proj = angr.Project(BIN, auto_load_libs=False)
print(f"[*] loaded; base=0x{proj.loader.main_object.min_addr:x}  ({time.time()-t0:.1f}s)")
print(f"[*] CFGFast region 0x{RSTART:x}-0x{REND:x}")
cfg = proj.analyses.CFGFast(normalize=True, regions=[(RSTART,REND)], force_complete_scan=False)
print(f"[*] CFG: {len(proj.kb.functions)} funcs  ({time.time()-t0:.1f}s)")
proj.analyses.CompleteCallingConventions(recover_variables=True)
print(f"[*] CCC done ({time.time()-t0:.1f}s)")
try:
    func = proj.kb.functions.get_by_addr(TARGET)
except Exception:
    addrs=sorted(proj.kb.functions); import bisect
    func=proj.kb.functions[addrs[bisect.bisect_right(addrs,TARGET)-1]]
    print(f"[*] nearest func 0x{func.addr:x}")
dec = proj.analyses.Decompiler(func, cfg=cfg.model)
print("="*72)
print(dec.codegen.text if dec.codegen else "(no codegen)")
