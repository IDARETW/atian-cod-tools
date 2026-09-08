"""Read-only IDA export of MW2019 asset tables and reachable ABI types.

Run on a disposable database copy through idat_run.py. IDA_REQ_OUT is supplied
by that wrapper. This script never renames or changes database objects.
"""
import json
import os
import traceback
import idaapi
import ida_bytes
import ida_name
import ida_nalt
import ida_typeinf
import idautils
import idc

out = os.environ.get('IDA_OUT', os.environ.get('IDA_REQ_OUT', 'mw19_pools.json'))
report = {'schema': 1, 'input': ida_nalt.get_input_file_path(),
          'image_base': ida_nalt.get_imagebase(), 'symbols': {}, 'types': {}, 'errors': []}

def save():
    with open(out + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=2)
    os.replace(out + '.tmp', out)

def export_type(t):
    name = t.get_type_name() or str(t)
    if name in report['types']:
        return name
    item = {'name': name, 'declaration': str(t), 'size': t.get_size()}
    report['types'][name] = item
    if t.is_ptr():
        item['kind'] = 'pointer'
        item['target'] = export_type(t.get_pointed_object())
    elif t.is_array():
        a = ida_typeinf.array_type_data_t()
        t.get_array_details(a)
        item.update(kind='array', count=a.nelems, element=export_type(a.elem_type))
    elif t.is_udt():
        u = ida_typeinf.udt_type_data_t()
        t.get_udt_details(u)
        item.update(kind='union' if t.is_union() else 'struct', members=[])
        for m in u:
            item['members'].append({'name': m.name, 'offset_bits': m.offset,
                                    'size_bits': m.size, 'type': export_type(m.type)})
    elif t.is_enum():
        item['kind'] = 'enum'
        item['declaration'] = ida_typeinf.print_tinfo('', 0, 0, ida_typeinf.PRTYPE_MULTI | ida_typeinf.PRTYPE_TYPE | ida_typeinf.PRTYPE_DEF, t, name, '')
    else:
        item['kind'] = 'scalar' if not t.is_func() else 'function'
    return name

try:
    names = list(idautils.Names())
    needles = ('g_asset', 's_assetPool', 's_poolSize', 'g_poolSize', 'DB_GetXAsset', 'DB_AssetPool', 'DB_InitPool', 's_assetEntry', 's_assetManager')
    for ea, name in names:
        if not any(x in name for x in needles):
            continue
        t = ida_typeinf.tinfo_t()
        item = {'va': ea, 'rva': ea - report['image_base'], 'type': idc.get_type(ea)}
        report['symbols'][name] = item
        if ida_nalt.get_tinfo(t, ea):
            item['type_name'] = export_type(t)
        if 'g_asset' in name or 'poolSize' in name or 'PoolSize' in name:
            size = min(ida_bytes.get_item_size(ea), 65536)
            data = ida_bytes.get_bytes(ea, size)
            item['bytes'] = data.hex() if data else None
            item['strings'] = []
            for i in range(min(size // 8, 256)):
                ptr = ida_bytes.get_qword(ea + i * 8)
                s = ida_bytes.get_strlit_contents(ptr, -1, ida_nalt.STRTYPE_C) if ida_bytes.is_mapped(ptr) else None
                item['strings'].append(s.decode('utf-8', 'replace') if s else None)
    for name in ('XAssetType', 'XAssetHeader', 'XAsset', 'DB_AssetPool', 'DB_AssetEntry', 'DB_AssetEntryPool', 'DB_AssetEntryTable'):
        t = ida_typeinf.tinfo_t()
        if t.get_named_type(None, name):
            export_type(t)
        else:
            report['errors'].append('Type not found: ' + name)
    report['complete'] = True
except Exception:
    report['errors'].append(traceback.format_exc())
    report['complete'] = False
finally:
    save()
    idc.qexit(0)
