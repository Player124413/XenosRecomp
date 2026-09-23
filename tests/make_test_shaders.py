#!/usr/bin/env python3
"""Generates small synthetic Xbox 360 shader containers.

The generated files contain the same container layout as the shaders found inside a
game, which makes it possible to test the recompiler without shipping any game data.
They are deliberately built around the control flow constructs that are easy to get
wrong, such as conditional jumps on boolean constant registers.

The generator also builds Xbox 360 compressed archives (the format of the "shader.ar.00"
and "shader.ar.01" files of a game), so that the decompression done by the recompiler is
tested as well.

Usage: make_test_shaders.py <output directory>
"""

import json
import os
import struct
import sys

SHADER_CONTAINER_FLAGS_PIXEL = 0x102A1100
SHADER_CONTAINER_FLAGS_VERTEX = 0x102A1101

REGISTER_SET_BOOL = 0
REGISTER_SET_INT4 = 1
REGISTER_SET_FLOAT4 = 2
REGISTER_SET_SAMPLER = 3

PARAMETER_CLASS_VECTOR = 1
PARAMETER_TYPE_FLOAT = 3
PARAMETER_TYPE_BOOL = 1

OPCODE_EXEC = 1
OPCODE_EXEC_END = 2
OPCODE_COND_EXEC = 3
OPCODE_COND_EXEC_END = 4
OPCODE_LOOP_START = 7
OPCODE_LOOP_END = 8
OPCODE_COND_JMP = 11
OPCODE_RETURN = 10

DECL_USAGE_POSITION = 0
DECL_USAGE_TEXCOORD = 5
DECL_USAGE_COLOR = 10


class BitWriter:
    """Collects the bits of a single control flow instruction."""

    def __init__(self):
        self.bits = []

    def add(self, value, count):
        for i in range(count):
            self.bits.append((value >> i) & 1)
        return self

    def finish(self):
        assert len(self.bits) == 48, len(self.bits)
        value = 0
        for i, bit in enumerate(self.bits):
            value |= bit << i
        return value


def exec_instruction(address, count, sequence, opcode, is_yield=0):
    return (BitWriter()
            .add(address, 12)
            .add(count, 3)
            .add(is_yield, 1)
            .add(sequence, 12)
            .add(0, 4)   # vertexCacheHigh
            .add(0, 2)   # vertexCacheLow
            .add(0, 7)
            .add(0, 1)   # isPredicateClean
            .add(0, 1)
            .add(0, 1)   # absoluteAddressing
            .add(opcode, 4)
            .finish())


def cond_exec_instruction(address, count, sequence, bool_address, condition, opcode):
    return (BitWriter()
            .add(address, 12)
            .add(count, 3)
            .add(0, 1)   # isYield
            .add(sequence, 12)
            .add(0, 4)   # vertexCacheHigh
            .add(0, 2)   # vertexCacheLow
            .add(bool_address, 8)
            .add(condition, 1)
            .add(0, 1)   # absoluteAddressing
            .add(opcode, 4)
            .finish())


def cond_jmp_instruction(address, bool_address, condition, is_unconditional=0, is_predicated=0, direction=0):
    return (BitWriter()
            .add(address, 13)
            .add(is_unconditional, 1)
            .add(is_predicated, 1)
            .add(0, 17)
            .add(0, 1)
            .add(direction, 1)
            .add(bool_address, 8)
            .add(condition, 1)
            .add(0, 1)   # absoluteAddressing
            .add(OPCODE_COND_JMP, 4)
            .finish())


def pack_instructions(instructions):
    """Packs control flow instructions into the 3 dword groups the hardware uses."""
    assert len(instructions) % 2 == 0, "instructions are stored in pairs"

    data = bytearray()
    for i in range(0, len(instructions), 2):
        first = instructions[i]
        second = instructions[i + 1]

        dword0 = first & 0xFFFFFFFF
        dword1 = ((first >> 32) & 0xFFFF) | ((second & 0xFFFF) << 16)
        dword2 = (second >> 16) & 0xFFFFFFFF
        data += struct.pack(">III", dword0, dword1, dword2)

    return bytes(data)


