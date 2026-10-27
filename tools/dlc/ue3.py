"""Unreal Engine 3 (Gears of War 3, Xbox 360) packages and script patches.

Reads a cooked package (file version 828, big-endian) once it is uncompressed: summary, name table,
imports, exports, and the serialized data of classes and default objects. Writes script patches
(FLinkerPatchData), the format the engine reads from GearGame/Patches/ScriptPatch_<Package>.bin, and
packs them as an Unreal Engine 3 compressed chunk.

Object references are 32-bit indices: positive for exports (1-based), negative for imports, 0 for
none. A patch's own exports come after the package's: export N + 1 is the patch's first.
"""

import struct

from lzx import TAG, decompress_chunk

# Object flags (64-bit), as the engine writes them for script objects.
RF_PUBLIC_LOADED = 0x0007000400000000      # public, loaded on clients, servers and in the editor
RF_STANDALONE = 0x0008000000000000
RF_NATIVE = 0x0400000000000000
RF_CLASS_DEFAULT_OBJECT = 0x0000000000000200

# Class flags.
CLASS_ABSTRACT = 0x1
CLASS_COMPILED = 0x2
CLASS_PARSED = 0x10
CLASS_NATIVE = 0x80
CLASS_NOEXPORT = 0x100
CLASS_NATIVE_REPLICATION = 0x800
CLASS_EXPORTED = 0x08000000
CLASS_INTRINSIC = 0x10000000


