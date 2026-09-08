"""Locate native Replay asset names by string references, independently of types."""
import json, os, struct, traceback
import ida_bytes, ida_funcs, ida_hexrays, ida_nalt, ida_name, idautils, idc

out = os.environ['IDA_OUT']
r = {'image_base': ida_nalt.get_imagebase(), 'candidates': [], 'symbols': [], 'functions': {}, 'errors': []}
def string(ea):
    if not ida_bytes.is_mapped(ea): return None
    s = ida_bytes.get_strlit_contents(ea, -1, ida_nalt.STRTYPE_C)
    return s.decode('utf-8', 'replace') if s else None
def decompile(ea):
    f = ida_funcs.get_func(ea)
    if not f or str(f.start_ea) in r['functions']: return
    item = {'name': ida_name.get_name(f.start_ea), 'rva': f.start_ea-r['image_base']}
    r['functions'][str(f.start_ea)] = item
    try: item['code'] = str(ida_hexrays.decompile(f.start_ea))
    except Exception as e: item['error'] = str(e)
try:
    ida_hexrays.init_hexrays_plugin()
    for ea,n in idautils.Names():
        if ('asset' in n.lower() and any(x in n.lower() for x in ('pool','size','names','entry'))) or 'DB_Init' in n:
            r['symbols'].append({'name':n,'rva':ea-r['image_base']})
    for s in idautils.Strings():
        if str(s) != 'physicslibrary': continue
        for ref in idautils.DataRefsTo(s.ea):
            if ida_bytes.get_qword(ref) != s.ea: continue
            names=[]
            for i in range(160):
                n=string(ida_bytes.get_qword(ref+i*8))
                if not n or len(n)>64: break
                names.append(n)
                if n=='assetlist': break
            if len(names)<50: continue
            c={'va':ref,'rva':ref-r['image_base'],'names':names,
               'nearby_start':ref,'nearby_bytes':ida_bytes.get_bytes(ref,0x2000).hex(),'xrefs':[]}
            r['candidates'].append(c)
            for x in idautils.XrefsTo(ref):
                c['xrefs'].append(x.frm)
                decompile(x.frm)
    r['complete']=True
except Exception:
    r['errors'].append(traceback.format_exc()); r['complete']=False
with open(out,'w',encoding='utf-8') as f: json.dump(r,f,indent=2)
idc.qexit(0)