XCOMPRESS_SIGNATURE = 0x0FF512EE


class LzxBitWriter:
    """Writes bits the way LZX stores them: 16 bit little endian words, MSB first."""

    def __init__(self):
        self.bits = []

    def add(self, value, count):
        for i in range(count - 1, -1, -1):
            self.bits.append((value >> i) & 1)

    def finish(self):
        while len(self.bits) % 16:
            self.bits.append(0)

        data = bytearray()
        for offset in range(0, len(self.bits), 16):
            word = 0
            for bit in self.bits[offset:offset + 16]:
                word = (word << 1) | bit
            data += struct.pack("<H", word)

        return bytes(data)


def lzx_stream(data):
    """Builds an LZX stream holding the data in a single uncompressed block."""
    writer = LzxBitWriter()
    writer.add(0, 1)                  # no intel filesize, shaders are not executables
    writer.add(3, 3)                  # block type 3 is an uncompressed block
    writer.add(len(data) >> 8, 16)    # block length, high bits first
    writer.add(len(data) & 0xFF, 8)
    stream = writer.finish()          # the rest of the current word is padding
    stream += struct.pack("<III", 1, 1, 1)    # stored R0/R1/R2 values
    stream += data

    if len(data) % 2:
        stream += b"\0"

    return stream


def xcompress_container(data, window_size=0x10000, partition_size=0x80000, uncompressed_block_size=None):
    """Builds an Xbox 360 compressed file holding the data in a single block."""
    payload = struct.pack(">H", len(lzx_stream(data))) + lzx_stream(data)
    uncompressed_block_size = uncompressed_block_size or len(data)

    header = struct.pack(">IIIIIIIIIIII",
        XCOMPRESS_SIGNATURE,           # identifier
        0x01030000,                    # version 1.3
        0,                             # reserved
        0,                             # context flags
        window_size,                   # window size
        partition_size,                # compression partition size
        0,                             # uncompressed size, high
        len(data),                     # uncompressed size, low
        0,                             # compressed size, high
        len(payload) + 4,              # compressed size, low
        uncompressed_block_size,       # uncompressed block size
        len(payload))                  # compressed block size, maximum

    return header + struct.pack(">I", len(payload)) + payload


class ConstantTableBuilder:
    """Builds a D3DX style constant table and reports its offsets."""

    def __init__(self):
        self.constants = []
        self.strings = bytearray()
        self.string_offsets = {}

    def string(self, value):
        offset = self.string_offsets.get(value)
        if offset is None:
            offset = len(self.strings) + 1     # keep offset 0 free as a null terminator
            self.string_offsets[value] = offset
            self.strings += value.encode("ascii") + b"\0"
        return offset

    def add_constant(self, name, register_set, register_index, register_count,
                     parameter_class=PARAMETER_CLASS_VECTOR, parameter_type=PARAMETER_TYPE_FLOAT,
                     rows=1, columns=4, elements=0):
        self.constants.append({
            "name": name,
            "register_set": register_set,
            "register_index": register_index,
            "register_count": register_count,
            "parameter_class": parameter_class,
            "parameter_type": parameter_type,
            "rows": rows,
            "columns": columns,
            "elements": elements,
        })

    def build(self):
        """Returns (table bytes, name of the offsets used in the header)."""
        # Layout: [ConstantTable][ConstantInfo array][TypeInfo array][names]
        table_size = 7 * 4
        info_size = len(self.constants) * 20
        type_info_size = len(self.constants) * 16

        names_offset = table_size + info_size + type_info_size

        info_data = bytearray()
        type_data = bytearray()

        for constant in self.constants:
            name_offset = names_offset + self.string(constant["name"]) - 1
            info_data += struct.pack(">IHHHHII",
                                     name_offset,
                                     constant["register_set"],
                                     constant["register_index"],
                                     constant["register_count"],
                                     0,
                                     0,   # typeInfo
                                     0)   # defaultValue

            type_data += struct.pack(">HHHHHH", constant["parameter_class"], constant["parameter_type"],
                                     constant["rows"], constant["columns"], constant["elements"], 0) + struct.pack(">I", 0)

        data = bytearray()
        data += struct.pack(">IIIIIII",
                            4 + len(info_data) + len(type_data) + len(self.strings),  # size
                            0,   # creator
                            0,   # version
                            len(self.constants),
                            table_size,             # constantInfo offset, relative to the table
                            0,   # flags
                            0)   # target
        data += info_data
        data += type_data
        data += self.strings

        return bytes(data)


