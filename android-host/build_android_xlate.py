"""Build an isolated Android translator from pinned upstream source.

No edits to the upstream checkout. Output under ignored android-work/tools.
Float64/interlock defaults in upstream target Metal conversion, not this GPU.
"""
from pathlib import Path
import argparse
import os
import json
import hashlib
import subprocess
import shutil
from xml.sax.saxutils import escape
from patch_guarded_uniforms import patch_guarded_uniforms

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--dotnet', default=os.environ.get('DOTNET', 'dotnet'))
args = parser.parse_args()
up = root / 'd2r-mobile'
source = up / 'dm/gfx/tools/dmsh_xlate'
work = root / 'android-work/tools/dmsh_xlate_android_src'
out = root / 'android-work/tools/dmsh_xlate_android'
work.mkdir(parents=True, exist_ok=True)
# Keep Android generator fixes isolated from the upstream reference checkout.
ryu = root / 'android-work/tools/ryujinx_android'
shutil.copytree(up / 'dm/third_party/ryujinx', ryu, dirs_exist_ok=True,
                ignore=shutil.ignore_patterns('bin', 'obj', '.git'))
instructions = ryu / 'src/Ryujinx.Graphics.Shader/CodeGen/Spirv/Instructions.cs'
generator = instructions.read_text(encoding='utf-8')
store = '            context.ImageWrite(image, pCoords, texel, ImageOperandsMask.MaskNone);'
assert generator.count(store) == 1, 'Upstream image-store generator changed'
generator = generator.replace(store, '''            if (texOp.Format == TextureFormat.Unknown)
            {
                context.AddCapability(Capability.StorageImageWriteWithoutFormat);
            }
''' + store)
instructions.write_text(generator, encoding='utf-8')
patch_guarded_uniforms(ryu)
code = (source / 'Program.cs').read_text(encoding='utf-8')
abi_anchor = '                    ["cbufs"] = new JsonArray(),'
assert code.count(abi_anchor) == 1, 'Upstream reflection changed; review guarded UBO ABI'
code = code.replace(abi_anchor, '                    ["guarded_uniform_abi"] = (string)job["stage"] is "vs" or "ps" ? 1 : 0,\n' + abi_anchor)
# The translator's --source-sha hashes embedded Program.cs + project, not its
# referenced Ryujinx sources. Include the patch fingerprint in that identity.
guard_patch_sha = hashlib.sha256((Path(__file__).parent / 'patch_guarded_uniforms.py').read_bytes()).hexdigest()
code = '// Android guarded UBO patch sha256: ' + guard_patch_sha + '\n' + code
anchor = '        public bool QueryHostSupportsTextureShadowLod() => true;'
assert code.count(anchor) == 1, 'Upstream accessor changed; review Android profile'
code = code.replace(anchor, anchor + '''
        // Android/Adreno baseline. Never declare unenabled desktop capabilities.
        public bool QueryHostSupportsShaderFloat64() => false;
        public bool QueryHostSupportsFragmentShaderInterlock() => false;
''')
(work / 'Program.cs').write_text(code, encoding='utf-8')
project = (source / 'dmsh_xlate.csproj').read_text(encoding='utf-8')
project = project.replace('$(MSBuildThisFileDirectory)../../../third_party/ryujinx',
                          escape(str(ryu)))
(work / 'dmsh_xlate.csproj').write_text(project, encoding='utf-8')
subprocess.run([args.dotnet, 'publish', str(work / 'dmsh_xlate.csproj'), '-c', 'Release',
                '-o', str(out), '--nologo'], check=True)
revision = subprocess.check_output(['git', '-C', str(up), 'rev-parse', 'HEAD'], text=True).strip()
(out / 'android-profile.json').write_text(json.dumps({
    'upstream': revision, 'shaderFloat64': False, 'fragmentShaderInterlock': False,
    'storageImageWriteWithoutFormatDeclarationFix': True,
    'guarded_uniform_abi': 1,
    'guarded_uniform_stages': ['vs', 'ps'],
    'guarded_uniform_patch_sha256': guard_patch_sha,
    'status': 'translation profile; Vulkan validation and rendering still required'
}, indent=2) + '\n', encoding='utf-8')
print(out)