class Reader:
    def __init__(self, data, pos=0):
        self.d, self.p = data, pos

    def u8(self):
        v = self.d[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from('>H', self.d, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from('>I', self.d, self.p)[0]; self.p += 4; return v

    def i32(self):
        v = struct.unpack_from('>i', self.d, self.p)[0]; self.p += 4; return v

    def u64(self):
        v = struct.unpack_from('>Q', self.d, self.p)[0]; self.p += 8; return v

    def fstr(self):
        n = self.i32()
        if n < 0:
            s = self.d[self.p:self.p - 2 * n].decode('utf-16-be'); self.p += -2 * n
        else:
            s = self.d[self.p:self.p + n].decode('latin-1'); self.p += n
        return s.rstrip('\0')

    def name(self):
        return self.i32(), self.i32()

    def take(self, n):
        v = self.d[self.p:self.p + n]; self.p += n; return v


class Writer:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v): self.b += bytes([v])
    def u16(self, v): self.b += struct.pack('>H', v)
    def u32(self, v): self.b += struct.pack('>I', v & 0xFFFFFFFF)
    def i32(self, v): self.b += struct.pack('>i', v)
    def u64(self, v): self.b += struct.pack('>Q', v)
    def raw(self, v): self.b += v

    def fstr(self, s):
        data = s.encode('latin-1') + b'\0'
        self.i32(len(data)); self.b += data

    def name(self, index, number=0):
        self.i32(index); self.i32(number)


def read_game_package(path):
    """An uncompressed package from a cooked .xxx file (fully compressed or with chunks)."""
    data = open(path, 'rb').read()
    if struct.unpack_from('>II', data, 0) == (TAG, 0x20000):
        return decompress_chunk(data)
    raise ValueError(f'{path}: only fully compressed packages are supported')


class Package:
    def __init__(self, data):
        self.d = data
        r = Reader(data)
        assert r.u32() == TAG, 'not a package'
        self.version = r.u32()
        r.i32()  # header size
        r.fstr()  # folder name
        r.u32()  # package flags
        name_count, name_offset = r.i32(), r.i32()
        export_count, export_offset = r.i32(), r.i32()
        import_count, import_offset = r.i32(), r.i32()
        r.i32()  # depends offset
        r.take(16)  # import/export guids and thumbnail table
        r.take(16)  # package guid
        generations = [(r.i32(), r.i32(), r.i32()) for _ in range(r.i32())]
        self.net_object_count = generations[-1][2]

        r.p = name_offset
        self.names = []
        for _ in range(name_count):
            self.names.append(r.fstr()); r.u64()
        self.name_index = {n: i for i, n in enumerate(self.names)}

        r.p = import_offset
        self.imports = []
        for _ in range(import_count):
            class_package, class_name, outer, name = r.name(), r.name(), r.i32(), r.name()
            self.imports.append(dict(class_package=self.names[class_package[0]],
                                     class_name=self.names[class_name[0]], outer=outer, name=self.text(name)))

        r.p = export_offset
        self.exports = []
        for _ in range(export_count):
            e = dict(cls=r.i32(), super=r.i32(), outer=r.i32(), name=self.text(r.name()), archetype=r.i32(),
                     flags=r.u64(), size=r.i32(), offset=r.i32())
            e['export_flags'] = r.u32()
            for _ in range(r.i32()):  # generation net object counts
                r.i32()
            r.take(16)  # package guid
            e['package_flags'] = r.u32()
            self.exports.append(e)
        self.paths = {}
        for i in range(1, len(self.exports) + 1):
            self.paths.setdefault(self.path(i), i)
        for k in range(len(self.imports)):
            self.paths.setdefault(self.path(-k - 1), -k - 1)

    def text(self, name):
        index, number = name
        return self.names[index] if number == 0 else f'{self.names[index]}_{number - 1}'

    def obj(self, index):
        return self.exports[index - 1] if index > 0 else self.imports[-index - 1]

    def obj_name(self, index):
        return self.obj(index)['name'] if index else 'None'

    def path(self, index):
        """Full path of an object reference, from the package (Outer.Outer.Name)."""
        parts = []
        while index:
            o = self.obj(index); parts.append(o['name']); index = o['outer']
        return '.'.join(reversed(parts))

    def class_name(self, index):
        if index < 0:
            return self.imports[-index - 1]['class_name']
        cls = self.exports[index - 1]['cls']
        return 'Class' if cls == 0 else self.obj_name(cls)

    def data(self, index):
        e = self.exports[index - 1]
        return self.d[e['offset']:e['offset'] + e['size']]

    def find(self, path):
        """Index of the export or import with this path (Package.Outer.Name for imports)."""
        return self.paths.get(path, 0)

    def find_class(self, name):
        for i, e in enumerate(self.exports, 1):
            if e['cls'] == 0 and e['name'] == name:
                return i
        return 0

    def children(self, outer):
        return [i for i, e in enumerate(self.exports, 1) if e['outer'] == outer]


def parse_class(blob):
    """Fields of a serialized UClass (version 828, cooked)."""
    r = Reader(blob)
    c = dict(net_index=r.i32(), next=r.i32(), super=r.i32(), children=r.i32())
    r.i32()
    script_size = r.i32()
    c['script'] = r.take(script_size)
    c['probe_mask'] = r.u32()
    c['label_table_offset'] = r.u16()
    c['state_flags'] = r.u32()
    c['func_map'] = [(r.name(), r.i32()) for _ in range(r.i32())]
    c['class_flags'] = r.u32()
    c['within'] = r.i32()
    c['config_name'] = r.name()
    c['components'] = [(r.name(), r.i32()) for _ in range(r.i32())]
    c['interfaces'] = [(r.i32(), r.i32()) for _ in range(r.i32())]
    c['dll_bind_name'] = r.name()
    c['default_object'] = r.i32()
    assert r.p == len(blob), 'unexpected class data'
    return c


def parse_tags(pkg, blob, pos):
    """Tagged properties at `pos` (after the object's NetIndex): (header bytes, name, type, value)."""
    r = Reader(blob, pos)
    tags = []
    while True:
        start = r.p
        name = pkg.text(r.name())
        if name == 'None':
            break
        kind = pkg.text(r.name())
        size = r.i32()
        r.i32()  # array index
        if kind == 'BoolProperty':
            r.u8()
        elif kind in ('StructProperty', 'ByteProperty'):
            r.name()
        header = blob[start:r.p]
        tags.append((header, name, kind, r.take(size)))
    return tags, r.p


class Patch:
    """A script patch (FLinkerPatchData) to write."""

    def __init__(self, package):
        self.package = package
        self.names = []
        self.exports = []        # dicts: cls, super, outer, name, archetype, flags
        self.imports = []
        self.new_objects = []    # (export name, data), one per export, in export order
        self.defaults = []       # (default object name, data)
        self.enums = []          # (name, path, values)
        self.scripts = []        # (function name, path, bytecode)

    def write(self):
        w = Writer()

        def blobs(items):
            w.u32(len(items))
            for name, data in items:
                w.fstr(name); w.u32(len(data)); w.raw(data)

        w.fstr(self.package)
        w.u32(len(self.names))
        for n in self.names:
            w.fstr(n)
        w.u32(len(self.exports))
        for e in self.exports:
            w.i32(e['cls']); w.i32(e['super']); w.i32(e['outer']); w.fstr(e['name'])
            w.i32(e['archetype']); w.u64(e['flags'])
            w.raw(bytes(36))  # serial size and offset, export flags, net counts, guid, package flags
        w.u32(len(self.imports))
        for i in self.imports:
            w.fstr(i['class_package']); w.fstr(i['class_name']); w.i32(i['outer']); w.fstr(i['name'])
        blobs(self.new_objects)
        blobs(self.defaults)
        w.u32(len(self.enums))
        for name, path, values in self.enums:
            w.fstr(name); w.fstr(path); w.u32(len(values))
            for v in values:
                w.fstr(v)
        w.u32(len(self.scripts))
        for name, path, code in self.scripts:
            w.fstr(name); w.fstr(path); w.u32(len(code)); w.raw(code)
        return bytes(w.b)


def pack(data):
    """`data` as a UE3 compressed chunk of stored (uncompressed) XMemCompress LZX blocks."""
    block_size, frame = 0x20000, 0x8000
    blocks = [data[i:i + block_size] for i in range(0, len(data), block_size)] or [b'']
    streams = []
    for block in blocks:
        # LZX bits, most significant first in 16-bit little-endian words: no Intel E8 translation
        # (1 bit), block type 3 = uncompressed (3 bits), length (24 bits), padding to 32 bits; then
        # R0-R2. XMemCompress splits the stream into frames of 32 KB of output.
        value = ((3 << 24) | len(block)) << 4
        header = struct.pack('<HHIII', (value >> 16) & 0xFFFF, value & 0xFFFF, 1, 1, 1)
        stream = header + block
        out = bytearray()
        pos = 0
        for start in range(0, len(block), frame):
            size = min(frame, len(block) - start)
            take = size + (len(header) if start == 0 else 0)
            payload = stream[pos:pos + take]; pos += take
            out += struct.pack('>H', len(payload)) if size == frame else b'\xff' + struct.pack('>HH', size, len(payload))
            out += payload
        out += bytes(9)  # end of stream (an empty frame) and padding, as XMemCompress writes them
        streams.append(bytes(out))
    head = struct.pack('>IIII', TAG, block_size, sum(map(len, streams)), len(data))
    for block, stream in zip(blocks, streams):
        head += struct.pack('>II', len(stream), len(block))
    return head + b''.join(streams)