def vertex_element_value(address, usage, usage_index):
    return (address & 0xFFF) | ((usage & 0xF) << 12) | ((usage_index & 0xF) << 16)


def interpolator_value(usage, usage_index, register):
    return (usage_index & 0xF) | ((usage & 0xF) << 4) | ((register & 0xF) << 8)


def build_shader_container(is_pixel_shader, constant_table, instructions, outputs=1,
                           interpolator_info=0, vertex_elements=(), interpolators=(),
                           definition_table=b"", is_valid_container=True):
    """Creates a shader container with the given constant table and instruction stream."""
    # virtualSize in the container header holds the offset at which the physical area
    # starts, and the total size of a container is virtualSize + physicalSize.
    constant_table_offset = 0x100
    physical_area_offset = 0x400

    # The instruction stream is stored in bytes, and every pair of instructions takes
    # up three big endian dwords.
    instr_size = len(instructions)

    table_data = constant_table.build()
    table_container = struct.pack(">I", 4 + len(table_data)) + table_data

    physical = bytearray()
    physical += b"\0" * 0x40
    code_offset = len(physical)
    physical += instructions
    while len(physical) % 4 != 0:
        physical += b"\0"

    physical_size = len(physical)

    header = bytearray()
    header += struct.pack(">IIIIIIIII",
                          SHADER_CONTAINER_FLAGS_PIXEL if is_pixel_shader else SHADER_CONTAINER_FLAGS_VERTEX,
                          physical_area_offset,
                          physical_size,
                          0,                      # fieldC
                          constant_table_offset,
                          0,                      # definitionTableOffset
                          physical_area_offset,   # shaderOffset
                          0,
                          0)

    if not is_valid_container:
        # Breaks the container magic so that scanners skip this file.
        header[0] ^= 0xFF

    # The shader structure itself.
    if is_pixel_shader:
        shader = struct.pack(">IIIIIIII", code_offset, instr_size, 0, 0xFF, 0, interpolator_info << 5, 0, outputs)
        shader += b"".join(struct.pack(">I", value) for value in interpolators)
    else:
        shader = struct.pack(">IIIIIIIII", code_offset, instr_size, 0, 0, 0, interpolator_info << 5, 0,
                             len(vertex_elements), 0)
        shader += b"".join(struct.pack(">I", value) for value in vertex_elements)
        shader += b"".join(struct.pack(">I", value) for value in interpolators)

    physical[0:len(shader)] = shader

    file_data = bytearray(header) + b"\0" * (constant_table_offset - len(header)) + table_container
    file_data += b"\0" * (physical_area_offset - len(file_data))
    file_data += definition_table
    file_data += physical

    return bytes(file_data)


