"""Conservative FUSE facts from the exact Android SPIR-V bytes being packaged.

This is an offline proof producer, not permission to sample a live attachment.
Runtime must prove view/UV/state equivalence and use Vulkan input attachments.
Instruction word offsets and result IDs identify rewrite sites only for sha256.
Unknown control flow, pointer provenance or arithmetic never becomes a fact.
"""
import hashlib
import math
import struct


SCHEMA = 1


def analyze(code, reflection):
    module = FragmentModule if reflection['stage'] == 'ps' else Module
    return module(code, reflection).facts()


def input_attachment_variant(code, facts, mapping):
    """Return (SPIR-V bytes, descriptor ABI) for runtime-proven fetch slots.

    mapping is {(descriptor_set, binding): input_attachment_index}. The caller
    must first prove the runtime same-pixel/view conditions and establish the
    subpass dependency. This function proves only the static shader rewrite.
    It does not authorize same-subpass feedback or change a runtime descriptor.
    """
    if (facts.get('schema') != SCHEMA or facts.get('stage') != 'ps' or
            facts.get('spirv_sha256') != hashlib.sha256(code).hexdigest()):
        raise ValueError('FUSE facts schema/stage/SPIR-V identity mismatch')
    if not mapping:
        raise ValueError('Empty input attachment mapping')
    for key, index in mapping.items():
        if (not isinstance(key, tuple) or len(key) != 2 or
                any(type(x) is not int or x < 0 for x in key) or
                type(index) is not int or not 0 <= index < 0x100000000):
            raise ValueError('Invalid descriptor/input attachment mapping')
    module = FragmentModule(code, {'stage': 'ps'})
    checked = module.facts()
    # Re-derive sample proofs so a stale/tampered eligible flag is not authority.
    if checked['samples'] != facts.get('samples'):
        raise ValueError('FUSE sample facts do not match actual SPIR-V')
    selected = [s for s in checked['samples']
                if (s.get('descriptor_set'), s.get('binding')) in mapping]
    if ({(s['descriptor_set'], s['binding']) for s in selected} != set(mapping) or
            not all(s['eligible'] for s in selected)):
        raise ValueError('Requested slot is missing, mixed-use or unproven')
    variables = {s['variable_id'] for s in selected}
    sites = {s['word_offset'] for s in selected}
    image_values = set()
    rewrites = {}
    for offset, op, a in module.ins:
        if op == 59 and a[1] in variables:
            if len(a) != 3 or a[2] != 0:
                raise ValueError('Unsupported descriptor variable')
            rewrites[offset] = 'variable'
        elif op == 61 and a[2] in variables:
            if len(a) != 3:
                raise ValueError('Unsupported descriptor load operands')
            image_values.add(a[1])
            rewrites[offset] = 'value'
        elif op == 83 and a[2] in image_values:
            image_values.add(a[1])
            rewrites[offset] = 'value'
        elif offset in sites:
            if op != 87 or a[2] not in image_values:
                raise ValueError('Sample has unsupported descriptor alias')
            rewrites[offset] = 'sample'
    # A sampler stored in a local or consumed by another operation would need
    # additional type/semantic changes. Reject it instead of partially rewriting.
    related = variables | image_values
    for offset, op, a in module.ins:
        if offset in rewrites or op in (5, 6, 8, 15, 71, 72, 317):
            continue
        if op <= 53:  # Debug/imports/types/constants cannot consume these IDs.
            continue
        if related.intersection(a):
            raise ValueError('Unsupported descriptor use at word %d' % offset)
    if any(43 in module.decs.get(v, {}) for v in variables):
        raise ValueError('Descriptor already has InputAttachmentIndex')

    next_id = module.words[3]
    def new_id():
        nonlocal next_id
        result = next_id
        next_id += 1
        return result

    def instruction(op, *args):
        return [((len(args) + 1) << 16) | op, *args]

    # Allocate fresh image and pointer types; never mutate a shared sampled-image
    # type, because nonselected descriptor slots must remain ordinary samplers.
    first_variable_offset, first_var = next((o, a[1]) for o, op, a in module.ins if op == 59 and a[1] in variables)
    old_pointer = module.types[module.variables[first_var][0]][1]
    old_sampled = module.types[old_pointer[1]][1]
    component = module.types[old_sampled[0]][1][0]
    image_type, pointer_type = new_id(), new_id()
    definitions = instruction(25, image_type, component, 6, 0, 0, 0, 2, 0)
    definitions += instruction(32, pointer_type, 0, image_type)
    int_type = next((i for i, t in module.types.items() if t == (21, (32, 1))), None)
    # Coordinate declarations go after existing globals, before the function.
    # This reuses integer types even when they follow the descriptor variable.
    coordinates = []
    if int_type is None:
        int_type = new_id()
        coordinates += instruction(21, int_type, 32, 1)
    vector_type = next((i for i, t in module.types.items() if t == (23, (int_type, 2))), None)
    if vector_type is None:
        vector_type = new_id()
        coordinates += instruction(23, vector_type, int_type, 2)
    zero, coords = new_id(), new_id()
    coordinates += instruction(43, int_type, zero, 0)
    coordinates += instruction(44, vector_type, coords, zero, zero)
    annotations = []
    abi = []
    for variable in sorted(variables):
        key = module.descriptor(variable)
        annotations += instruction(71, variable, 43, mapping[key])
        abi.append({'descriptor_set': key[0], 'binding': key[1],
                    'input_attachment_index': mapping[key], 'descriptor_type': 'input_attachment'})
    output = list(module.words[:5])
    if not any(op == 17 and a == (40,) for _, op, a in module.ins):
        output += instruction(17, 40)
    added_annotations = False
    for offset, op, a in module.ins:
        if not added_annotations and 19 <= op <= 33:
            output += annotations
            added_annotations = True
        if offset == first_variable_offset:
            output += definitions
        if op == 54:
            output += coordinates
        kind = rewrites.get(offset)
        if kind == 'variable':
            output += instruction(op, pointer_type, *a[1:])
        elif kind == 'value':
            output += instruction(op, image_type, *a[1:])
        elif kind == 'sample':
            output += instruction(98, a[0], a[1], a[2], coords)
        else:
            output += instruction(op, *a)
    output[3] = next_id
    result = struct.pack('<%dI' % len(output), *output)
    return result, {'schema': SCHEMA, 'source_sha256': module.sha,
                    'spirv_sha256': hashlib.sha256(result).hexdigest(), 'bindings': abi}


