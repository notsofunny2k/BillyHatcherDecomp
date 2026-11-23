#!/usr/bin/env python3
"""Static recompiler: Gekko (PowerPC 750CL) code from a GameCube DOL -> C.

Function boundaries and names come from decomp-toolkit's symbols.txt, jump
tables from its `jumptable_*` objects. Each guest function becomes one C
function `void ppc_XXXXXXXX(PPCContext* ctx)`; branches inside a function become
gotos, `bl` becomes a direct C call, `blr` a return, and `bctr` either a switch
over the function's jump-table targets or an indirect tail call.

The output is derived from the game binary: it is written to the build folder
and must not be committed or distributed.

Usage:
  python recomp/ppc2c.py <main.dol> <symbols.txt> <out_dir> [--only name1,name2,...] [--chunk N]
"""
import argparse
import re
import struct
from collections import defaultdict
from pathlib import Path

SYM_RE = re.compile(r"^(\S+) = (\.?\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")


class Dol:
    def __init__(self, path):
        d = Path(path).read_bytes()
        self.d = d
        self.off = struct.unpack(">18I", d[0:72])
        self.addr = struct.unpack(">18I", d[72:144])
        self.size = struct.unpack(">18I", d[144:216])

    def u32(self, a):
        for k in range(18):
            if self.size[k] and self.addr[k] <= a < self.addr[k] + self.size[k]:
                o = self.off[k] + a - self.addr[k]
                return struct.unpack(">I", self.d[o:o + 4])[0]
        return None


def sext(v, bits):
    m = 1 << (bits - 1)
    return (v & (m - 1)) - (v & m)


def rot_mask(mb, me):
    if mb <= me:
        m = (0xFFFFFFFF >> mb) & (0xFFFFFFFF << (31 - me))
    else:
        m = (0xFFFFFFFF >> mb) | (0xFFFFFFFF << (31 - me))
    return m & 0xFFFFFFFF


def R(n):
    return f"ctx->r[{n}]"


def F(n):
    return f"ctx->f[{n}]"


def P1(n):
    return f"ctx->ps1[{n}]"


def ra0(ra):
    """(rA|0)"""
    return "0u" if ra == 0 else R(ra)