def make_test_shaders(output_directory):
    os.makedirs(output_directory, exist_ok=True)
    generated = []

    def write(name, data, expectation):
        path = os.path.join(output_directory, name)
        with open(path, "wb") as f:
            f.write(data)
        generated.append(dict(expectation, file=name, size=len(data)))

    #
    # Pixel shader that jumps on a boolean constant register in the upper half of the
    # boolean register file (b129 is the second boolean register of a pixel shader).
    # This is the case that used to produce "use of undeclared identifier 'b129'".
    #
    table = ConstantTableBuilder()
    table.add_constant("g_TestConstant", REGISTER_SET_FLOAT4, 2, 1)
    table.add_constant("g_TestBoolean", REGISTER_SET_BOOL, 129, 1,
                       parameter_type=PARAMETER_TYPE_BOOL, rows=1, columns=1)

    instructions = [
        cond_jmp_instruction(address=1, bool_address=129, condition=1),
        exec_instruction(address=0, count=0, sequence=0, opcode=OPCODE_EXEC_END),
    ]

    psBool129 = build_shader_container(True, table, pack_instructions(instructions), outputs=1)

    write("ps_bool129.bin", psBool129,
          dict(stage="ps", expected_bit=17, expected_register=129))

    # The same shader inside an Xbox 360 compressed file, which is how the shaders of a game
    # are stored on disk. It has to recompile into exactly the same shader.
    write("xc_bool129.bin", xcompress_container(psBool129),
          dict(stage="ps", expected_bit=17, expected_register=129, cache_alias="ps_bool129.bin"))

    #
    # Pixel shader that jumps on a boolean register that is not part of the constant
    # table at all, with an inverted condition.
    #
    table = ConstantTableBuilder()
    table.add_constant("g_TestConstant", REGISTER_SET_FLOAT4, 0, 1)

    instructions = [
        cond_jmp_instruction(address=1, bool_address=130, condition=0),
        exec_instruction(address=0, count=0, sequence=0, opcode=OPCODE_EXEC_END),
    ]

    write("ps_bool130_unreflected.bin",
          build_shader_container(True, table, pack_instructions(instructions), outputs=1),
          dict(stage="ps", expected_bit=18, expected_register=130))

    #
    # Vertex shader that jumps on a lower boolean register.
    #
    table = ConstantTableBuilder()
    table.add_constant("g_TestConstant", REGISTER_SET_FLOAT4, 4, 1)
    table.add_constant("g_TestBoolean", REGISTER_SET_BOOL, 5, 1,
                       parameter_type=PARAMETER_TYPE_BOOL, rows=1, columns=1)

    instructions = [
        cond_jmp_instruction(address=1, bool_address=5, condition=1),
        exec_instruction(address=0, count=0, sequence=0, opcode=OPCODE_EXEC_END),
    ]

    vsBool5 = build_shader_container(False, table, pack_instructions(instructions), outputs=0,
                                     vertex_elements=[vertex_element_value(0, DECL_USAGE_POSITION, 0)])

    write("vs_bool5.bin", vsBool5,
          dict(stage="vs", expected_bit=5, expected_register=5))

    #
    # Pixel shader with an unconditional jump, which forces the recompiler to use the
    # program counter based control flow instead of the flattened one, and with a jump
    # on a boolean register past the range that is packed into the shared constants.
    #
    table = ConstantTableBuilder()
    table.add_constant("g_TestConstant", REGISTER_SET_FLOAT4, 1, 1)

    instructions = [
        cond_jmp_instruction(address=3, bool_address=200, condition=1),
        exec_instruction(address=0, count=0, sequence=0, opcode=OPCODE_EXEC),
        cond_jmp_instruction(address=2, bool_address=0, condition=1, is_unconditional=1, direction=1),
        exec_instruction(address=0, count=0, sequence=0, opcode=OPCODE_EXEC_END),
    ]

    write("ps_bool200_out_of_range.bin",
          build_shader_container(True, table, pack_instructions(instructions), outputs=1,
                                 interpolator_info=1,
                                 interpolators=[interpolator_value(DECL_USAGE_TEXCOORD, 0, 0)]),
          dict(stage="ps", out_of_range_register=200))

    #
    # Pixel shader with a conditional exec, which the recompiler currently translates as
    # an unconditional block and reports as a warning.
    #
    table = ConstantTableBuilder()
    table.add_constant("g_TestConstant", REGISTER_SET_FLOAT4, 1, 1)
    table.add_constant("g_TestBoolean", REGISTER_SET_BOOL, 128, 1,
                       parameter_type=PARAMETER_TYPE_BOOL, rows=1, columns=1)

    instructions = [
        cond_exec_instruction(address=0, count=0, sequence=0, bool_address=128, condition=1, opcode=OPCODE_COND_EXEC),
        exec_instruction(address=0, count=0, sequence=0, opcode=OPCODE_EXEC_END),
    ]

    write("ps_cond_exec.bin",
          build_shader_container(True, table, pack_instructions(instructions), outputs=1),
          dict(stage="ps", expected_warning="conditionally executes"))

    #
    # Boolean constants declared as arrays, which used to only register the first element.
    #
    table = ConstantTableBuilder()
    table.add_constant("g_TestBooleanArray", REGISTER_SET_BOOL, 2, 3,
                       parameter_type=PARAMETER_TYPE_BOOL, rows=1, columns=1, elements=3)

    instructions = [
        cond_jmp_instruction(address=1, bool_address=4, condition=1),
        exec_instruction(address=0, count=0, sequence=0, opcode=OPCODE_EXEC_END),
    ]

    # The array starts at b2, so the jump on b4 hits its last element. Pixel shader
    # booleans live in the upper half of the packed value, which makes b4 bit 20.
    write("ps_bool_array.bin",
          build_shader_container(True, table, pack_instructions(instructions), outputs=1),
          dict(stage="ps", expected_bit=20, expected_register=4))

    with open(os.path.join(output_directory, "expected.json"), "w") as f:
        json.dump(generated, f, indent=2)

    write_archives(output_directory, psBool129, vsBool5)

    return generated