class Module:
    def __init__(self, code, reflection):
        if len(code) < 20 or len(code) % 4:
            raise ValueError('Malformed SPIR-V size')
        self.words = struct.unpack('<%dI' % (len(code) // 4), code)
        if self.words[0] != 0x07230203:
            raise ValueError('Invalid SPIR-V magic')
        self.sha = hashlib.sha256(code).hexdigest()
        self.reflection = reflection
        self.ins = []
        at = 5
        while at < len(self.words):
            count, op = self.words[at] >> 16, self.words[at] & 65535
            if not count or at + count > len(self.words):
                raise ValueError('Malformed SPIR-V instruction at word %d' % at)
            self.ins.append((at, op, self.words[at + 1:at + count]))
            at += count
        self.types, self.decs, self.members, self.values = {}, {}, {}, {}
        self.ptr, self.variables, self.memory, self.imports = {}, {}, {}, {}
        self.memory_keys = {}
        self.value_types = {}
        self.entries = []
        for _, op, a in self.ins:
            if op == 11:
                self.imports[a[0]] = struct.pack('<%dI' % len(a[1:]), *a[1:]).split(b'\0')[0]
            elif op == 15:
                self.entries.append((a[0], a[1]))
            elif 19 <= op <= 33:
                self.types[a[0]] = (op, a[1:])
            elif op == 71:
                self.decs.setdefault(a[0], {})[a[1]] = a[2:]
            elif op == 72:
                self.members.setdefault((a[0], a[1]), {})[a[2]] = a[3:]
            elif op in (41, 42, 43, 44, 46):
                self.value_types[a[1]] = a[0]
                typ, args = self.types.get(a[0], (0, ()))
                if op in (41, 42):
                    self.values[a[1]] = ('const', op == 41)
                elif op == 43 and typ == 21 and args[0] == 32:
                    n = a[2]
                    if args[1] and n & 0x80000000:
                        n -= 0x100000000
                    self.values[a[1]] = ('const', n)
                elif op == 43 and typ == 22 and args[0] == 32:
                    f = struct.unpack('<f', struct.pack('<I', a[2]))[0]
                    self.values[a[1]] = ('const', f) if math.isfinite(f) else None
                elif op == 44:
                    self.values[a[1]] = ('vector', tuple(self.values.get(i) for i in a[2:]))
                elif op == 46 and typ in (21, 22):
                    self.values[a[1]] = ('const', 0)
            elif op == 59:
                self.variables[a[1]] = (a[0], a[2])
                self.ptr[a[1]] = (a[1], ())
        # A straight-line function can be interpreted without speculative path
        # merging. Reject branches/calls globally until a real CFG proof exists.
        self.linear = (len(self.entries) == 1 and
                       self.entries[0][0] == {'vs': 0, 'ps': 4}.get(reflection['stage']) and
                       sum(op == 54 for _, op, _ in self.ins) == 1 and
                       sum(op == 248 for _, op, _ in self.ins) == 1 and
                       sum(op == 253 for _, op, _ in self.ins) == 1)
        # Decoration groups can hide Flat/NoPerspective (or descriptor facts).
        # Until group expansion is implemented, reject the module rather than
        # interpreting an incompletely decorated interface as smooth varying.
        self.linear &= not any(op in (57, 73, 74, 75, 245, 246, 247, 249, 250, 251, 252, 254, 255, 4416, 5380)
                               for _, op, _ in self.ins)
        self.samples = []
        self.resource_uses = {}
        self.unknown_resource_use = False
        self.output_unknown = False

    def const(self, value):
        expr = self.values.get(value)
        return expr[1] if expr and expr[0] == 'const' and isinstance(expr[1], int) else None

    def location(self, variable, indices):
        """Return (decoration map, remaining component indices)."""
        dec = self.decs.get(variable, {})
        type_id = self.types[self.variables[variable][0]][1][1]
        if self.types.get(type_id, (0,))[0] == 30 and indices:
            dec = self.members.get((type_id, indices[0]), {})
            indices = indices[1:]
        return dec, indices

    def descriptor(self, variable):
        d = self.decs.get(variable, {})
        if 33 not in d or 34 not in d:
            return None
        return (d[34][0], d[33][0])

    def read(self, pointer, type_id):
        if pointer is None:
            return None
        variable, indices = pointer
        _, storage = self.variables[variable]
        if storage == 0 and not indices:
            desc = self.descriptor(variable)
            return ('resource', variable, desc) if desc else None
        if storage == 1:
            dec, components = self.location(variable, indices)
            if 30 not in dec or 31 in dec or any(x in dec for x in (11, 13, 14, 16, 17)):
                return None
            loc = dec[30][0]
            if len(components) == 1:
                return ('input', loc, components[0])
            op, a = self.types.get(type_id, (0, ()))
            if not components and op == 23:
                return ('vector', tuple(('input', loc, c) for c in range(a[1])))
        if storage == 2:
            desc = self.descriptor(variable)
            cbuf = next((b for b in self.reflection.get('cbufs', [])
                         if desc == (0, b['binding']) and b['slot'] >= 3), None)
            if cbuf and len(indices) == 3 and indices[0] == 0 and 0 <= indices[2] < 4:
                return ('ubo', cbuf['slot'] - 3, indices[1], indices[2])
        if storage in (3, 6, 7):
            if pointer in self.memory:
                return self.memory[pointer]
            for n in range(len(indices) - 1, -1, -1):
                value = self.memory.get((variable, indices[:n]))
                if value is not None:
                    for c in indices[n:]:
                        value = self.component(value, c)
                    return value
            op, args = self.types.get(type_id, (0, ()))
            if op == 23:
                return ('vector', tuple(self.memory.get((variable, indices + (c,))) for c in range(args[1])))
        return None

    @staticmethod
    def component(value, index):
        if value and value[0] == 'vector' and 0 <= index < len(value[1]):
            return value[1][index]
        return None

    def write(self, pointer, value):
        if pointer is None:
            # An unknown alias could overwrite any local/output value.
            self.memory.clear()
            self.memory_keys.clear()
            self.output_unknown = True
            return
        variable, indices = pointer
        keys = self.memory_keys.setdefault(variable, set())
        for p in tuple(keys):
            if p[1][:len(indices)] == indices:
                del self.memory[p]
                keys.remove(p)
        # Split vector writes so later per-component writes cannot leave a
        # stale ancestor vector visible to a whole-vector load.
        if value and value[0] == 'vector':
            for c, item in enumerate(value[1]):
                self.write((variable, indices + (c,)), item)
        else:
            self.memory[pointer] = value
            keys.add(pointer)

    def run(self):
        for offset, op, a in self.ins:
            self.current_offset = offset
            if op in (65, 66):  # AccessChain / InBoundsAccessChain
                base = self.ptr.get(a[2])
                indices = tuple(self.const(i) for i in a[3:])
                self.ptr[a[1]] = (base[0], base[1] + indices) if base and None not in indices else None
            elif op == 61:
                self.values[a[1]] = self.read(self.ptr.get(a[2]), a[0])
                self.value_types[a[1]] = a[0]
            elif op == 62:
                self.write(self.ptr.get(a[0]), self.values.get(a[1]))
            elif op in (63, 64):
                self.memory.clear()
                self.memory_keys.clear()
                self.output_unknown = True
            elif op == 80:
                parts = []
                for i in a[2:]:
                    value = self.values.get(i)
                    parts.extend(value[1] if value and value[0] == 'vector' else (value,))
                self.values[a[1]] = ('vector', tuple(parts))
            elif op == 81:
                value = self.values.get(a[2])
                for index in a[3:]:
                    value = self.component(value, index)
                self.values[a[1]] = value
            elif op == 79:
                x, y = self.values.get(a[2]), self.values.get(a[3])
                if x and y and x[0] == y[0] == 'vector':
                    values = x[1] + y[1]
                    self.values[a[1]] = ('vector', tuple(values[i] if i < len(values) else None for i in a[4:]))
            elif op == 83:
                self.values[a[1]] = self.values.get(a[2])
                if a[2] in self.ptr:
                    self.ptr[a[1]] = self.ptr[a[2]]
            elif op == 127:  # FNegate: retain Y flip in actual Vulkan position.
                value = self.values.get(a[2])
                self.values[a[1]] = ('neg', value) if value else None
            elif op == 129:
                x, y = self.values.get(a[2]), self.values.get(a[3])
                # Only add +/-0 is accepted. Do not algebraically simplify
                # arbitrary floating arithmetic into an equivalent formula.
                self.values[a[1]] = x if y == ('const', 0) else y if x == ('const', 0) else None
            elif op == 12 and self.imports.get(a[2]) == b'GLSL.std.450' and a[3] == 50:
                args = tuple(self.values.get(i) for i in a[4:])
                self.values[a[1]] = ('fma', *args) if len(args) == 3 and all(args) else None
            elif op == 12 and any(i in self.ptr for i in a[4:]):
                # GLSL Modf/Frexp pointer forms can write a function local.
                # Do not keep a value provenance across an unmodelled write.
                self.linear = False
                self.memory.clear()
                self.memory_keys.clear()
                self.output_unknown = True
            elif op == 156:  # IsNan
                value = self.values.get(a[2])
                self.values[a[1]] = ('isnan', value) if value else None
            elif op == 169:
                cond, yes, no = (self.values.get(i) for i in a[2:5])
                zero = yes and yes[0] == 'vector' and all(x == ('const', 0) for x in yes[1])
                if cond == ('isnan', no) and zero and no:
                    self.values[a[1]] = ('nan_zero', no)
                # Guarded UBO select deliberately stays unknown: a future
                # affine proof must emulate its length predicate, not erase it.
            elif op == 86:
                self.values[a[1]] = None  # Separate image/sampler unsupported.
            elif 87 <= op <= 107:
                image = self.values.get(a[2]) if len(a) > 2 else None
                if op == 99:  # ImageWrite has no result type/result ID.
                    image = self.values.get(a[0])
                if image and image[0] == 'resource':
                    key = image[2]
                    self.resource_uses[key] = self.resource_uses.get(key, 0) + 1
                else:
                    self.unknown_resource_use = True
                if op == 87:
                    self.sample(offset, a, image)

    def sample(self, offset, a, image):
        site = {'word_offset': offset, 'opcode': 87, 'word_count': len(a) + 1,
                'result_id': a[1], 'result_type_id': a[0],
                'sampled_image_id': a[2], 'coordinate_id': a[3], 'eligible': False}
        self.samples.append(site)
        if not image or image[0] != 'resource':
            site['reason'] = 'unknown_image'
            return
        variable, (desc_set, binding) = image[1:]
        site.update(descriptor_set=desc_set, binding=binding, variable_id=variable)
        pointer_type = self.variables[variable][0]
        t = self.types[self.types[pointer_type][1][1]]
        t = self.types.get(t[1][0], (0, ())) if t[0] == 27 else (0, ())
        # 2D, non-depth, non-array, non-MS, sampled float32 image only.
        if t[0] != 25 or t[1][1:6] != (1, 0, 0, 0, 1) or self.types.get(t[1][0]) != (22, (32,)):
            site['reason'] = 'unsupported_image_type'
            return
        if len(a) > 4 and a[4:] != (0,):
            site['reason'] = 'image_operands'
            return
        coord = self.values.get(a[3])
        site['nan_zero'] = bool(coord and coord[0] == 'nan_zero')
        if site['nan_zero']:
            coord = coord[1]
        if not coord or coord[0] != 'vector' or len(coord[1]) != 2:
            site['reason'] = 'unknown_coordinate'
            return
        x, y = coord[1]
        if not x or not y or x[0] != 'input' or y != ('input', x[1], 1) or x[2] != 0:
            site['reason'] = 'coordinate_not_direct_varying'
            return
        site.update(varying_location=x[1], eligible=bool(self.linear), reason='candidate' if self.linear else 'control_flow')

    def raster(self):
        # Declared outputs are a safe over-approximation even when a branch or
        # unknown alias prevents reconstructing final stores.
        builtins = [d[11][0] for d in (*self.decs.values(), *self.members.values()) if 11 in d]
        result = {'position': None, 'uv_from_attr': {}, 'uv_affine': {},
                  'may_discard': any(op in (252, 4416, 5380) for _, op, _ in self.ins),
                  'color_locations': [], 'writes_sample_mask': 20 in builtins,
                  'writes_frag_depth': 22 in builtins}
        positions, varyings = {}, {}
        for (var, indices), value in self.memory.items():
            if self.variables[var][1] != 3:
                continue
            dec, components = self.location(var, indices)
            builtin = dec.get(11, (None,))[0]
            if builtin == 20:
                result['writes_sample_mask'] = True
            if builtin in (3, 4):  # Clip/cull distances invalidate simple raster proof.
                self.output_unknown = True
            if len(components) != 1:
                continue
            if builtin == 0:
                positions[components[0]] = value
            if 30 in dec and 31 not in dec:
                location = dec[30][0]
                varyings.setdefault(location, {})[components[0]] = value
        result['color_locations'] = sorted(varyings) if self.reflection['stage'] == 'ps' else []
        if not self.linear or self.output_unknown or self.reflection['stage'] != 'vs':
            return result
        xyz, signs = [], []
        for c in range(3):
            value = positions.get(c)
            sign = -1 if value and value[0] == 'neg' else 1
            value = value[1] if sign == -1 else value
            xyz.append(value)
            signs.append(sign)
        w = positions.get(3)
        if xyz[0] and xyz[0][0] == 'input' and all(xyz[c] == ('input', xyz[0][1], c) for c in range(3)):
            if w == ('const', 1) or w == ('input', xyz[0][1], 3):
                result['position'] = {'attribute': xyz[0][1], 'xyz_sign': signs,
                                      'w': 'one' if w == ('const', 1) else 'attribute'}
        for loc, components in varyings.items():
            x, y = components.get(0), components.get(1)
            if x and x[0] == 'input' and x[2] == 0 and y == ('input', x[1], 1):
                result['uv_from_attr'][str(loc)] = x[1]
            elif x and y and x[0] == y[0] == 'fma':
                ax, ay = x[1], y[1]
                coeffs = (x[2], x[3], y[2], y[3])
                if ax[0] == 'input' and ax[2] == 0 and ay == ('input', ax[1], 1) and all(c[0] in ('const', 'ubo') for c in coeffs):
                    result['uv_affine'][str(loc)] = {'attribute': ax[1], 'operation': 'fma',
                                                    'coefficients': dict(zip(('sx', 'ox', 'sy', 'oy'), coeffs))}
        return result

    def facts(self):
        self.run()
        eligible = {}
        for site in self.samples:
            if not self.linear and site['eligible']:
                site.update(eligible=False, reason='control_flow_or_pointer_effect')
            if site['eligible']:
                key = (site['descriptor_set'], site['binding'])
                eligible[key] = eligible.get(key, 0) + 1
        mixed = [list(k) for k, count in sorted(self.resource_uses.items()) if count != eligible.get(k, 0)]
        # Unknown resource use prevents a whole-slot equivalence proof.
        for site in self.samples:
            if site['eligible'] and (self.unknown_resource_use or [site['descriptor_set'], site['binding']] in mixed):
                site.update(eligible=False, reason='mixed_or_unknown_resource_use')
        return {'schema': SCHEMA, 'spirv_sha256': self.sha, 'word_count': len(self.words),
                'stage': self.reflection['stage'], 'straight_line': bool(self.linear),
                'raster': self.raster(), 'samples': self.samples, 'mixed_bindings': mixed,
                'unknown_resource_use': self.unknown_resource_use}


class FragmentModule(Module):
    """Additional FS proof without interpreting mutable locals across a CFG.

    Preserve the existing straight-line proof verbatim. Otherwise only immutable
    Input/UniformConstant SSA and a restricted entry-block single-store alias
    are available. No Phi, path merging, sequential local-memory interpretation
    or guessed dominance is used. Every converted sample must precede the first
    entry-block branch/terminator; later uses make the whole binding mixed.
    """
    def __init__(self, code, reflection):
        super().__init__(code, reflection)
        self.original_linear = self.linear
        self.entry_alias = {}
        self.entry_end = 0
        if self.original_linear:
            return
        self.linear = (len(self.entries) == 1 and self.entries[0][0] == 4 and
                       sum(op == 54 for _, op, _ in self.ins) == 1 and
                       not any(op in (57, 73, 74, 75) for _, op, _ in self.ins))
        if not self.linear:
            return
        # An entry block has no predecessors. A sole direct store in it
        # dominates every subsequent reachable load. An explicit instruction
        # offset check below rejects loads preceding that store.
        self.entry_end = next((at for at, op, _ in self.ins
                               if op in (249, 250, 251, 252, 253, 254, 255, 4416)), 0)
        locals_ = {v for v, (_, storage) in self.variables.items() if storage == 7}
        stores, escaped = {}, set()
        for at, op, args in self.ins:
            if op == 59 and len(args) > 3:
                escaped.add(args[1])
            if op <= 53 or op in (59, 71, 72, 317):
                continue
            for variable in locals_.intersection(args):
                if op == 62 and args[0] == variable and args[1] != variable:
                    stores.setdefault(variable, []).append((at, args[1]))
                elif op == 61 and args[2] == variable:
                    pass
                else:
                    # This intentionally over-rejects literal-word collisions.
                    # A missed optimization is safer than a missed pointer use.
                    escaped.add(variable)
        self.entry_alias = {v: writes[0] for v, writes in stores.items()
                            if len(writes) == 1 and writes[0][0] < self.entry_end and v not in escaped}
        self.current_offset = 0

    def read(self, pointer, type_id):
        if self.original_linear:
            return super().read(pointer, type_id)
        if pointer is None:
            return None
        if self.variables[pointer[0]][1] in (0, 1):
            return super().read(pointer, type_id)
        alias = self.entry_alias.get(pointer[0]) if not pointer[1] else None
        if alias:
            # A load before the store is not initialized, even if its source
            # happens to be a globally available constant/input SSA value.
            return self.values.get(alias[1]) if alias[0] < self.current_offset else None
        return None

    def write(self, pointer, value):
        if self.original_linear:
            super().write(pointer, value)

    def sample(self, offset, args, image):
        super().sample(offset, args, image)
        if not self.original_linear and offset >= self.entry_end:
            self.samples[-1].update(eligible=False, reason='outside_entry_block')

    def facts(self):
        result = super().facts()
        if not self.original_linear:
            result['straight_line'] = False
            result['sample_proof'] = 'immutable_ssa_entry_single_store'
            # Output locations are declarations, not speculative final stores
            # through divergent paths. Only FS facts use this alternate mode.
            locations = set()
            for variable, (type_id, storage) in self.variables.items():
                if storage != 3:
                    continue
                dec = self.decs.get(variable, {})
                if 30 in dec and 31 not in dec:
                    locations.add(dec[30][0])
                target = self.types[type_id][1][1]
                op, members = self.types.get(target, (0, ()))
                if op == 30:
                    for index in range(len(members)):
                        dec = self.members.get((target, index), {})
                        if 30 in dec and 31 not in dec:
                            locations.add(dec[30][0])
            result['raster']['color_locations'] = sorted(locations)
        return result