def h(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


class Func:
    def __init__(self, name, addr, size):
        self.name, self.addr, self.size = name, addr, size

    @property
    def cname(self):
        return f"ppc_{self.addr:08X}"

    def contains(self, a):
        return self.addr <= a < self.addr + self.size


class Recompiler:
    def __init__(self, dol, funcs, jumptables):
        self.dol = dol
        self.funcs = funcs                      # addr -> Func
        self.sorted_addrs = sorted(funcs)
        self.jumptables = jumptables            # list of (addr, [targets])
        self.referenced = set()                 # function addrs called/branched to
        self.unimpl = defaultdict(int)

    # ---- helpers -------------------------------------------------------
    def func_at(self, a):
        return self.funcs.get(a)

    def call_stmt(self, target, ret_addr):
        f = self.func_at(target)
        s = f"ctx->lr = {h(ret_addr)}; "
        if f:
            self.referenced.add(target)
            return s + f"{f.cname}(ctx);"
        return s + f"ppc_call_indirect(ctx, {h(target)});"

    def jump_stmt(self, fn, target, ret_addr):
        """Unconditional transfer to target from inside fn."""
        if fn.contains(target) and target != fn.addr:
            return f"goto L_{target:08X};"
        f = self.func_at(target)
        if f:
            self.referenced.add(target)
            return f"{{ {f.cname}(ctx); return; }}"
        return f"{{ ppc_call_indirect(ctx, {h(target)}); return; }}"

    def bc_cond(self, bo, bi):
        pre, conds = "", []
        if not bo & 4:
            pre = "ctx->ctr--; "
            conds.append("(ctx->ctr == 0)" if bo & 2 else "(ctx->ctr != 0)")
        if not bo & 0x10:
            bit = f"((ctx->cr[{bi >> 2}] >> {3 - (bi & 3)}) & 1)"
            conds.append(bit if bo & 8 else f"!{bit}")
        return pre, " && ".join(conds)

    def rc(self, ins, expr):
        return f" ppc_set_cr0(ctx, (int32_t){expr});" if ins & 1 else ""

    # ---- per-function --------------------------------------------------
    def labels_for(self, fn):
        labels = set()
        for a in range(fn.addr, fn.addr + fn.size, 4):
            ins = self.dol.u32(a)
            if ins is None:
                continue
            op = ins >> 26
            if op == 18 and not ins & 1:
                t = (sext(ins & 0x03FFFFFC, 26) + (0 if ins & 2 else a)) & 0xFFFFFFFF
                if fn.contains(t):
                    labels.add(t)
            elif op == 16:
                t = (sext(ins & 0xFFFC, 16) + (0 if ins & 2 else a)) & 0xFFFFFFFF
                if fn.contains(t):
                    labels.add(t)
        for jt_addr, targets in self.jumptables:
            if targets and all(fn.contains(t) for t in targets):
                labels.update(targets)
        return labels

    def jt_targets(self, fn):
        out = []
        for jt_addr, targets in self.jumptables:
            if targets and all(fn.contains(t) for t in targets):
                out.extend(targets)
        return sorted(set(out))

    def recompile(self, fn):
        labels = self.labels_for(fn)
        jt = self.jt_targets(fn)
        lines = [f"// {fn.name} @ {fn.addr:08X} size 0x{fn.size:X}",
                 f"void {fn.cname}(PPCContext* ctx) {{"]
        last_ends = False
        for a in range(fn.addr, fn.addr + fn.size, 4):
            ins = self.dol.u32(a)
            if a in labels:
                lines.append(f"L_{a:08X}:;")
            if ins is None:
                lines.append(f"  ppc_unimplemented(ctx, {h(a)}, 0);")
                continue
            stmt, ends = self.translate(fn, a, ins, jt)
            lines.append(f"  /* {a:08X} {ins:08X} */ {stmt}")
            last_ends = ends
        if not last_ends:
            nxt = fn.addr + fn.size
            f = self.func_at(nxt)
            if f:
                self.referenced.add(nxt)
                lines.append(f"  {f.cname}(ctx); /* falls through */")
        lines.append("}")
        return "\n".join(lines)

    # ---- instruction translation ----------------------------------------
    def translate(self, fn, a, ins, jt):
        """Return (C statement, ends_control_flow)."""
        op = ins >> 26
        rd = (ins >> 21) & 31
        ra = (ins >> 16) & 31
        rb = (ins >> 11) & 31
        simm = sext(ins & 0xFFFF, 16)
        uimm = ins & 0xFFFF
        nxt = a + 4

        if op == 18:  # b / bl / ba
            t = (sext(ins & 0x03FFFFFC, 26) + (0 if ins & 2 else a)) & 0xFFFFFFFF
            if ins & 1:
                return self.call_stmt(t, nxt), False
            return self.jump_stmt(fn, t, nxt), True
        if op == 16:  # bc
            bo, bi = rd, ra
            t = (sext(ins & 0xFFFC, 16) + (0 if ins & 2 else a)) & 0xFFFFFFFF
            pre, cond = self.bc_cond(bo, bi)
            act = self.call_stmt(t, nxt) if ins & 1 else self.jump_stmt(fn, t, nxt)
            if not cond:
                return pre + act, not (ins & 1)
            return f"{pre}if ({cond}) {{ {act} }}", False
        if op == 19:
            return self.op19(fn, a, ins, jt)
        if op == 17:
            return "ppc_syscall(ctx);", False

        # integer immediates
        if op == 14:
            return f"{R(rd)} = {ra0(ra)} + {h(simm)};", False
        if op == 15:
            return f"{R(rd)} = {ra0(ra)} + {h(simm << 16)};", False
        if op == 7:
            return f"{R(rd)} = (uint32_t)((int32_t){R(ra)} * {simm});", False
        if op == 8:  # subfic
            return (f"{{ uint64_t t = (uint64_t)(uint32_t){h(simm)} + (uint32_t)~{R(ra)} + 1u; "
                    f"{R(rd)} = (uint32_t)t; ctx->xer_ca = (uint32_t)(t >> 32); }}"), False
        if op in (12, 13):  # addic / addic.
            s = (f"{{ uint64_t t = (uint64_t){R(ra)} + (uint32_t){h(simm)}; "
                 f"{R(rd)} = (uint32_t)t; ctx->xer_ca = (uint32_t)(t >> 32); }}")
            if op == 13:
                s += f" ppc_set_cr0(ctx, (int32_t){R(rd)});"
            return s, False
        if op == 10:
            return f"ppc_cmp_u(ctx, {rd >> 2}, {R(ra)}, {h(uimm)});", False
        if op == 11:
            return f"ppc_cmp_s(ctx, {rd >> 2}, (int32_t){R(ra)}, {simm});", False
        if op == 24:
            return f"{R(ra)} = {R(rd)} | {h(uimm)};", False
        if op == 25:
            return f"{R(ra)} = {R(rd)} | {h(uimm << 16)};", False
        if op == 26:
            return f"{R(ra)} = {R(rd)} ^ {h(uimm)};", False
        if op == 27:
            return f"{R(ra)} = {R(rd)} ^ {h(uimm << 16)};", False
        if op == 28:
            return f"{R(ra)} = {R(rd)} & {h(uimm)}; ppc_set_cr0(ctx, (int32_t){R(ra)});", False
        if op == 29:
            return f"{R(ra)} = {R(rd)} & {h(uimm << 16)}; ppc_set_cr0(ctx, (int32_t){R(ra)});", False
        if op in (20, 21, 23):
            sh, mb, me = rb, (ins >> 6) & 31, (ins >> 1) & 31
            m = h(rot_mask(mb, me))
            rot = f"ROTL32({R(rd)}, {R(rb)} & 31)" if op == 23 else f"ROTL32({R(rd)}, {sh})"
            if op == 20:
                s = f"{R(ra)} = ({rot} & {m}) | ({R(ra)} & ~{m});"
            else:
                s = f"{R(ra)} = {rot} & {m};"
            return s + self.rc(ins, R(ra)), False

        # loads / stores (D-form)
        dform = {
            32: ("ld32", 32, False), 33: ("ld32", 32, True),
            34: ("ld8", 8, False), 35: ("ld8", 8, True),
            40: ("ld16", 16, False), 41: ("ld16", 16, True),
            42: ("lha", 16, False), 43: ("lha", 16, True),
        }
        if op in dform:
            fnc, _, upd = dform[op]
            ea = f"({R(ra)} + {h(simm)})" if upd or ra else h(simm)
            load = f"(uint32_t)(int32_t)(int16_t)ld16(ea)" if fnc == "lha" else f"(uint32_t){fnc}(ea)"
            s = f"{{ uint32_t ea = {ea}; {R(rd)} = {load};"
            if upd:
                s += f" {R(ra)} = ea;"
            return s + " }", False
        stores = {36: ("st32", False), 37: ("st32", True), 38: ("st8", True if False else False),
                  39: ("st8", True), 44: ("st16", False), 45: ("st16", True)}
        stores[38] = ("st8", False)
        if op in stores:
            fnc, upd = stores[op]
            cast = {"st32": "", "st16": "(uint16_t)", "st8": "(uint8_t)"}[fnc]
            ea = f"({R(ra)} + {h(simm)})" if upd or ra else h(simm)
            s = f"{{ uint32_t ea = {ea}; {fnc}(ea, {cast}{R(rd)});"
            if upd:
                s += f" {R(ra)} = ea;"
            return s + " }", False
        if op == 46:  # lmw
            ea = f"({ra0(ra)} + {h(simm)})"
            return (f"{{ uint32_t ea = {ea}; for (int i = {rd}; i < 32; i++, ea += 4) "
                    f"ctx->r[i] = ld32(ea); }}"), False
        if op == 47:  # stmw
            ea = f"({ra0(ra)} + {h(simm)})"
            return (f"{{ uint32_t ea = {ea}; for (int i = {rd}; i < 32; i++, ea += 4) "
                    f"st32(ea, ctx->r[i]); }}"), False
        if 48 <= op <= 55:
            upd = op & 1
            ea = f"({R(ra)} + {h(simm)})" if upd or ra else h(simm)
            if op in (48, 49):
                s = f"{{ uint32_t ea = {ea}; double v = (double)ldf32(ea); {F(rd)} = v; {P1(rd)} = v;"
            elif op in (50, 51):
                s = f"{{ uint32_t ea = {ea}; {F(rd)} = ldf64(ea);"
            elif op in (52, 53):
                s = f"{{ uint32_t ea = {ea}; stf32(ea, (float){F(rd)});"
            else:
                s = f"{{ uint32_t ea = {ea}; stf64(ea, {F(rd)});"
            if upd:
                s += f" {R(ra)} = ea;"
            return s + " }", False
        if op in (56, 57, 60, 61):  # psq_l, psq_lu, psq_st, psq_stu
            w, i, d = (ins >> 15) & 1, (ins >> 12) & 7, sext(ins & 0xFFF, 12)
            upd = op in (57, 61)
            ea = f"({R(ra)} + {h(d)})" if upd or ra else h(d)
            f = "ppc_psq_l" if op in (56, 57) else "ppc_psq_st"
            s = f"{{ uint32_t ea = {ea}; {f}(ctx, {rd}, ea, {w}, {i});"
            if upd:
                s += f" {R(ra)} = ea;"
            return s + " }", False

        if op == 31:
            return self.op31(fn, a, ins)
        if op == 59:
            return self.op59(a, ins)
        if op == 63:
            return self.op63(a, ins)
        if op == 4:
            return self.op4(a, ins)
        return self.unimp(a, ins)

    def unimp(self, a, ins):
        self.unimpl[(ins >> 26, (ins >> 1) & 0x3FF)] += 1
        return f"ppc_unimplemented(ctx, {h(a)}, {h(ins)});", False

    def op19(self, fn, a, ins, jt):
        xo = (ins >> 1) & 0x3FF
        bo, bi = (ins >> 21) & 31, (ins >> 16) & 31
        nxt = a + 4
        lk = ins & 1
        if xo == 16:  # bclr
            pre, cond = self.bc_cond(bo, bi)
            if lk:
                act = f"{{ uint32_t t = ctx->lr; ctx->lr = {h(nxt)}; ppc_call_indirect(ctx, t); }}"
            else:
                act = "return;"
            if not cond:
                return pre + act, not lk
            return f"{pre}if ({cond}) {act}", False
        if xo == 528:  # bcctr
            pre, cond = self.bc_cond(bo | 4, bi)  # bcctr never decrements CTR
            if lk:
                act = f"{{ ctx->lr = {h(nxt)}; ppc_call_indirect(ctx, ctx->ctr); }}"
            elif jt:
                cases = " ".join(f"case {h(t)}: goto L_{t:08X};" for t in jt)
                act = (f"{{ switch (ctx->ctr) {{ {cases} default: "
                       f"ppc_call_indirect(ctx, ctx->ctr); return; }} }}")
            else:
                act = "{ ppc_call_indirect(ctx, ctx->ctr); return; }"
            if not cond:
                return pre + act, not lk
            return f"{pre}if ({cond}) {act}", False
        crd, cra, crb = (ins >> 21) & 31, (ins >> 16) & 31, (ins >> 11) & 31
        ga, gb = f"ppc_get_crbit(ctx, {cra})", f"ppc_get_crbit(ctx, {crb})"
        crops = {257: f"{ga} & {gb}", 129: f"{ga} & !{gb}", 289: f"!({ga} ^ {gb})",
                 225: f"!({ga} & {gb})", 33: f"!({ga} | {gb})", 449: f"{ga} | {gb}",
                 417: f"{ga} | !{gb}", 193: f"{ga} ^ {gb}"}
        if xo in crops:
            return f"ppc_set_crbit(ctx, {crd}, {crops[xo]});", False
        if xo == 0:  # mcrf
            return f"ctx->cr[{crd >> 2}] = ctx->cr[{cra >> 2}];", False
        if xo == 150:  # isync
            return "/* isync */", False
        return self.unimp(a, ins)

    def op31(self, fn, a, ins):
        xo = (ins >> 1) & 0x3FF
        rd, ra, rb = (ins >> 21) & 31, (ins >> 16) & 31, (ins >> 11) & 31
        rs = rd
        rc = ins & 1
        x9 = xo & 0x1FF
        # XO-form arithmetic (OE ignored)
        arith = {
            266: f"{R(ra)} + {R(rb)}",
            40: f"{R(rb)} - {R(ra)}",
            104: f"(uint32_t)-(int32_t){R(ra)}",
            235: f"(uint32_t)((int32_t){R(ra)} * (int32_t){R(rb)})",
            75: f"(uint32_t)(((int64_t)(int32_t){R(ra)} * (int32_t){R(rb)}) >> 32)",
            11: f"(uint32_t)(((uint64_t){R(ra)} * {R(rb)}) >> 32)",
            491: (f"({R(rb)} == 0 || ({R(ra)} == 0x80000000u && {R(rb)} == 0xFFFFFFFFu)) ? "
                  f"((int32_t){R(ra)} < 0 ? 0xFFFFFFFFu : 0u) : "
                  f"(uint32_t)((int32_t){R(ra)} / (int32_t){R(rb)})"),
            459: f"{R(rb)} == 0 ? 0u : {R(ra)} / {R(rb)}",
        }
        if x9 in arith and xo in (x9, x9 | 0x200):
            return f"{R(rd)} = {arith[x9]};" + self.rc(ins, R(rd)), False
        carry = {
            10: (f"(uint64_t){R(ra)} + {R(rb)}"),                      # addc
            8: (f"(uint64_t)(uint32_t)~{R(ra)} + {R(rb)} + 1u"),       # subfc
            138: (f"(uint64_t){R(ra)} + {R(rb)} + ctx->xer_ca"),        # adde
            136: (f"(uint64_t)(uint32_t)~{R(ra)} + {R(rb)} + ctx->xer_ca"),  # subfe
            202: (f"(uint64_t){R(ra)} + ctx->xer_ca"),                   # addze
            200: (f"(uint64_t)(uint32_t)~{R(ra)} + ctx->xer_ca"),        # subfze
            234: (f"(uint64_t){R(ra)} + ctx->xer_ca + 0xFFFFFFFFu"),     # addme
            232: (f"(uint64_t)(uint32_t)~{R(ra)} + ctx->xer_ca + 0xFFFFFFFFu"),  # subfme
        }
        if x9 in carry and xo in (x9, x9 | 0x200):
            return (f"{{ uint64_t t = {carry[x9]}; {R(rd)} = (uint32_t)t; "
                    f"ctx->xer_ca = (uint32_t)(t >> 32) & 1; }}" + self.rc(ins, R(rd))), False

        logic = {
            28: f"{R(rs)} & {R(rb)}", 60: f"{R(rs)} & ~{R(rb)}", 444: f"{R(rs)} | {R(rb)}",
            412: f"{R(rs)} | ~{R(rb)}", 316: f"{R(rs)} ^ {R(rb)}", 124: f"~({R(rs)} | {R(rb)})",
            476: f"~({R(rs)} & {R(rb)})", 284: f"~({R(rs)} ^ {R(rb)})",
            922: f"(uint32_t)(int32_t)(int16_t){R(rs)}", 954: f"(uint32_t)(int32_t)(int8_t){R(rs)}",
            26: f"{R(rs)} ? (uint32_t)__builtin_clz({R(rs)}) : 32u",
            24: f"({R(rb)} & 0x20) ? 0u : {R(rs)} << ({R(rb)} & 31)",
            536: f"({R(rb)} & 0x20) ? 0u : {R(rs)} >> ({R(rb)} & 31)",
        }
        if xo in logic:
            return f"{R(ra)} = {logic[xo]};" + self.rc(ins, R(ra)), False
        if xo == 824:  # srawi
            sh = rb
            return (f"{{ int32_t s = (int32_t){R(rs)}; ctx->xer_ca = (s < 0 && (s & {h((1 << sh) - 1)})) ? 1 : 0; "
                    f"{R(ra)} = (uint32_t)(s >> {sh}); }}" + self.rc(ins, R(ra))), False
        if xo == 792:  # sraw
            return (f"{{ int32_t s = (int32_t){R(rs)}; uint32_t n = {R(rb)} & 0x3F; "
                    f"if (n >= 32) {{ {R(ra)} = s < 0 ? 0xFFFFFFFFu : 0u; ctx->xer_ca = s < 0; }} "
                    f"else {{ ctx->xer_ca = (s < 0 && n && (s & ((1u << n) - 1))) ? 1 : 0; "
                    f"{R(ra)} = (uint32_t)(s >> n); }} }}" + self.rc(ins, R(ra))), False
        if xo == 0:
            return f"ppc_cmp_s(ctx, {rd >> 2}, (int32_t){R(ra)}, (int32_t){R(rb)});", False
        if xo == 32:
            return f"ppc_cmp_u(ctx, {rd >> 2}, {R(ra)}, {R(rb)});", False

        ea = f"({ra0(ra)} + {R(rb)})"
        eau = f"({R(ra)} + {R(rb)})"
        loads = {23: ("ld32", 0), 55: ("ld32", 1), 87: ("ld8", 0), 119: ("ld8", 1),
                 279: ("ld16", 0), 311: ("ld16", 1), 343: ("lha", 0), 375: ("lha", 1),
                 20: ("ld32", 0)}  # lwarx
        if xo in loads:
            f, upd = loads[xo]
            val = "(uint32_t)(int32_t)(int16_t)ld16(ea)" if f == "lha" else f"(uint32_t){f}(ea)"
            s = f"{{ uint32_t ea = {eau if upd else ea}; {R(rd)} = {val};"
            if upd:
                s += f" {R(ra)} = ea;"
            return s + " }", False
        stores = {151: ("st32", 0), 183: ("st32", 1), 215: ("st8", 0), 247: ("st8", 1),
                  407: ("st16", 0), 439: ("st16", 1)}
        if xo in stores:
            f, upd = stores[xo]
            cast = {"st32": "", "st16": "(uint16_t)", "st8": "(uint8_t)"}[f]
            s = f"{{ uint32_t ea = {eau if upd else ea}; {f}(ea, {cast}{R(rs)});"
            if upd:
                s += f" {R(ra)} = ea;"
            return s + " }", False
        if xo == 150:  # stwcx.
            return f"st32({ea}, {R(rs)}); ctx->cr[0] = (uint8_t)(2 | (ctx->xer_so & 1));", False
        if xo == 534:
            return f"{R(rd)} = __builtin_bswap32(ld32({ea}));", False
        if xo == 790:
            return f"{R(rd)} = __builtin_bswap16(ld16({ea}));", False
        if xo == 662:
            return f"st32({ea}, __builtin_bswap32({R(rs)}));", False
        if xo == 918:
            return f"st16({ea}, __builtin_bswap16((uint16_t){R(rs)}));", False
        fl = {535: (0, 0), 567: (0, 1), 599: (1, 0), 631: (1, 1)}
        if xo in fl:
            dbl, upd = fl[xo]
            s = f"{{ uint32_t ea = {eau if upd else ea}; "
            s += f"{F(rd)} = ldf64(ea);" if dbl else f"double v = (double)ldf32(ea); {F(rd)} = v; {P1(rd)} = v;"
            if upd:
                s += f" {R(ra)} = ea;"
            return s + " }", False
        fs = {663: (0, 0), 695: (0, 1), 727: (1, 0), 759: (1, 1)}
        if xo in fs:
            dbl, upd = fs[xo]
            s = f"{{ uint32_t ea = {eau if upd else ea}; "
            s += f"stf64(ea, {F(rd)});" if dbl else f"stf32(ea, (float){F(rd)});"
            if upd:
                s += f" {R(ra)} = ea;"
            return s + " }", False
        if xo == 983:  # stfiwx
            return f"st32({ea}, (uint32_t)d2u({F(rd)}));", False
        if xo == 1014:  # dcbz
            return f"memset(ppc_ptr({ea} & ~31u), 0, 32);", False
        if xo in (54, 86, 246, 278, 470, 982, 598, 854):  # cache ops / sync / eieio
            return "/* cache/sync */", False
        if xo == 19:
            return f"{R(rd)} = ppc_get_cr(ctx);", False
        if xo == 144:
            crm = (ins >> 12) & 0xFF
            return f"ppc_set_cr(ctx, {R(rs)}, {h(crm)});", False
        if xo == 512:  # mcrxr
            return (f"ctx->cr[{rd >> 2}] = (uint8_t)((ppc_get_xer(ctx) >> 28) & 0xF); "
                    f"ctx->xer_so = ctx->xer_ov = ctx->xer_ca = 0;"), False
        if xo in (339, 467, 371):
            spr = ((ins >> 16) & 0x1F) | (((ins >> 11) & 0x1F) << 5)
            if xo == 371:  # mftb
                return (f"{R(rd)} = (uint32_t)(ppc_get_timebase() >> "
                        f"{32 if spr == 269 else 0});"), False
            name = {8: "ctx->lr", 9: "ctx->ctr"}.get(spr)
            if xo == 339:
                if spr == 1:
                    return f"{R(rd)} = ppc_get_xer(ctx);", False
                if spr in (268, 269):
                    return (f"{R(rd)} = (uint32_t)(ppc_get_timebase() >> "
                            f"{32 if spr == 269 else 0});"), False
                return f"{R(rd)} = {name or f'ctx->spr[{spr}]'};", False
            if spr == 1:
                return f"ppc_set_xer(ctx, {R(rs)});", False
            return f"{name or f'ctx->spr[{spr}]'} = {R(rs)};", False
        if xo == 83:
            return f"{R(rd)} = ctx->msr;", False
        if xo == 146:
            return f"ctx->msr = {R(rs)};", False
        return self.unimp(a, ins)

    def fp_arith(self, ins, single):
        fd, fa, fb, fc = (ins >> 21) & 31, (ins >> 16) & 31, (ins >> 11) & 31, (ins >> 6) & 31
        x5 = (ins >> 1) & 31
        A, B, C = F(fa), F(fb), F(fc)
        ops = {18: f"{A} / {B}", 20: f"{A} - {B}", 21: f"{A} + {B}", 25: f"{A} * {C}",
               28: f"fma({A}, {C}, -{B})", 29: f"fma({A}, {C}, {B})",
               30: f"-fma({A}, {C}, -{B})", 31: f"-fma({A}, {C}, {B})"}
        if single:
            ops.update({24: f"1.0 / {B}"})
        else:
            ops.update({22: f"sqrt({B})", 26: f"1.0 / sqrt({B})",
                        23: f"({A} >= 0.0) ? {C} : {B}"})
        if x5 not in ops:
            return None
        if single:
            return f"{{ double v = R32({ops[x5]}); {F(fd)} = v; {P1(fd)} = v; }}"
        return f"{F(fd)} = {ops[x5]};"

    def op59(self, a, ins):
        s = self.fp_arith(ins, True)
        return (s, False) if s else self.unimp(a, ins)

    def op63(self, a, ins):
        xo = (ins >> 1) & 0x3FF
        fd, fa, fb = (ins >> 21) & 31, (ins >> 16) & 31, (ins >> 11) & 31
        crf = (ins >> 23) & 7
        if xo in (0, 32):
            return f"ppc_fcmp(ctx, {crf}, {F(fa)}, {F(fb)});", False
        simple = {12: f"R32({F(fb)})", 40: f"-{F(fb)}", 72: F(fb), 136: f"-fabs({F(fb)})",
                  264: f"fabs({F(fb)})"}
        if xo in simple:
            s = f"{F(fd)} = {simple[xo]};"
            if xo == 12:
                s = f"{{ double v = R32({F(fb)}); {F(fd)} = v; }}"
            return s, False
        if xo in (14, 15):
            return f"ppc_fctiw(ctx, {fd}, {F(fb)}, {1 if xo == 15 else 0});", False
        if xo == 583:  # mffs
            return f"{F(fd)} = u2d((uint64_t)ctx->fpscr);", False
        if xo == 711:  # mtfsf
            return f"ctx->fpscr = (uint32_t)d2u({F(fb)});", False
        if xo == 38:
            return f"ctx->fpscr |= {h(0x80000000 >> fd)};", False
        if xo == 70:
            return f"ctx->fpscr &= ~{h(0x80000000 >> fd)};", False
        if xo == 134:  # mtfsfi
            imm = (ins >> 12) & 0xF
            sh = 28 - 4 * crf
            return f"ctx->fpscr = (ctx->fpscr & ~{h(0xF << sh)}) | {h(imm << sh)};", False
        if xo == 64:  # mcrfs
            src = (ins >> 18) & 7
            return f"ctx->cr[{crf}] = (uint8_t)((ctx->fpscr >> {28 - 4 * src}) & 0xF);", False
        s = self.fp_arith(ins, False)
        return (s, False) if s else self.unimp(a, ins)

    def op4(self, a, ins):
        fd, fa, fb, fc = (ins >> 21) & 31, (ins >> 16) & 31, (ins >> 11) & 31, (ins >> 6) & 31
        x5 = (ins >> 1) & 31
        x6 = (ins >> 1) & 63
        xo = (ins >> 1) & 0x3FF
        a0, a1, b0, b1, c0, c1 = F(fa), P1(fa), F(fb), P1(fb), F(fc), P1(fc)
        if x6 in (6, 7, 38, 39):  # psq_lx, psq_stx, psq_lux, psq_stux
            w, i = (ins >> 10) & 1, (ins >> 7) & 7
            upd = x6 >= 38
            ea = f"({R(fa)} + {R(fb)})" if upd else f"({ra0(fa)} + {R(fb)})"
            f = "ppc_psq_l" if x6 in (6, 38) else "ppc_psq_st"
            s = f"{{ uint32_t ea = {ea}; {f}(ctx, {fd}, ea, {w}, {i});"
            if upd:
                s += f" {R(fa)} = ea;"
            return s + " }", False
        pair = {
            21: (f"{a0} + {b0}", f"{a1} + {b1}"),
            20: (f"{a0} - {b0}", f"{a1} - {b1}"),
            25: (f"{a0} * {c0}", f"{a1} * {c1}"),
            18: (f"{a0} / {b0}", f"{a1} / {b1}"),
            29: (f"fma({a0}, {c0}, {b0})", f"fma({a1}, {c1}, {b1})"),
            28: (f"fma({a0}, {c0}, -{b0})", f"fma({a1}, {c1}, -{b1})"),
            31: (f"-fma({a0}, {c0}, {b0})", f"-fma({a1}, {c1}, {b1})"),
            30: (f"-fma({a0}, {c0}, -{b0})", f"-fma({a1}, {c1}, -{b1})"),
            12: (f"{a0} * {c0}", f"{a1} * {c0}"),
            13: (f"{a0} * {c1}", f"{a1} * {c1}"),
            14: (f"fma({a0}, {c0}, {b0})", f"fma({a1}, {c0}, {b1})"),
            15: (f"fma({a0}, {c1}, {b0})", f"fma({a1}, {c1}, {b1})"),
            10: (f"{a0} + {b1}", c1),
            11: (c0, f"{a0} + {b1}"),
            23: (f"({a0} >= 0.0) ? {c0} : {b0}", f"({a1} >= 0.0) ? {c1} : {b1}"),
            24: (f"1.0 / {b0}", f"1.0 / {b1}"),
            26: (f"1.0 / sqrt({b0})", f"1.0 / sqrt({b1})"),
        }
        if x5 in pair:
            e0, e1 = pair[x5]
            return (f"{{ double v0 = R32({e0}), v1 = R32({e1}); "
                    f"{F(fd)} = v0; {P1(fd)} = v1; }}"), False
        mv = {
            72: (b0, b1), 40: (f"-{b0}", f"-{b1}"), 264: (f"fabs({b0})", f"fabs({b1})"),
            136: (f"-fabs({b0})", f"-fabs({b1})"),
            528: (a0, b0), 560: (a0, b1), 592: (a1, b0), 624: (a1, b1),
        }
        if xo in mv:
            e0, e1 = mv[xo]
            return f"{{ double v0 = {e0}, v1 = {e1}; {F(fd)} = v0; {P1(fd)} = v1; }}", False
        if xo in (0, 32, 64, 96):
            crf = (ins >> 23) & 7
            x, y = (a0, b0) if xo in (0, 32) else (a1, b1)
            return f"ppc_fcmp(ctx, {crf}, {x}, {y});", False
        if xo == 1014:  # dcbz_l
            return f"memset(ppc_ptr(({ra0(fa)} + {R(fb)}) & ~31u), 0, 32);", False
        return self.unimp(a, ins)


def load_symbols(path):
    funcs, jts = {}, []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        m = SYM_RE.match(line)
        if not m:
            continue
        name, sec, addr, typ, size = m.group(1), m.group(2), int(m.group(3), 16), m.group(4), m.group(5)
        size = int(size, 16) if size else 0
        if typ == "function" and sec in (".init", ".text") and size:
            funcs[addr] = Func(name, addr, size)
        elif typ == "object" and name.startswith("jumptable_"):
            jts.append((addr, size))
    return funcs, jts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dol")
    ap.add_argument("symbols")
    ap.add_argument("out", type=Path)
    ap.add_argument("--only", default="", help="comma-separated function names to recompile")
    ap.add_argument("--chunk", type=int, default=400)
    ap.add_argument("--closure", action="store_true",
                    help="with --only, also recompile every function they (transitively) call")
    args = ap.parse_args()

    dol = Dol(args.dol)
    funcs, jt_syms = load_symbols(args.symbols)
    jumptables = []
    for addr, size in jt_syms:
        targets = [dol.u32(addr + 4 * i) for i in range(size // 4)]
        jumptables.append((addr, [t for t in targets if t is not None]))

    rc = Recompiler(dol, funcs, jumptables)
    if args.only:
        wanted = set(args.only.split(","))
        selected = [f for f in funcs.values() if f.name in wanted]
        missing = wanted - {f.name for f in selected}
        if missing:
            raise SystemExit(f"unknown functions: {sorted(missing)}")
        if args.closure:
            todo = list(selected)
            seen = {f.addr for f in selected}
            while todo:
                f = todo.pop()
                probe = Recompiler(dol, funcs, jumptables)
                probe.recompile(f)
                for t in probe.referenced:
                    if t not in seen:
                        seen.add(t)
                        todo.append(funcs[t])
            selected = [funcs[a] for a in seen]
    else:
        selected = list(funcs.values())
    selected.sort(key=lambda f: f.addr)

    args.out.mkdir(parents=True, exist_ok=True)
    bodies = [(f, rc.recompile(f)) for f in selected]
    generated = {f.addr for f in selected}

    hdr = ["// Generated by ppc2c.py from the game binary. Do not commit or distribute.",
           "#pragma once", '#include "ppc_context.h"', ""]
    all_decl = sorted(generated | rc.referenced)
    for a in all_decl:
        hdr.append(f"void ppc_{a:08X}(PPCContext* ctx); // {funcs[a].name}")
    hdr.append("")
    for f in selected:
        if re.fullmatch(r"[A-Za-z_]\w*", f.name):
            hdr.append(f"#define PPC_ADDR_{f.name} 0x{f.addr:08X}u")
    (args.out / "ppc_funcs.h").write_text("\n".join(hdr) + "\n", encoding="utf-8")

    for i in range(0, len(bodies), args.chunk):
        part = ['// Generated by ppc2c.py from the game binary. Do not commit or distribute.',
                '#include "ppc_funcs.h"', ""]
        part += [b for _, b in bodies[i:i + args.chunk]]
        (args.out / f"ppc_recomp_{i // args.chunk:03d}.c").write_text("\n\n".join(part) + "\n", encoding="utf-8")

    tbl = ['// Generated by ppc2c.py. Do not commit or distribute.', '#include "ppc_funcs.h"',
           '#include "ppc_runtime.h"', ""]
    stubs = sorted(rc.referenced - generated)
    for a in stubs:
        tbl.append(f"void ppc_{a:08X}(PPCContext* ctx) {{ ppc_missing(ctx, 0x{a:08X}u); }}")
    tbl.append("const PPCFuncEntry ppc_func_table[] = {")
    for a in sorted(generated | set(stubs)):
        tbl.append(f"  {{ 0x{a:08X}u, ppc_{a:08X} }},")
    tbl.append("};")
    tbl.append(f"const size_t ppc_func_table_size = {len(generated | set(stubs))};")
    (args.out / "ppc_table.c").write_text("\n".join(tbl) + "\n", encoding="utf-8")

    total_unimpl = sum(rc.unimpl.values())
    print(f"recompiled {len(selected)} functions, {len(stubs)} stubs, "
          f"{total_unimpl} unimplemented instructions")
    for (op, xo), n in sorted(rc.unimpl.items(), key=lambda kv: -kv[1])[:20]:
        print(f"  op {op} xo {xo}: {n}")


if __name__ == "__main__":
    main()
