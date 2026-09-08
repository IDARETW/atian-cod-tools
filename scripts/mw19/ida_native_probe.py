"""Bounded, read-only decompilation and byte-table evidence by explicit address."""
import json, os, traceback
import ida_bytes, ida_funcs, ida_hexrays, ida_nalt, ida_name, idautils, idc
cfg=json.load(open(os.environ['IDA_REQUEST'],encoding='utf-8'))
r={'image_base':ida_nalt.get_imagebase(),'functions':{},'tables':{},'errors':[]}
ida_hexrays.init_hexrays_plugin()
def save():
    with open(os.environ['IDA_OUT']+'.tmp','w',encoding='utf-8') as f: json.dump(r,f,indent=2)
    os.replace(os.environ['IDA_OUT']+'.tmp',os.environ['IDA_OUT'])
for ea in cfg.get('functions',[]):
    ea=int(ea,0) if isinstance(ea,str) else ea
    item={'name':ida_name.get_name(ea),'rva':ea-r['image_base']}
    r['functions'][hex(ea)]=item
    try:
        item['code']=str(ida_hexrays.decompile(ea))
        f=ida_funcs.get_func(ea)
        if f:
            item['bytes']=ida_bytes.get_bytes(f.start_ea,min(f.end_ea-f.start_ea,32768)).hex()
    except Exception: item['error']=traceback.format_exc()
    save()
for name,entry in cfg.get('tables',{}).items():
    ea=int(entry['va'],0); n=entry['size']; b=ida_bytes.get_bytes(ea,n)
    item={'va':ea,'rva':ea-r['image_base'],'bytes':b.hex() if b else None,'xrefs':[]}
    r['tables'][name]=item
    for x in idautils.XrefsTo(ea):
        f=ida_funcs.get_func(x.frm)
        item['xrefs'].append({'from':x.frm,'function':f.start_ea if f else None,'name':ida_name.get_name(f.start_ea) if f else None})
    if entry.get('strings'):
        item['strings']=[]
        for i in range(n//8):
            ptr=ida_bytes.get_qword(ea+i*8)
            s=ida_bytes.get_strlit_contents(ptr,-1,ida_nalt.STRTYPE_C) if ida_bytes.is_mapped(ptr) else None
            item['strings'].append(s.decode('utf-8','replace') if s else None)
    save()
r['complete']=True
save()
idc.qexit(0)
