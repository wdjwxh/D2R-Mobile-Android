"""Android graphics UBO bounds ABI; patch only the isolated Ryujinx copy."""
from pathlib import Path


def patch_guarded_uniforms(root: Path):
    base = root / 'src/Ryujinx.Graphics.Shader'

    def replace(path, old, new):
        target = base / path
        text = target.read_text(encoding='utf-8')
        assert text.count(old) == 1, f'Guarded UBO upstream anchor changed: {path}: {old[:60]}'
        target.write_text(text.replace(old, new), encoding='utf-8')

    replace('StructuredIr/BufferDefinition.cs', '        public BufferLayout Layout { get; }',
            '        public int RawConstantBufferSlot { get; }\n        public BufferLayout Layout { get; }')
    replace('StructuredIr/BufferDefinition.cs', 'string name, StructureType type)',
            'string name, StructureType type, int rawConstantBufferSlot = -1)')
    replace('StructuredIr/BufferDefinition.cs', '            Layout = layout;',
            '            RawConstantBufferSlot = rawConstantBufferSlot;\n            Layout = layout;')
    replace('Translation/ResourceManager.cs', 'AddNewConstantBuffer(setAndBinding.SetIndex, setAndBinding.Binding, $"{_stagePrefix}_c{slotNumber}");',
            'AddNewConstantBuffer(setAndBinding.SetIndex, setAndBinding.Binding, $"{_stagePrefix}_c{slotNumber}", slot);')
    replace('Translation/ResourceManager.cs', 'private void AddNewConstantBuffer(int setIndex, int binding, string name)',
            'private void AddNewConstantBuffer(int setIndex, int binding, string name, int rawSlot)')
    replace('Translation/ResourceManager.cs', 'Properties.AddOrUpdateConstantBuffer(new(BufferLayout.Std140, setIndex, binding, name, type));',
            'Properties.AddOrUpdateConstantBuffer(new(BufferLayout.Std140, setIndex, binding, name, type, rawSlot));')
    replace('CodeGen/Spirv/CodeGenContext.cs', '        public Dictionary<int, Instruction> ConstantBuffers { get; } = new();',
            '        public Instruction GuardedUniformLengths { get; set; }\n        public Dictionary<int, Instruction> ConstantBuffers { get; } = new();')
    replace('CodeGen/Spirv/Declarations.cs', '            DeclareConstantBuffers(context, context.Properties.ConstantBuffers.Values);',
            '            DeclareGuardedUniformLengths(context);\n            DeclareConstantBuffers(context, context.Properties.ConstantBuffers.Values);')
    replace('CodeGen/Spirv/Declarations.cs', '        private static void DeclareConstantBuffers(CodeGenContext context, IEnumerable<BufferDefinition> buffers)', '''        private static void DeclareGuardedUniformLengths(CodeGenContext context)
        {
            if (context.Definitions.Stage != ShaderStage.Vertex && context.Definitions.Stage != ShaderStage.Fragment) return;
            var array = context.GetType(AggregateType.Array | AggregateType.U32, 32);
            context.Decorate(array, Decoration.ArrayStride, (LiteralInteger)4);
            var block = context.TypeStruct(false, array);
            context.Decorate(block, Decoration.Block);
            context.MemberDecorate(block, 0, Decoration.Offset, (LiteralInteger)0);
            var variable = context.Variable(context.TypePointer(StorageClass.PushConstant, block), StorageClass.PushConstant);
            context.Name(variable, "android_guarded_uniform_vec4_lengths_v1");
            context.AddGlobalVariable(variable);
            context.GuardedUniformLengths = variable;
        }

        private static void DeclareConstantBuffers(CodeGenContext context, IEnumerable<BufferDefinition> buffers)''')
    replace('CodeGen/Spirv/Instructions.cs', '''        private static OperationResult GenerateLoadOrStore(CodeGenContext context, AstOperation operation, bool isStore)
        {
            SpvInstruction pointer = GetStoragePointer(context, operation, out AggregateType varType);''', '''        private static OperationResult GenerateLoadOrStore(CodeGenContext context, AstOperation operation, bool isStore)
        {
            // Static cbuf operands and dynamic LDC both reach this Load lowering.
            // Clamp BEFORE creating the pointer/load; a select after an OOB load is unsafe.
            if (!isStore && context.GuardedUniformLengths != null && operation.StorageKind == StorageKind.ConstantBuffer)
            {
                if (operation.GetSource(0) is not AstOperand binding || binding.Type != OperandType.Constant)
                    throw new InvalidOperationException("Guarded UBO requires a constant binding");
                var buffer = context.Properties.ConstantBuffers[binding.Value];
                int slot = buffer.RawConstantBufferSlot;
                if (slot >= 3 && slot < 19)
                {
                    if (operation.SourcesCount != 4 || operation.GetSource(1) is not AstOperand field ||
                        field.Type != OperandType.Constant || field.Value != 0 || buffer.Type.Fields.Length != 1 ||
                        buffer.Type.Fields[0].Type != (AggregateType.Array | AggregateType.Vector4 | AggregateType.FP32) ||
                        buffer.Type.Fields[0].ArrayLength != 4096)
                        throw new InvalidOperationException("Guarded UBO layout changed");
                    var u32 = context.TypeU32();
                    var zero = context.Constant(u32, 0);
                    var vec = context.GetU32(operation.GetSource(2));
                    var elem = context.GetU32(operation.GetSource(3));
                    int pushSlot = slot - 3 + (context.Definitions.Stage == ShaderStage.Fragment ? 16 : 0);
                    var lengthPointer = context.AccessChain(context.TypePointer(StorageClass.PushConstant, u32),
                        context.GuardedUniformLengths, zero, context.Constant(u32, pushSlot));
                    var length = context.Load(u32, lengthPointer);
                    var valid = context.LogicalAnd(context.TypeBool(),
                        context.ULessThan(context.TypeBool(), vec, length),
                        context.ULessThan(context.TypeBool(), vec, context.Constant(u32, 4096)));
                    valid = context.LogicalAnd(context.TypeBool(), valid,
                        context.ULessThan(context.TypeBool(), elem, context.Constant(u32, 4)));
                    var safeVec = context.Select(u32, valid, vec, zero);
                    var safeElem = context.Select(u32, valid, elem, zero);
                    var fp32 = context.GetType(AggregateType.FP32);
                    var safePointer = context.AccessChain(context.TypePointer(StorageClass.Uniform, fp32),
                        context.ConstantBuffers[binding.Value], zero, safeVec, safeElem);
                    var loaded = context.Load(fp32, safePointer);
                    return new OperationResult(AggregateType.FP32,
                        context.Select(fp32, valid, loaded, context.Constant(fp32, 0f)));
                }
            }
            SpvInstruction pointer = GetStoragePointer(context, operation, out AggregateType varType);''')
