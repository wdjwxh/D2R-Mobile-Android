"""Package existing Vulkan graphics shaders for lazy loading, no retranslating."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib
from build_compute import ROOT, spv_build, reflect_bindings, binding_sample_types
from spirv_fuse_facts import analyze as analyze_fuse, input_attachment_variant


def fuse_extension(code, reflection, bindings, facts):
    """SH4 little-endian VFS1 extension, aligned by its enclosing record.

    Header: magic/schema/bytes/guardABI, 37 signed raster words,
    ninputs/nvariants plus four raster flag words. Then 16-byte input records,
    12-byte variant records (mask/offset/bytes), and aligned SPIR-V modules.
    """
    raster=facts['raster'];position=raster['position']
    pos_attr=position['attribute'] if position and 0<=position['attribute']<16 else -1
    w_one=int(bool(position and position['w']=='one'))
    signs=position['xyz_sign'] if position else [1,1,1]
    uv=[raster['uv_from_attr'].get(str(i),-1) for i in range(32)]
    uv=[a if 0<=a<16 else -1 for a in uv]
    groups={}
    unsupported={s.get('binding') for s in facts['samples'] if s['eligible'] and
                 (s['descriptor_set']!=0 or not 0<=s['varying_location']<32)}
    for sample in facts['samples']:
        if sample['eligible'] and sample['descriptor_set']==0 and sample['binding'] not in unsupported:
            groups.setdefault(sample['binding'],[]).append(sample)
    inputs=[]
    if reflection['stage']=='ps':
        for binding_index,b in enumerate(bindings):
            if b[0] not in groups or b[1]!=6:continue
            varying_mask=0
            for sample in groups[b[0]]:varying_mask|=1<<sample['varying_location']
            inputs.append((binding_index,len(inputs),varying_mask,0))
    if len(inputs)>8:inputs=[]
    variants=[]
    try:
        if len({bindings[i[0]][0] for i in inputs})!=len(inputs):
            raise ValueError('Duplicate reflected descriptor binding')
        for mask in range(1,1<<len(inputs)):
            mapping={(0,bindings[binding_index][0]):index for binding_index,index,_,_ in inputs if mask&(1<<index)}
            variant,_=input_attachment_variant(code,facts,mapping)
            variants.append((mask,variant))
            if sum(len(v) for _,v in variants)>16*1024*1024:
                raise ValueError('Input variant module budget exceeded')
    except ValueError as error:
        # Unsupported resource aliases are a normal fail-closed fallback.
        print(f'FUSE disabled for {reflection["key"]}: {error}')
        inputs=[];variants=[]
    header_bytes=47*4
    offset=header_bytes+16*len(inputs)+12*len(variants)
    records=[];payload=[]
    for mask,variant in variants:
        records.append(struct.pack('<3I',mask,offset,len(variant)))
        payload.append(variant);offset+=len(variant)
    color_mask=sum(1<<i for i in raster['color_locations'] if 0<=i<8)
    header=struct.pack('<4I37i6I',0x31534656,1,offset,reflection.get('guarded_uniform_abi',0),
                       pos_attr,w_one,*signs,*uv,len(inputs),len(variants),
                       int(raster['may_discard']),int(raster['writes_sample_mask']),
                       int(raster['writes_frag_depth']),color_mask)
    return header+b''.join(struct.pack('<4I',*i) for i in inputs)+b''.join(records)+b''.join(payload),len(variants)

def main():
    destination=Path(sys.argv[1]);destination.parent.mkdir(parents=True,exist_ok=True)
    shader_dir=ROOT/'android-work/shaders/spv-adreno'
    shaders=[]
    for path in sorted(shader_dir.glob('*.res.json')):
        metadata=path.read_bytes();j=json.loads(metadata)
        if j['stage'] in ('vs','ps'): shaders.append((j,metadata))
    locations,_,_=spv_build.load_map(str(ROOT/'d2r-mobile/dm/gfx/nvn'))
    guard_abis={j.get('guarded_uniform_abi',0) for j,_ in shaders}
    if not shaders or len(guard_abis)!=1 or not guard_abis<={0,1}:
        raise ValueError('Mixed/unknown graphics guarded-uniform ABI; rebuild the complete shader catalog')
    guard_abi=next(iter(guard_abis))
    packs={};records=[];main_image=None;fuse_variants=0;fuse_shaders=0
    with destination.open('wb') as f:
        f.write(b'D2RVKSH4'+struct.pack('<I',len(shaders)))
        f.write(bytes(48*len(shaders)))
        for j,metadata in shaders:
            key=j['key'];name,index=locations[key]
            if name.startswith('main.rodata@'):
                if main_image is None:main_image=(ROOT/'android-work/modules/main.android.bin').read_bytes()
                start=main_image.rfind(spv_build.CODE_MARK,index-0x1000,index)
                if start<0:raise ValueError('Missing embedded code '+key)
                record=bytes(0x38)+main_image[start:index+0x880]
            else:
                if name not in packs:packs[name]=spv_build.records((ROOT/'android-work/shaders/packs'/Path(name).name).read_bytes())
                record=packs[name][index]
            ctrl=record.index(spv_build.CTRL)
            if hashlib.sha256(record[ctrl:ctrl+0x880]).hexdigest()[:16]!=key:raise ValueError('Control mismatch '+key)
            start,end=struct.unpack_from('<II',record,ctrl+0x700)
            constants=record[0x38+start:0x38+end]
            code=(shader_dir/(key+'.spv')).read_bytes()
            # Analyze exactly the final guarded-UBO SPIR-V bytes placed below.
            # Never trust pre-patch IDs or facts derived from upstream MSL.
            j['vulkan_fuse']=analyze_fuse(code,j)
            metadata=json.dumps(j,separators=(',',':')).encode('utf-8')
            bindings=binding_sample_types(code,reflect_bindings(j))
            binding_data=struct.pack('<I',len(bindings))+b''.join(struct.pack('<6i',*b) for b in bindings)
            extension,nvariants=fuse_extension(code,j,bindings,j['vulkan_fuse'])
            fuse_variants+=nvariants;fuse_shaders+=bool(nvariants)
            raw=code+metadata+b'\0'+constants+binding_data
            raw+=bytes((-len(raw))&3)+extension
            compressed=zlib.compress(raw,1);offset=f.tell()
            f.write(compressed)
            records.append(struct.pack('<16s8I',key.encode(),0 if j['stage']=='vs' else 1,len(code),len(metadata)+1,len(constants),j.get('frag_out_map',0),offset,len(compressed),len(raw)))
        f.seek(12)
        for record in records:f.write(record)
    print(f'Graphics archive: {len(shaders)} shaders, {destination.stat().st_size/1048576:.1f} MiB')
    print(f'FUSE: {fuse_shaders} fragment shaders, {fuse_variants} input-attachment variants, guarded ABI {guard_abi}')

if __name__=='__main__':main()
