"""Local tool discovery. No SDK, executable or credentials are bundled."""
import os
from pathlib import Path
import shutil


def shader_tools():
    if os.environ.get('D2R_SHADER_TOOLS'):
        return Path(os.environ['D2R_SHADER_TOOLS'])
    if os.environ.get('ANDROID_NDK_HOME'):
        return Path(os.environ['ANDROID_NDK_HOME']) / 'shader-tools/windows-x86_64'
    compiler = shutil.which('glslc')
    if compiler:
        return Path(compiler).parent
    raise RuntimeError('Set D2R_SHADER_TOOLS to the directory containing glslc and spirv-val')