def write_archives(output_directory, ps_shader, vs_shader):
    """Writes the archive layouts that the recompiler has to unpack before it can recompile.

    The parts of an archive are either compressed files of their own or the pieces of a single
    compressed stream that was cut in half, and both layouts are stored here.
    """
    split_directory = os.path.join(output_directory, "xcompress-split")
    joined_directory = os.path.join(output_directory, "xcompress-joined")
    broken_directory = os.path.join(output_directory, "xcompress-broken")

    for directory in (split_directory, joined_directory, broken_directory):
        os.makedirs(directory, exist_ok=True)

    # Every part is a compressed file of its own, one shader per part.
    with open(os.path.join(split_directory, "shader.ar.00"), "wb") as f:
        f.write(xcompress_container(ps_shader))

    with open(os.path.join(split_directory, "shader.ar.01"), "wb") as f:
        f.write(xcompress_container(vs_shader))

    # A single compressed stream that was cut in half, so that only the first part still has
    # the header of the container.
    container = xcompress_container(ps_shader)
    cut = len(container) * 2 // 3

    with open(os.path.join(joined_directory, "shader.ar.00"), "wb") as f:
        f.write(container[:cut])

    with open(os.path.join(joined_directory, "shader.ar.01"), "wb") as f:
        f.write(container[cut:])

    # A valid compressed file next to one whose payload was overwritten, which has to be
    # reported as an archive without shaders instead of taking the recompiler down.
    with open(os.path.join(broken_directory, "good.ar.00"), "wb") as f:
        f.write(xcompress_container(ps_shader))

    # The size of the first block is made far larger than the file, which is what a cut off or
    # corrupted download of an archive looks like.
    broken = bytearray(xcompress_container(vs_shader))
    broken[48:52] = (0x0FFFFFF0).to_bytes(4, "big")

    with open(os.path.join(broken_directory, "broken.ar.00"), "wb") as f:
        f.write(broken)

    return dict(split=split_directory, joined=joined_directory, broken=broken_directory)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    generated = make_test_shaders(sys.argv[1])
    for shader in generated:
        print("{} ({} bytes)".format(shader["file"], shader["size"]))

    return 0


if __name__ == "__main__":
    sys.exit(main())
