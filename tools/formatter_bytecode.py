#!/usr/bin/env python3
# Part of the LLVM Project (lldb/examples/python/formatter_bytecode.py), under the Apache License v2.0
# with LLVM Exceptions. See LICENSE for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# Copied from llvm-project e9c7db29f12a and extended with a Python -> C++
# translator (CppTranslator, `--compile --format cpp`), `--dump-section`, and
# LEB128 lengths and literals in the assembler and disassembler.

"""
Specification, assembler, disassembler, and interpreter
for LLDB dataformatter bytecode.

See https://lldb.llvm.org/resources/formatterbytecode.html for more details.
"""

from __future__ import annotations

# Work around the fact that one of the local files is called
# types.py, which breaks some versions of python.
import os, sys

path = os.path.abspath(os.path.dirname(__file__))
if path in sys.path:
    sys.path.remove(path)

import re
import io
import ast
import enum
import shlex
import textwrap
from copy import copy
from dataclasses import dataclass
from typing import Any, BinaryIO, Optional, Sequence, TextIO, Tuple, Union, cast

BINARY_VERSION = 1

# Types
type_String = 1
type_Int = 2
type_UInt = 3
type_Object = 4
type_Type = 5

# Opcodes
opcode = dict()


def define_opcode(n, mnemonic, name):
    globals()["op_" + name] = n
    if mnemonic:
        opcode[mnemonic] = n
    opcode[n] = mnemonic


define_opcode(1, "dup", "dup")
define_opcode(2, "drop", "drop")
define_opcode(3, "pick", "pick")
define_opcode(4, "over", "over")
define_opcode(5, "swap", "swap")
define_opcode(6, "rot", "rot")

define_opcode(0x10, "{", "begin")
define_opcode(0x11, "if", "if")
define_opcode(0x12, "ifelse", "ifelse")
define_opcode(0x13, "return", "return")

define_opcode(0x20, None, "lit_uint")
define_opcode(0x21, None, "lit_int")
define_opcode(0x22, None, "lit_string")
define_opcode(0x23, None, "lit_selector")
define_opcode(0x24, None, "lit_integer")
define_opcode(0x25, "null", "lit_null")

define_opcode(0x2A, "as_int", "as_int")
define_opcode(0x2B, "as_uint", "as_uint")
define_opcode(0x2C, "is_null", "is_null")

define_opcode(0x30, "+", "plus")
define_opcode(0x31, "-", "minus")
define_opcode(0x32, "*", "mul")
define_opcode(0x33, "/", "div")
define_opcode(0x34, "%", "mod")
define_opcode(0x35, "<<", "shl")
define_opcode(0x36, ">>", "shr")

define_opcode(0x40, "&", "and")
define_opcode(0x41, "|", "or")
define_opcode(0x42, "^", "xor")
define_opcode(0x43, "~", "not")

define_opcode(0x50, "=", "eq")
define_opcode(0x51, "!=", "neq")
define_opcode(0x52, "<", "lt")
define_opcode(0x53, ">", "gt")
define_opcode(0x54, "=<", "le")
define_opcode(0x55, ">=", "ge")

define_opcode(0x60, "call", "call")

define_opcode(0x70, "dict", "dict")
define_opcode(0x71, "dict_set", "dict_set")
define_opcode(0x72, "dict_get", "dict_get")
define_opcode(0x73, "dict_has", "dict_has")

# Function signatures
sig_summary = 0
sig_init = 1
sig_get_num_children = 2
sig_get_child_index = 3
sig_get_child_at_index = 4
sig_get_value = 5
sig_update = 6

SIGNATURES = {
    "summary": sig_summary,
    "init": sig_init,
    "get_num_children": sig_get_num_children,
    "get_child_index": sig_get_child_index,
    "get_child_at_index": sig_get_child_at_index,
    "get_value": sig_get_value,
    "update": sig_update,
}

SIGNATURE_NAMES = "|".join(SIGNATURES.keys())
SIGNATURE_IDS = {v: k for k, v in SIGNATURES.items()}

# Selectors
selector = dict()


def define_selector(n, name):
    globals()["sel_" + name] = n
    selector["@" + name] = n
    selector[n] = "@" + name


define_selector(0, "summary")
define_selector(1, "type_summary")

define_selector(0x10, "get_num_children")
define_selector(0x11, "get_child_at_index")
define_selector(0x12, "get_child_with_name")
define_selector(0x13, "get_child_index")
define_selector(0x15, "get_type")
define_selector(0x14, "get_parent")
define_selector(0x16, "get_template_argument_type")
define_selector(0x17, "cast")
define_selector(0x18, "get_synthetic_value")
define_selector(0x19, "get_non_synthetic_value")
define_selector(0x20, "get_value")
define_selector(0x21, "get_value_as_unsigned")
define_selector(0x22, "get_value_as_signed")
define_selector(0x23, "get_value_as_address")
define_selector(0x24, "clone")
define_selector(0x25, "get_pointee_type")
define_selector(0x26, "get_byte_size")
define_selector(0x27, "create_child_at_offset")

define_selector(0x40, "read_memory_byte")
define_selector(0x41, "read_memory_uint32")
define_selector(0x42, "read_memory_int32")
define_selector(0x43, "read_memory_unsigned")
define_selector(0x44, "read_memory_signed")
define_selector(0x45, "read_memory_address")
define_selector(0x46, "read_memory")

define_selector(0x50, "fmt")
define_selector(0x51, "sprintf")
define_selector(0x52, "strlen")


################################################################################
# Assembler.
################################################################################

_SIGNATURE_LABEL = re.compile(f"@(?:{SIGNATURE_NAMES}):$")


def _tokenize(assembler: str) -> list[str]:
    """Convert string of assembly into tokens."""
    # With one exception, tokens are sequences of non-space characters.
    # The one exception is string literals, which may have spaces.

    # To parse strings, which can contain escaped contents, use a "Friedl
    # unrolled loop". The high level of such a regex is:
    #     open normal* ( special normal* )* close
    # which for string literals is:
    string_literal = r'" [^"\\]* (?: \\. [^"\\]* )* "'

    return re.findall(rf"{string_literal} | \S+", assembler, re.VERBOSE)


def _segment_by_signature(input: list[str]) -> list[Tuple[str, list[str]]]:
    """Segment the input tokens along signature labels."""
    segments = []

    # Loop state
    signature = None
    tokens = []

    for token in input:
        if _SIGNATURE_LABEL.match(token):
            if signature:
                segments.append((signature, tokens))
            signature = token[1:-1]  # strip leading @, trailing :
            tokens = []
        else:
            tokens.append(token)

    if signature:
        segments.append((signature, tokens))

    return segments


@dataclass
class BytecodeSection:
    """Abstraction of the data serialized to __lldbformatters sections."""

    type_name: str
    flags: int
    signatures: list[Tuple[str, bytes]]

    def validate(self):
        seen = set()
        for sig, _ in self.signatures:
            if sig in seen:
                raise ValueError(f"duplicate signature: {sig}")
            seen.add(sig)

    def _to_binary(self) -> bytes:
        bin = bytearray()
        bin.extend(_to_uleb(len(self.type_name)))
        bin.extend(bytes(self.type_name, encoding="utf-8"))
        bin.extend(_to_uleb(self.flags))
        for sig, bc in self.signatures:
            bin.extend(_to_byte(SIGNATURES[sig]))
            bin.extend(_to_uleb(len(bc)))
            bin.extend(bc)

        return bytes(bin)

    def write_binary(self, output: BinaryIO) -> None:
        self.validate()

        bin = self._to_binary()
        output.write(_to_byte(BINARY_VERSION))
        output.write(_to_uleb(len(bin)))
        output.write(self._to_binary())

    def write_source(self, output: TextIO, language: str) -> None:
        if language == "c":
            self.write_c(output)
        elif language == "swift":
            self.write_swift(output)

    class _CBuilder:
        """Helper class for emitting binary data as a C-string literal."""

        entries: list[Tuple[str, str]]

        def __init__(self) -> None:
            self.entries = []

        def emit_byte(self, x: int, comment: str) -> None:
            self.emit_bytes(_to_byte(x), comment)

        def emit_uleb(self, x: int, comment: str) -> None:
            self.emit_bytes(_to_uleb(x), comment)

        def emit_bytes(self, x: bytes, comment: str) -> None:
            # Construct zero pemited hex values with length two.
            string = "".join(f"\\x{b:02x}" for b in x)
            self.emit_string(string, comment)

        def emit_string(self, string: str, comment: str) -> None:
            self.entries.append((f'"{string}"', comment))

    class _SwiftBuilder:
        """Helper class for emitting binary data as a Swift tuple literal."""

        entries: list[Tuple[bytes, str]]

        def __init__(self) -> None:
            self.entries = []

        def emit_byte(self, x: int, comment: str) -> None:
            self.emit_bytes(_to_byte(x), comment)

        def emit_uleb(self, x: int, comment: str) -> None:
            self.emit_bytes(_to_uleb(x), comment)

        def emit_bytes(self, x: bytes, comment: str) -> None:
            self.entries.append((x, comment))

        def emit_string(self, string: str, comment: str) -> None:
            self.emit_bytes(string.encode(), comment)

        @property
        def type_decl(self):
            total_bytes = sum((len(bs) for bs, _ in self.entries))
            element_list = ", ".join(["UInt8"] * total_bytes)
            return f"({element_list})"

    def _build(self, builder) -> None:
        size = len(self._to_binary())
        builder.emit_byte(BINARY_VERSION, "version")
        builder.emit_uleb(size, "remaining record size")
        builder.emit_uleb(len(self.type_name), "type name size")
        builder.emit_string(self.type_name, "type name")
        builder.emit_uleb(self.flags, "flags")
        for sig, bc in self.signatures:
            builder.emit_byte(SIGNATURES[sig], f"sig_{sig}")
            builder.emit_uleb(len(bc), "program size")
            builder.emit_bytes(bc, "program")

    def write_c(self, output: TextIO) -> None:
        self.validate()

        builder = self._CBuilder()
        self._build(builder)

        print(
            textwrap.dedent(
                """
                #ifdef __APPLE__
                #define FORMATTER_SECTION "__DATA_CONST,__lldbformatters"
                #else
                #define FORMATTER_SECTION ".lldbformatters"
                #endif
                """
            ),
            file=output,
        )
        print(
            "__attribute__((used, section(FORMATTER_SECTION)))",
            file=output,
        )
        var_name = re.sub(r"\W", "_", self.type_name)
        print(f"unsigned char _{var_name}_formatter[] =", file=output)
        indent = "    "
        for string, comment in builder.entries:
            print(f"{indent}// {comment}", file=output)
            print(f"{indent}{string}", file=output)
        print(";", file=output)

    def write_swift(self, output: TextIO) -> None:
        self.validate()

        builder = self._SwiftBuilder()
        self._build(builder)

        print(
            textwrap.dedent(
                """\
                #if swift(>=6.3) && !objectFormat(Wasm)
                #if objectFormat(MachO)
                @section("__DATA_CONST,__lldbformatters")
                #else
                @section(".lldbformatters")
                #endif
                @used"""
            ),
            file=output,
        )
        print(
            f"let `{self.type_name} formatter`: {builder.type_decl} = (",
            file=output,
        )
        indent = "    "
        for bs, comment in builder.entries:
            print(f"{indent}// {comment}", file=output)
            byte_list = ", ".join(f"0x{b:02x}" for b in bs)
            print(f"{indent}{byte_list},", file=output)
        print(")", file=output)
        print("#endif", file=output)  # swift(>=6.3)


def assemble_file(type_name: str, input: TextIO) -> BytecodeSection:
    input_tokens = _tokenize(input.read())
    signatures = []
    for sig, tokens in _segment_by_signature(input_tokens):
        if tokens:
            signatures.append((sig, assemble_tokens(tokens)))

    return BytecodeSection(type_name, flags=0, signatures=signatures)


def assemble(assembly: str) -> bytes:
    return assemble_tokens(_tokenize(assembly))


def assemble_tokens(tokens: list[str]) -> bytes:
    """Assemble assembly into bytecode"""
    # This is a stack of all in-flight/unterminated blocks.
    bytecode = [bytearray()]

    def emit(byte):
        bytecode[-1].append(byte)

    tokens.reverse()
    while tokens:
        tok = tokens.pop()
        if tok == "":
            pass
        elif tok == "{":
            bytecode.append(bytearray())
        elif tok == "}":
            block = bytecode.pop()
            emit(op_begin)
            bytecode[-1].extend(_to_uleb(len(block)))
            bytecode[-1].extend(block)
        elif tok[0].isdigit():
            if tok[-1] == "u":
                emit(op_lit_uint)
                bytecode[-1].extend(_to_uleb(int(tok[:-1])))
            else:
                # With the introduction of op_lit_integer, op_lit_int is no
                # longer emitted by the assembler.
                emit(op_lit_integer)
                bytecode[-1].extend(_to_sleb(int(tok)))
        elif tok[0] == "@":
            emit(op_lit_selector)
            emit(selector[tok])
        elif tok[0] == '"':
            # Remove backslash escaping '"' and '\'.
            s = re.sub(r'\\(["\\])', r"\1", tok[1:-1]).encode()
            emit(op_lit_string)
            bytecode[-1].extend(_to_uleb(len(s)))
            bytecode[-1].extend(s)
        else:
            emit(opcode[tok])
    assert len(bytecode) == 1  # unterminated {
    return bytes(bytecode[0])


################################################################################
# Disassembler.
################################################################################


def disassemble_file(input: BinaryIO, output: TextIO) -> None:
    stream = io.BytesIO(input.read())

    version = stream.read(1)[0]
    if version != BINARY_VERSION:
        raise ValueError(f"unknown binary version: {version}")

    record_size = _from_uleb(stream)
    stream.truncate(stream.tell() + record_size)

    name_size = _from_uleb(stream)
    _type_name = stream.read(name_size).decode()
    _flags = _from_uleb(stream)

    while True:
        sig_byte = stream.read(1)
        if not sig_byte:
            break
        sig_name = SIGNATURE_IDS[sig_byte[0]]
        body_size = _from_uleb(stream)
        bc = stream.read(body_size)
        asm, _ = disassemble(bc)
        print(f"@{sig_name}: {asm}", file=output)


def disassemble(bytecode: bytes) -> Tuple[str, list[int]]:
    """Disassemble bytecode into (assembly, token starts)"""
    asm = ""
    all_bytes = list(bytecode)
    all_bytes.reverse()
    blocks = []
    tokens = [0]

    def next_byte():
        """Fetch the next byte in the bytecode and keep track of all
        in-flight blocks"""
        for i in range(len(blocks)):
            blocks[i] -= 1
        tokens.append(len(asm))
        return all_bytes.pop()

    def next_uleb():
        result = shift = 0
        while True:
            b = next_byte()
            result |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return result

    def next_sleb():
        result = shift = 0
        while True:
            b = next_byte()
            result |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return result - (1 << shift) if b & 0x40 else result

    while all_bytes:
        b = next_byte()
        if b == op_begin:
            asm += "{"
            length = next_uleb()
            blocks.append(length)
        elif b == op_lit_uint:
            b = next_uleb()
            asm += str(b)
            asm += "u"
        elif b == op_lit_int:
            b = next_sleb()
            asm += str(b)
        elif b == op_lit_integer:
            b = next_sleb()
            asm += str(b)
        elif b == op_lit_selector:
            b = next_byte()
            asm += selector[b]
        elif b == op_lit_string:
            length = next_uleb()
            s = '"'
            for _ in range(length):
                c = chr(next_byte())
                if c in ('"', "\\"):
                    s += "\\"
                s += c
            s += '"'
            asm += s
        else:
            asm += opcode[b]

        while blocks and blocks[-1] == 0:
            asm += " }"
            blocks.pop()

        if all_bytes:
            asm += " "

    if blocks:
        asm += "ERROR"
    return asm, tokens



def disassemble_pretty(bytecode: bytes, indent: int = 1) -> str:
    """Disassemble bytecode with each block on its own indented lines:

        2u pick 0u = {
            1
        } {
            2
        } ifelse
    """
    out: list[str] = []
    line = ""
    depth = indent
    blocks: list[int] = []
    stream = io.BytesIO(bytecode)

    def flush() -> None:
        nonlocal line
        if line.strip():
            out.append(line)
        line = ""

    def token(text: str) -> None:
        nonlocal line
        line += ("" if not line.strip() else " ") if line.strip() else "    " * depth
        line += text

    def close_blocks() -> None:
        nonlocal depth, line
        while blocks and blocks[-1] <= stream.tell():
            blocks.pop()
            depth -= 1
            flush()
            line = "    " * depth + "}"

    while stream.tell() < len(bytecode):
        b = stream.read(1)[0]
        if b == op_begin:
            length = _from_uleb(stream)
            token("{")
            flush()
            depth += 1
            blocks.append(stream.tell() + length)
        elif b == op_lit_uint:
            token(str(_from_uleb(stream)) + "u")
        elif b in (op_lit_int, op_lit_integer):
            token(str(_from_sleb(stream)))
        elif b == op_lit_selector:
            token(selector.get(stream.read(1)[0], "@?"))
        elif b == op_lit_string:
            text = stream.read(_from_uleb(stream)).decode(errors="replace")
            token('"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"')
        else:
            token(opcode.get(b) or f"<opcode {b:#x}>")
        close_blocks()
    flush()
    return "\n".join(out)


def read_records(data: bytes):
    """Yield (type name, flags, [(signature name, bytecode)]) for each record
    in the contents of a formatter section."""
    stream = io.BytesIO(data)
    while stream.tell() < len(data):
        if data[stream.tell()] == 0:  # Padding between records.
            stream.read(1)
            continue
        version = _from_uleb(stream)
        size = _from_uleb(stream)
        end = stream.tell() + size
        if version != BINARY_VERSION:
            stream.seek(end)
            continue
        name = stream.read(_from_uleb(stream)).decode(errors="replace")
        flags = _from_uleb(stream)
        programs = []
        while stream.tell() < end:
            signature = stream.read(1)[0]
            programs.append(
                (SIGNATURE_IDS.get(signature, f"?{signature}"), stream.read(_from_uleb(stream)))
            )
        yield name, flags, programs


def find_formatter_section(path: str) -> bytes:
    """The bytes of the __lldbformatters / .lldbformatters section of a Mach-O
    (thin or universal, the first architecture) or ELF file, or a .o file."""
    import struct

    with open(path, "rb") as f:
        data = f.read()
    wanted = (b"__lldbformatters", b".lldbformatters", b".lldbfor")
    magic = data[:4]
    if magic in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"):  # Mach-O 64/32, little endian
        return _macho_section(data, 0, wanted)
    if magic in (b"\xca\xfe\xba\xbe",):  # Universal binary: use the first slice.
        offset = struct.unpack(">I", data[16:20])[0]
        return _macho_section(data, offset, wanted)
    if magic == b"\x7fELF":
        return _elf_section(data, wanted)
    raise ValueError(f"{path}: not a Mach-O or ELF file")


def _macho_section(data: bytes, base: int, wanted) -> bytes:
    import struct

    ncmds = struct.unpack_from("<I", data, base + 16)[0]
    position = base + 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, position)
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack_from("<I", data, position + 64)[0]
            section = position + 72
            for _ in range(nsects):
                sectname = data[section : section + 16].rstrip(b"\0")
                size, offset = struct.unpack_from("<QI", data, section + 40)
                if sectname in wanted:
                    return data[base + offset : base + offset + size] if base else data[offset : offset + size]
                section += 80
        position += cmdsize
    raise ValueError("no formatter section found")


def _elf_section(data: bytes, wanted) -> bytes:
    import struct

    if data[4] != 2 or data[5] != 1:
        raise ValueError("only little-endian ELF64 is supported")
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x3A)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i * shentsize) for i in range(shnum)]
    strtab = sections[shstrndx]
    for name, _, _, _, offset, size, *_ in sections:
        end = data.index(b"\0", strtab[4] + name)
        if data[strtab[4] + name : end] in wanted:
            return data[offset : offset + size]
    raise ValueError("no formatter section found")


def dump_section(path: str, output: TextIO) -> None:
    for name, flags, programs in read_records(find_formatter_section(path)):
        print(f'record "{name}" (flags {flags})', file=output)
        for signature, bytecode in programs:
            print(f"  @{signature} ({len(bytecode)} bytes):", file=output)
            print(disassemble_pretty(bytecode, 2), file=output)
        print(file=output)


################################################################################
# Interpreter.
################################################################################


def count_fmt_params(fmt: str) -> int:
    """Count the number of parameters in a format string"""
    from string import Formatter

    f = Formatter()
    n = 0
    for _, name, _, _ in f.parse(fmt):
        if name > n:
            n = name
    return n


def interpret(bytecode: bytes, control: list, data: list, tracing: bool = False):
    """Interpret bytecode"""
    frame = []
    frame.append((0, len(bytecode)))

    def trace():
        """print a trace of the execution for debugging purposes"""

        def fmt(d):
            if isinstance(d, int):
                return str(d)
            if isinstance(d, str):
                return d
            return repr(type(d))

        pc, end = frame[-1]
        asm, tokens = disassemble(bytecode)
        print(
            "=== frame = {1}, data = {2}, opcode = {0}".format(
                opcode[b], frame, [fmt(d) for d in data]
            )
        )
        print(asm)
        print(" " * (tokens[pc]) + "^")

    def next_byte():
        """Fetch the next byte and update the PC"""
        pc, end = frame[-1]
        assert pc < len(bytecode)
        b = bytecode[pc]
        frame[-1] = pc + 1, end
        # At the end of a block?
        while pc >= end:
            frame.pop()
            if not frame:
                return None
            pc, end = frame[-1]
            if pc >= end:
                return None
            b = bytecode[pc]
            frame[-1] = pc + 1, end
        return b

    while frame[-1][0] < len(bytecode):
        b = next_byte()
        if b == None:
            break
        if tracing:
            trace()
        # Data stack manipulation.
        if b == op_dup:
            data.append(data[-1])
        elif b == op_drop:
            data.pop()
        elif b == op_pick:
            data.append(data[data.pop()])
        elif b == op_over:
            data.append(data[-2])
        elif b == op_swap:
            x = data.pop()
            y = data.pop()
            data.append(x)
            data.append(y)
        elif b == op_rot:
            z = data.pop()
            y = data.pop()
            x = data.pop()
            data.append(z)
            data.append(x)
            data.append(y)

        # Control stack manipulation.
        elif b == op_begin:
            length = next_byte()
            pc, end = frame[-1]
            control.append((pc, pc + length))
            frame[-1] = pc + length, end
        elif b == op_if:
            if data.pop():
                frame.append(control.pop())
        elif b == op_ifelse:
            if data.pop():
                control.pop()
                frame.append(control.pop())
            else:
                frame.append(control.pop())
                control.pop()
        elif b == op_return:
            control.clear()
            return data[-1]

        # Literals.
        elif b == op_lit_uint:
            b = next_byte()  # FIXME uleb
            data.append(int(b))
        elif b == op_lit_int:
            b = next_byte()  # FIXME uleb
            data.append(int(b))
        elif b == op_lit_integer:
            b = next_byte()  # FIXME sleb
            data.append(int(b))
        elif b == op_lit_selector:
            b = next_byte()
            data.append(b)
        elif b == op_lit_string:
            length = next_byte()
            s = ""
            while length:
                s += chr(next_byte())
                length -= 1
            data.append(s)
        elif b == op_lit_null:
            data.append(None)

        elif b == op_as_uint:
            pass
        elif b == op_as_int:
            pass
        elif b == op_is_null:
            data.append(1 if data.pop() == None else 0)

        # Arithmetic, logic, etc.
        elif b == op_plus:
            data.append(data.pop() + data.pop())
        elif b == op_minus:
            data.append(-data.pop() + data.pop())
        elif b == op_mul:
            data.append(data.pop() * data.pop())
        elif b == op_div:
            y = data.pop()
            data.append(data.pop() / y)
        elif b == op_mod:
            y = data.pop()
            data.append(data.pop() % y)
        elif b == op_shl:
            y = data.pop()
            data.append(data.pop() << y)
        elif b == op_shr:
            y = data.pop()
            data.append(data.pop() >> y)
        elif b == op_and:
            data.append(data.pop() & data.pop())
        elif b == op_or:
            data.append(data.pop() | data.pop())
        elif b == op_xor:
            data.append(data.pop() ^ data.pop())
        elif b == op_not:
            data.append(not data.pop())
        elif b == op_eq:
            data.append(data.pop() == data.pop())
        elif b == op_neq:
            data.append(data.pop() != data.pop())
        elif b == op_lt:
            data.append(data.pop() > data.pop())
        elif b == op_gt:
            data.append(data.pop() < data.pop())
        elif b == op_le:
            data.append(data.pop() >= data.pop())
        elif b == op_ge:
            data.append(data.pop() <= data.pop())

        # Function calls.
        elif b == op_call:
            sel = data.pop()
            if sel == sel_summary:
                data.append(data.pop().GetSummary())
            elif sel == sel_get_num_children:
                data.append(data.pop().GetNumChildren())
            elif sel == sel_get_child_at_index:
                index = data.pop()
                valobj = data.pop()
                data.append(valobj.GetChildAtIndex(index))
            elif sel == sel_get_child_with_name:
                name = data.pop()
                valobj = data.pop()
                data.append(valobj.GetChildMemberWithName(name))
            elif sel == sel_get_child_index:
                name = data.pop()
                valobj = data.pop()
                data.append(valobj.GetIndexOfChildWithName(name))
            elif sel == sel_get_type:
                data.append(data.pop().GetType())
            elif sel == sel_get_parent:
                valobj = data.pop()
                data.append(valobj.GetParent())
            elif sel == sel_get_template_argument_type:
                n = data.pop()
                valobj = data.pop()
                data.append(valobj.GetTemplateArgumentType(n))
            elif sel == sel_get_synthetic_value:
                data.append(data.pop().GetSyntheticValue())
            elif sel == sel_get_non_synthetic_value:
                data.append(data.pop().GetNonSyntheticValue())
            elif sel == sel_get_value:
                data.append(data.pop().GetValue())
            elif sel == sel_get_value_as_unsigned:
                data.append(data.pop().GetValueAsUnsigned())
            elif sel == sel_get_value_as_signed:
                data.append(data.pop().GetValueAsSigned())
            elif sel == sel_get_value_as_address:
                data.append(data.pop().GetValueAsAddress())
            elif sel == sel_cast:
                sbtype = data.pop()
                valobj = data.pop()
                data.append(valobj.Cast(sbtype))
            elif sel == sel_clone:
                new_name = data.pop()
                valobj = data.pop()
                data.append(valobj.Clone(new_name))
            elif sel == sel_get_pointee_type:
                sbtype = data.pop()
                data.append(sbtype.GetPointeeType())
            elif sel == sel_get_byte_size:
                sbtype = data.pop()
                data.append(sbtype.GetByteSize())
            elif sel == sel_create_child_at_offset:
                sbtype = data.pop()
                offset = data.pop()
                name = data.pop()
                valobj = data.pop()
                data.append(valobj.CreateChildAtOffset(name, offset, sbtype))
            elif sel == sel_strlen:
                s = data.pop()
                data.append(len(s) if s else 0)
            elif sel == sel_fmt:
                fmt = data.pop()
                n = count_fmt_params(fmt)
                args = []
                for i in range(n):
                    args.append(data.pop())
                data.append(fmt.format(*args))
            else:
                print("not implemented: " + selector[sel])
                assert False

        # Dictionary operations.
        elif b == op_dict:
            data.append(dict())
        elif b == op_dict_set:
            value = data.pop()
            key = data.pop()
            d = data.pop()
            d[key] = value
            data.append(d)
        elif b == op_dict_get:
            key = data.pop()
            d = data.pop()
            data.append(d[key])
        elif b == op_dict_has:
            key = data.pop()
            d = data.pop()
            data.append(int(key in d))
    return data[-1]


################################################################################
# Python -> Bytecode Compiler
################################################################################

_BUILTINS = {
    "Cast": "@cast",
    "Clone": "@clone",
    "CreateChildAtOffset": "@create_child_at_offset",
    "GetByteSize": "@get_byte_size",
    "GetChildAtIndex": "@get_child_at_index",
    "GetChildMemberWithName": "@get_child_with_name",
    "GetIndexOfChildWithName": "@get_child_index",
    "GetNonSyntheticValue": "@get_non_synthetic_value",
    "GetNumChildren": "@get_num_children",
    "GetParent": "@get_parent",
    "GetPointeeType": "@get_pointee_type",
    "GetSummary": "@summary",
    "GetSyntheticValue": "@get_synthetic_value",
    "GetTemplateArgumentType": "@get_template_argument_type",
    "GetType": "@get_type",
    "GetValue": "@get_value",
    "GetValueAsAddress": "@get_value_as_address",
    "GetValueAsSigned": "@get_value_as_signed",
    "GetValueAsUnsigned": "@get_value_as_unsigned",
}

_COMPS = {
    ast.Eq: "=",
    ast.NotEq: "!=",
    ast.Lt: "<",
    ast.LtE: "=<",
    ast.Gt: ">",
    ast.GtE: ">=",
}

# Maps Python method names in a formatter class to their bytecode signatures.
_METHOD_SIGS = {
    "__init__": "@init",
    "update": "@update",
    "num_children": "@get_num_children",
    "get_child_index": "@get_child_index",
    "get_child_at_index": "@get_child_at_index",
    "get_value": "@get_value",
}


class CompilerError(Exception):
    lineno: int

    def __init__(self, message, node: Union[ast.expr, ast.stmt]) -> None:
        super().__init__(message)
        self.lineno = node.lineno


class Compiler(ast.NodeVisitor):
    """
    Compile Python LLDB data formatters to LLDB formatter bytecode.

    This compiler is supports a limited subset of Python.

    # Supported Features

    * Top level functions implementing LLDB summary formatters
    * Top level classes implementing LLDB synthetic formatters
    * Partial support for the following, see below for more details:
      - Object attributes (properties)
      - Local variables
      - Function calls
    * Python language support
    [x] If statements (including else, elif and nested if)
    [x] Return statements
    [x] String, integer, float, boolean, and None literals
    [x] Binary comparisons
    [ ] Boolean operators
    [ ] Math operations

    # Unsupported Features

        Note: that this is not exhaustive, refer to the list of supported
        features above.

    * For and while loops
    * Exceptions
    * User defined general purpose functions and classes
    * Lists, dicts, sets, and other container data types
    * Iterators, comprehensions, yield, etc
    * With statements
    * Imports of any modules

    # Variables

    The compiler supports two kinds of variables, local variables and attribute
    variables (properties), but there are limitations on both.

    In __init__ and update, local variables are currently *not* supported, but
    attributes can be assigned to. This matches the common case for these
    functions.

    In all other function bodies, local variables _are_ supported, but
    attributes can only be read from, *not* assigned to. This also matches the
    common case for these functions.

    Variables (local and attributes) are tracked, allowing the compiler to know
    their position in the stack. Variable reads can then be lowered to `pick`
    instructions. See the compiler's `locals` and `attrs` attributes.

    # Functions

    Known functions are supported, a design that customizes the scope of what
    formatters can and can't do. The functions known to the compiler are called
    "selectors". The selectors are primarily SBValue API, although there are
    also general purpose selectors. Formatters can only call selectors, not user
    defined functions, and not SB methods that have not been defined as a
    selector.
    """

    # Names of locals in bottom-to-top stack order. locals[0] is the
    # oldest/deepest; locals[-1] is the most recently pushed.
    locals: list[str]

    # Names of visible attrs in bottom-to-top stack order. Always holds the
    # full combined frame for the method being compiled: grows incrementally
    # during __init__/update, and is set to the combined list before getter
    # methods are compiled.
    attrs: list[str]

    # Bytecode signature of the method being compiled, or None for top-level
    # functions.
    current_sig: Optional[str]

    buffer: io.StringIO

    def __init__(self) -> None:
        self.locals = []
        self.attrs = []
        self.current_sig = None
        self.buffer = io.StringIO()

    def compile(self, source_file: str) -> str:
        with open(source_file) as f:
            root = ast.parse(f.read())
        self.visit(root)
        return self.buffer.getvalue()

    def visit_ClassDef(self, node: ast.ClassDef) -> None:
        # Compile methods in a fixed order so that attrs is fully populated
        # before getter methods are compiled.
        methods = {}
        for item in node.body:
            if isinstance(item, ast.FunctionDef):
                if item.name not in _METHOD_SIGS:
                    raise CompilerError(f"unsupported method: {item.name}", item)
                methods[item.name] = item

        self.attrs = []
        if method := methods.get("__init__"):
            self._compile_method(method)
        # self.attrs now holds init's attrs. update's attrs are appended above
        # them, so after update self.attrs is the combined init+update list.
        if method := methods.get("update"):
            self._compile_method(method)

        for method_name, method in methods.items():
            if method_name not in ("__init__", "update"):
                self._compile_method(method)

    def _compile_method(self, node: ast.FunctionDef) -> None:
        self.current_sig = _METHOD_SIGS[node.name]

        return_type = node.returns.id if isinstance(node.returns, ast.Name) else None
        if node.name == "update" and return_type != "bool":
            raise CompilerError(
                "update must be declared to return bool: def update(self) -> bool:",
                node,
            )

        # Strip 'self' (and 'internal_dict' for __init__) from the arg list;
        # the remaining args become the initial locals.
        args = copy(node.args.args)
        args.pop(0)  # drop 'self'
        if node.name == "__init__":
            args.pop()  # drop trailing 'internal_dict'

        self.locals = [arg.arg for arg in args]

        # Compile into a temporary buffer so the signature line can be
        # emitted first.
        saved_buffer = self.buffer
        self.buffer = io.StringIO()

        self._visit_each(node.body)

        method_output = self.buffer.getvalue()
        self.buffer = saved_buffer
        self._output(f"{self.current_sig}:")
        self._output(method_output)

        self.locals.clear()
        self.current_sig = None

    def visit_FunctionDef(self, node: ast.FunctionDef) -> None:
        # Top-level function (not inside a class).
        self.current_sig = None
        self.attrs = []
        self.locals = [arg.arg for arg in node.args.args]
        self._visit_each(node.body)
        self.locals.clear()

    def visit_Compare(self, node: ast.Compare) -> None:
        self.visit(node.left)
        # XXX: Does not handle multiple comparisons, ex: `0 < x < 10`
        self.visit(node.comparators[0])
        self._output(_COMPS[type(node.ops[0])])

    def visit_If(self, node: ast.If) -> None:
        self.visit(node.test)

        self._output("{")
        self._visit_each(node.body)
        if node.orelse:
            self._output("} {")
            self._visit_each(node.orelse)
            self._output("} ifelse")
        else:
            self._output("} if")

    def visit_Return(self, node: ast.Return) -> None:
        if node.value:
            self.visit(node.value)
        self._output("return")

    def visit_Constant(self, node: ast.Constant) -> None:
        if isinstance(node.value, str):
            self._output(f'"{node.value}"')
        elif isinstance(node.value, bool):
            self._output(int(node.value))
        elif node.value is None:
            self._output("null")
        else:
            self._output(node.value)

    def visit_Call(self, node: ast.Call) -> None:
        func = node.func
        if isinstance(func, ast.Attribute):
            receiver = func.value
            method = func.attr
            # self is not a valid call receiver.
            if isinstance(receiver, ast.Name) and receiver.id == "self":
                raise CompilerError(
                    "self is not a valid call receiver; use self.attr to read an attribute",
                    node,
                )
            if selector := _BUILTINS.get(method):
                self.visit(receiver)
                self._visit_each(node.args)
                self._output(f"{selector} call")
                return
            raise CompilerError(f"unsupported method: {method}", node)

        if isinstance(func, ast.Name):
            raise CompilerError(f"unsupported function: {func.id}", node)

        raise CompilerError("unsupported function call expression", node)

    def visit_Assign(self, node: ast.Assign) -> None:
        target = node.targets[0]

        # Handle self.attr = expr (attribute assignment).
        if (
            isinstance(target, ast.Attribute)
            and isinstance(target.value, ast.Name)
            and target.value.id == "self"
        ):
            if self.current_sig not in ("@init", "@update"):
                raise CompilerError(
                    "attribute assignment is only allowed in __init__ and update",
                    node,
                )

            attr = target.attr
            if attr in self.attrs:
                raise CompilerError(f"attribute '{attr}' is already assigned", node)

            # If the RHS is an argument (the only kind of local permitted in
            # __init__) - then it is already on the stack in place, and no
            # evaluation is needed.
            is_arg = (
                isinstance(node.value, ast.Name)
                and self._local_index(node.value) is not None
            )
            if not is_arg:
                # Evaluate the RHS, leaving its value on the stack.
                self.visit(node.value)

            # Record the attr.
            self.attrs.append(attr)
            return

        # Handle local variable assignment.
        if self.current_sig in ("@init", "@update"):
            raise CompilerError(
                "local variable assignment is not allowed in __init__ or update; "
                "use attribute assignment (self.attr = ...) instead",
                node,
            )

        if isinstance(target, ast.Name):
            names = [target]
        elif isinstance(target, ast.Tuple):
            names = cast(list[ast.Name], target.elts)
        else:
            raise CompilerError("unsupported assignment target", node)

        # Visit RHS, leaving its value on the stack.
        self.visit(node.value)

        # Forget any previous bindings of these names.
        # Their values are orphaned on the stack.
        for name in names:
            idx = self._local_index(name)
            if idx is not None:
                self.locals[idx] = ""

        self.locals.extend(x.id for x in names)

    def visit_Attribute(self, node: ast.Attribute) -> None:
        # Only self.attr reads are supported here.
        if not (isinstance(node.value, ast.Name) and node.value.id == "self"):
            raise CompilerError(
                "unsupported attribute access (only self.attr is supported)", node
            )
        pick_idx = self._attr_index(node.attr, node)
        self._output(f"{pick_idx} pick")  # "# self.{node.attr}"

    def visit_Name(self, node: ast.Name) -> None:
        idx = self._local_index(node)
        if idx is None:
            raise CompilerError(f"unknown local variable: {node.id}", node)
        self._output(f"{idx} pick")  # "# {node.id}"

    def _visit_each(self, nodes: Sequence[ast.AST]) -> None:
        for child in nodes:
            self.visit(child)

    def _attr_index(self, name: str, node: ast.expr) -> int:
        # self.attrs is always the full visible attr frame, so the index is
        # the direct pick offset with no further adjustment.
        try:
            return self.attrs.index(name)
        except ValueError:
            raise CompilerError(f"unknown attribute: {name}", node)

    def _local_index(self, name: ast.Name) -> Optional[int]:
        try:
            idx = self.locals.index(name.id)
            # Offset past all attrs.
            return len(self.attrs) + idx
        except ValueError:
            return None

    def _output(self, x: Any) -> None:
        print(x, file=self.buffer)



################################################################################
# Python -> C++ DSL translator
################################################################################
#
# Translates Python LLDB data formatters into the Python-shaped C++ DSL of
# lldbfmt/script.h, so that existing formatters can be pasted into a C++
# project and compiled into the binary.
#
# What is translated:
#
#   * top-level summary functions: def Foo(valobj, dict): ...
#   * synthetic provider classes with __init__, update, num_children,
#     get_child_index, get_child_at_index and get_value.
#   * which type each formatter is for, read from the `type summary add` and
#     `type synthetic add` commands in __lldb_init_module (or --type-name).
#
# Python constructs:
#
#   * SBValue and SBType calls, and the properties num_children, summary,
#     value and type.
#   * if/elif/else, return, assignments and augmented assignments.
#   * Arithmetic, bitwise and comparison operators, and/or/not, `is None`, and
#     `a if c else b`.
#   * String concatenation, str(), f-strings and `"..." % args` (as format).
#   * Comparing one value to string constants in an if/elif chain (as lookup).
#
# Anything else (loops, other calls, helper classes, ...) is reported on
# stderr with a line number and left in the output as FIXME_UNSUPPORTED, which
# does not compile, so it cannot be missed.

_CPP_KEYWORDS = frozenset(
    """alignas alignof and and_eq asm auto bitand bitor bool break case catch char
    class compl concept const consteval constexpr constinit const_cast continue
    decltype default delete do double dynamic_cast else enum explicit export
    extern false float for friend goto if inline int long mutable namespace new
    noexcept not not_eq nullptr operator or or_eq private protected public
    register reinterpret_cast requires return short signed sizeof static
    static_assert static_cast struct switch template this thread_local throw true
    try typedef typeid typename union unsigned using virtual void volatile
    wchar_t while xor xor_eq""".split()
)

# Names that the DSL puts in scope and a local must not shadow.
_DSL_NAMES = frozenset(
    "format if_ lookup null state state_reuse init update num_children child_index "
    "child_at_index value".split()
)

_SB_METHODS = frozenset(
    {
        "Cast", "Clone", "CreateChildAtOffset", "GetByteSize", "GetChildAtIndex",
        "GetChildMemberWithName", "GetIndexOfChildWithName", "GetNonSyntheticValue",
        "GetNumChildren", "GetParent", "GetPointeeType", "GetSummary",
        "GetSyntheticValue", "GetTemplateArgumentType", "GetType", "GetValue",
        "GetValueAsAddress", "GetValueAsSigned", "GetValueAsUnsigned", "IsValid",
    }
)  # fmt: skip

# SBValue properties, as Python spells them.
_SB_PROPERTIES = {
    "num_children": "get_num_children",
    "summary": "get_summary",
    "value": "get_value",
    "type": "get_type",
    "parent": "get_parent",
    "synthetic_value": "get_synthetic_value",
    "pointee_type": "get_pointee_type",
}

# SBValue methods whose Python form takes a defaulting argument the bytecode
# cannot honor (an unreadable value is an error here, not the default).
_DEFAULTING_METHODS = frozenset({"GetValueAsUnsigned", "GetValueAsSigned"})

_BINOPS = {
    ast.Add: "+", ast.Sub: "-", ast.Mult: "*", ast.Div: "/", ast.FloorDiv: "/",
    ast.Mod: "%", ast.BitAnd: "&", ast.BitOr: "|", ast.BitXor: "^",
    ast.LShift: "<<", ast.RShift: ">>",
}  # fmt: skip

_CMPOPS = {
    ast.Eq: "==", ast.NotEq: "!=", ast.Lt: "<", ast.LtE: "<=", ast.Gt: ">", ast.GtE: ">=",
}  # fmt: skip


def _snake(name: str) -> str:
    """LLDB's Python spelling (GetValueAsUnsigned) as the C++ one (get_value_as_unsigned)."""
    return re.sub(r"(?<=[a-z0-9])([A-Z])", r"_\1", name).lower()


class _Chain:
    """A conditional expression: if/elif/.../else, flattened when printed."""

    def __init__(self, cond: str, then: Any, otherwise: Any) -> None:
        self.cond = cond
        self.then = then
        self.otherwise = otherwise


# Stands for an argument (LLDB's internal dict) that formatters do not use. If
# one does, this text lands in the output and keeps it from compiling.
_UNUSED_ARGUMENT = "FIXME_UNSUPPORTED /* internal_dict is not available */"


class _Instance:
    """A helper-class object whose methods are inlined where they are called."""

    def __init__(self, cls: ast.ClassDef, attributes: dict[str, Any]) -> None:
        self.cls = cls
        self.attributes = attributes  # "self.x" -> C++ text


def _cpp_string(value: str) -> str:
    out = ['"']
    for ch in value:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif ord(ch) < 32 or ord(ch) == 127:
            # A hex escape swallows following hex digits, so end the literal.
            out.append('\\x%02x""' % ord(ch))
        else:
            out.append(ch)
    out.append('"')
    return "".join(out)


def _strip_parens(text: str) -> str:
    """Remove one pair of redundant outer parentheses."""
    if not (text.startswith("(") and text.endswith(")")):
        return text
    depth = 0
    in_string = False
    i = 0
    while i < len(text):
        c = text[i]
        if in_string:
            if c == "\\":
                i += 1
            elif c == '"':
                in_string = False
        elif c == '"':
            in_string = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0 and i != len(text) - 1:
                return text
        i += 1
    return text[1:-1]


class CppTranslator:
    """Translate the formatters in a Python source file to the C++ DSL."""

    def __init__(
        self,
        source: str,
        filename: str = "<input>",
        type_name: Optional[str] = None,
        quote_source: bool = True,
    ) -> None:
        self.source = source
        self.lines = source.splitlines()
        self.filename = filename
        self.default_type_name = type_name
        self.quote_source = quote_source
        self.warnings: list[str] = []  # Output contains FIXME_UNSUPPORTED.
        self.notes: list[str] = []  # Output is complete but differs slightly.
        self.registrations: dict[str, str] = {}
        # lldb::TypeOptions bits from the registration's flags. Cascading unless
        # the command said `-C no`, as `type summary add` does.
        self.type_options: dict[str, int] = {}
        # Per-function state.
        self._used: set[str] = set()
        self._counter: dict[str, int] = {}
        self._referenced: set[str] = set()
        self._index_method = False
        # Helper classes and functions in the file are inlined where called.
        self.classes: dict[str, ast.ClassDef] = {}
        self.functions: dict[str, ast.FunctionDef] = {}
        self._sink: list[str] = []
        self._class_stack: list[ast.ClassDef] = []
        self._depth = 0

    # Diagnostics.

    def _warn(self, node: Optional[ast.AST], message: str) -> str:
        line = getattr(node, "lineno", 0)
        text = f"{self.filename}:{line}: {message}"
        if text not in self.warnings:
            self.warnings.append(text)
        return f"FIXME_UNSUPPORTED /* {message.replace('*/', '* /')} */"

    # Registrations.

    def _read_registrations(self, tree: ast.Module) -> None:
        """Learn which type each formatter is for from HandleCommand strings."""
        for node in ast.walk(tree):
            if not (
                isinstance(node, ast.Call)
                and isinstance(node.func, ast.Attribute)
                and node.func.attr == "HandleCommand"
                and node.args
            ):
                continue
            command = self._constant_string(node.args[0])
            if command is None:
                continue
            try:
                words = shlex.split(command)
            except ValueError:
                continue
            if words[:3] not in (["type", "summary", "add"], ["type", "synthetic", "add"]):
                continue
            target = None
            regex = False
            options = 1  # cascade
            positional = []
            i = 3
            while i < len(words):
                word = words[i]
                if word in ("-F", "--python-function", "-l", "--python-class"):
                    i += 1
                    target = words[i].split(".")[-1] if i < len(words) else None
                elif word in ("-x", "--regex"):
                    regex = True
                elif word in ("-C", "--cascade"):
                    i += 1
                    if i < len(words) and words[i].lower() in ("false", "no", "0"):
                        options &= ~1
                elif word in ("-p", "--skip-pointers"):
                    options |= 1 << 1
                elif word in ("-r", "--skip-references"):
                    options |= 1 << 2
                elif word in ("-c", "--inline-children"):
                    options |= 1 << 5
                elif word in ("-v", "--no-value"):
                    options |= 1 << 4
                elif word in ("-w", "--category", "-n", "--name", "-s", "--summary-string", "-t", "--type"):
                    i += 1
                elif word.startswith("-"):
                    pass
                else:
                    positional.append(word)
                i += 1
            if target and positional:
                name = positional[0]
                if regex and not name.startswith("^"):
                    # LLDB searches a regex anywhere in the type name, but the
                    # section format needs a leading ^ to mark a regex.
                    name = "^" + name if name.startswith(".*") else "^.*" + name
                self.registrations.setdefault(target, name)
                self.type_options.setdefault(target, options)

    @staticmethod
    def _constant_string(node: ast.AST) -> Optional[str]:
        if isinstance(node, ast.Constant) and isinstance(node.value, str):
            return node.value
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Add):
            left = CppTranslator._constant_string(node.left)
            right = CppTranslator._constant_string(node.right)
            if left is not None and right is not None:
                return left + right
        return None

    # Top level.

    def translate(self) -> str:
        tree = ast.parse(self.source)
        self._read_registrations(tree)
        for node in tree.body:
            if isinstance(node, ast.ClassDef):
                self.classes[node.name] = node
            elif isinstance(node, ast.FunctionDef):
                self.functions[node.name] = node

        called = {
            n.func.id
            for n in ast.walk(tree)
            if isinstance(n, ast.Call) and isinstance(n.func, ast.Name)
        }
        candidates = []
        for node in tree.body:
            if isinstance(node, ast.FunctionDef) and self._is_summary(node):
                # With registrations, only registered functions are formatters.
                # Otherwise anything another function calls is a helper.
                if self.registrations:
                    if node.name in self.registrations:
                        candidates.append(node)
                elif node.name not in called:
                    candidates.append(node)
            elif isinstance(node, ast.ClassDef) and self._is_synthetic(node):
                if not self.registrations or node.name in self.registrations:
                    candidates.append(node)
        blocks = []
        for node in candidates:
            type_name = self.registrations.get(node.name) or self.default_type_name
            if type_name is None:
                self._warn(
                    node,
                    f"no type found for {node.name}; add a `type summary add` command to "
                    "__lldb_init_module or pass --type-name",
                )
                type_name = "FIXME_TYPE_NAME"
            try:
                if isinstance(node, ast.FunctionDef):
                    body = self._summary(node, type_name)
                else:
                    body = self._synthetic(node, type_name)
            except Exception as error:  # Keep going: report it and move on.
                body = f"// FIXME_UNSUPPORTED: {node.name} could not be translated ({type(error).__name__}: {error})\n"
                self._warn(node, f"{node.name} could not be translated ({type(error).__name__}: {error})")
            blocks.append(self._quote(node) + body)

        out = [
            '#include "lldbfmt/script.h"',
            "",
            "using namespace lldbfmt::script;",
            "",
        ]
        if not blocks:
            out.append("// No formatters were found.")
        out.append("\n\n".join(blocks))
        return "\n".join(out).rstrip("\n") + "\n"

    @staticmethod
    def _is_summary(node: ast.FunctionDef) -> bool:
        args = [a.arg for a in node.args.args]
        return len(args) == 2 and not node.name.startswith("__")

    @staticmethod
    def _is_synthetic(node: ast.ClassDef) -> bool:
        names = {n.name for n in node.body if isinstance(n, ast.FunctionDef)}
        return bool(names & {"num_children", "get_child_at_index"})

    def _quote(self, node: ast.AST) -> str:
        if not self.quote_source:
            return ""
        end = getattr(node, "end_lineno", node.lineno)
        text = self.lines[node.lineno - 1 : end]
        return "".join(("// " + line).rstrip() + "\n" for line in text) + "//\n"

    # Identifiers.

    def _reset(self, reserved: list[str]) -> None:
        self._used = set(reserved)
        self._counter = {}
        self._referenced = set()

    def _identifier(self, base: str) -> str:
        base = base.replace(".", "_")
        if base in _CPP_KEYWORDS or base in _DSL_NAMES:
            base += "_"
        if base not in self._used:
            self._used.add(base)
            return base
        n = self._counter.get(base, 1)
        while f"{base}_{n}" in self._used:
            n += 1
        self._counter[base] = n + 1
        name = f"{base}_{n}"
        self._used.add(name)
        return name

    @staticmethod
    def _param(name: str) -> str:
        name = name.replace(".", "_")
        if name in _CPP_KEYWORDS or name in _DSL_NAMES:
            name += "_"
        return name

    # Summaries.

    def _summary(self, node: ast.FunctionDef, type_name: str) -> str:
        valobj = self._param(node.args.args[0].arg)
        self._reset([valobj])
        env = {node.args.args[0].arg: valobj}
        # The second argument is LLDB's internal dict, which nothing here uses.
        env[node.args.args[1].arg] = _UNUSED_ARGUMENT
        lines, result = self._lower(node.body, env)
        body = self._lambda_body(lines, result, node, indent=4)
        return (
            f"{self._macro('SUMMARY', node.name)}({node.name}, {_cpp_string(type_name)}, "
            f"{self._flags_argument(node.name)}"
            f"[](auto {valobj}) {{\n{body}}});\n"
        )

    # Synthetic providers.

    def _synthetic(self, node: ast.ClassDef, type_name: str) -> str:
        methods = {n.name: n for n in node.body if isinstance(n, ast.FunctionDef)}
        valobj = "valobj"
        attrs: list[str] = []  # init then update attributes, in order
        pieces: list[str] = []

        def attribute_env() -> dict[str, str]:
            env = {"self.valobj": valobj}
            env.update(self._class_constants(node))
            for attr in attrs:
                env[f"self.{attr}"] = self._param(attr)
            return env

        init = methods.get("__init__")
        update = methods.get("update")

        if init is not None:
            self._reset([valobj])
            env = {"self.valobj": valobj}
            if len(init.args.args) >= 2:
                env[init.args.args[1].arg] = valobj
            if len(init.args.args) >= 3:
                env[init.args.args[2].arg] = _UNUSED_ARGUMENT
            lines, _ = self._lower(init.body, env)
            init_attrs = [a for a in self._assigned_attrs(init.body) if a != "valobj"]
            if init_attrs:
                state = self._state(env, init_attrs, "state")
                pieces.append(self._lambda_text("init", [(valobj, False)], lines, state))
            attrs += init_attrs

        if update is not None:
            self._reset([valobj] + [self._param(a) for a in attrs])
            env = attribute_env()
            self._class_stack = [node]
            lines, result = self._lower(update.body, env, returns_flag=True)
            self._class_stack = []
            update_attrs = [
                a for a in self._assigned_attrs(update.body) if a != "valobj" and a not in attrs
            ]
            if result not in (None, "0", "1"):
                self._warn(update, "update() must end in `return True` or `return False`")
            state = self._state(env, update_attrs, "state_reuse" if result == "1" else "state")
            params = [(valobj, False)] + [(self._param(a), False) for a in attrs]
            pieces.append(self._lambda_text("update", params, lines, state))
            attrs += update_attrs

        getters = [
            ("num_children", "num_children"),
            ("get_child_index", "child_index"),
            ("get_child_at_index", "child_at_index"),
            ("get_value", "value"),
        ]
        for python_name, wrapper in getters:
            fn = methods.get(python_name)
            if fn is None:
                continue
            arguments = [a.arg for a in fn.args.args[1:]]
            names = [valobj] + [self._param(a) for a in attrs] + [self._param(a) for a in arguments]
            self._reset(names)
            env = attribute_env()
            for argument in arguments:
                env[argument] = self._param(argument)
            self._index_method = python_name == "get_child_index"
            self._class_stack = [node]
            lines, result = self._lower(fn.body, env)
            self._class_stack = []
            self._index_method = False
            if result is None:
                result = "null"  # Falling off the end returns None.
            pieces.append(self._lambda_text(wrapper, [(n, False) for n in names], lines, result))

        known = {"__init__", "update", "num_children", "get_child_index", "get_child_at_index", "get_value", "has_children"}
        for name, fn in methods.items():
            if name not in known:
                self._warn(fn, f"method {name}() has no bytecode equivalent and was not translated")

        joined = ",\n\n".join(pieces)
        return (
            f"{self._macro('SYNTHETIC', node.name)}({node.name}, {_cpp_string(type_name)},"
            f"{self._flags_argument(node.name) or chr(10)}{joined});\n"
        )

    _OPTION_NAMES = (
        (1 << 0, "cascade"), (1 << 1, "skip_pointers"), (1 << 2, "skip_references"),
        (1 << 3, "hide_children"), (1 << 4, "hide_value"), (1 << 5, "show_one_liner"),
    )  # fmt: skip

    def _macro(self, kind: str, name: str) -> str:
        """LLDBFMT_SCRIPT_SUMMARY, or _WITH_FLAGS when the options are not the default."""
        suffix = "" if self.type_options.get(name, 1) == 1 else "_WITH_FLAGS"
        return f"LLDBFMT_SCRIPT_{kind}{suffix}"

    def _flags_argument(self, name: str) -> str:
        options = self.type_options.get(name, 1)
        if options == 1:
            return ""
        names = [f"::lldbfmt::type_options::{n}" for bit, n in self._OPTION_NAMES if options & bit]
        return (" | ".join(names) or "::lldbfmt::type_options::none") + ", "

    @staticmethod
    def _assigned_attrs(body: list[ast.stmt]) -> list[str]:
        found: list[str] = []
        for node in ast.walk(ast.Module(body=body, type_ignores=[])):
            targets: list[ast.expr] = []
            if isinstance(node, ast.Assign):
                for t in node.targets:
                    targets += list(t.elts) if isinstance(t, ast.Tuple) else [t]
            elif isinstance(node, (ast.AugAssign, ast.AnnAssign)):
                targets = [node.target]
            for t in targets:
                if (
                    isinstance(t, ast.Attribute)
                    and isinstance(t.value, ast.Name)
                    and t.value.id == "self"
                    and t.attr not in found
                ):
                    found.append(t.attr)
        return found

    def _state(self, env: dict[str, str], attrs: list[str], function: str) -> str:
        names = []
        for attr in attrs:
            name = env.get(f"self.{attr}")
            if name:
                self._referenced.add(name)
            names.append(name if name else self._warn(None, f"self.{attr} is not assigned on every path"))
        return f"{function}({', '.join(names)})"

    def _lambda_text(self, wrapper: str, params: list[tuple[str, bool]], lines: list[str], result: Any) -> str:
        parts = []
        for name, always_used in params:
            unused = not always_used and name not in self._referenced
            parts.append(("[[maybe_unused]] " if unused else "") + f"auto {name}")
        body = self._lambda_body(lines, result, None, indent=8)
        return f"    {wrapper}([]({', '.join(parts)}) {{\n{body}    }})"

    def _lambda_body(self, lines: list[str], result: Any, node: Optional[ast.AST], indent: int) -> str:
        pad = " " * indent
        out = [f"{pad}{line}\n" for line in lines]
        if result is None:
            result = self._warn(node, "function does not return a value on every path")
        out.append(f"{pad}return {self._format(result, indent)};\n")
        return "".join(out)

    # Statements.

    def _lower(
        self,
        stmts: list[ast.stmt],
        env: dict[str, Any],
        returns_flag: bool = False,
        outer: tuple = (),
    ) -> tuple[list[str], Any]:
        lines: list[str] = []
        saved = self._sink
        self._sink = lines
        try:
            result = self._lower_into(lines, stmts, env, returns_flag, outer)
        finally:
            self._sink = saved
        return lines, result

    def _lower_into(
        self,
        lines: list[str],
        stmts: list[ast.stmt],
        env: dict[str, Any],
        returns_flag: bool,
        outer: tuple,
    ) -> Any:
        """Lower statements to `auto` declarations and a result expression (or
        None if the statements do not always return). Declarations from every
        branch are hoisted: they only name expressions, which are not evaluated
        unless the branch that uses them runs. `outer` is what follows these
        statements in the enclosing blocks, for an `if` that returns on only
        some paths to carry on with."""
        stmts = list(stmts)
        i = 0
        while i < len(stmts):
            s = stmts[i]
            i += 1
            if isinstance(s, ast.Assign):
                self._assign(s, s.targets, s.value, env, lines)
            elif isinstance(s, ast.AnnAssign) and s.value is not None:
                self._assign(s, [s.target], s.value, env, lines)
            elif isinstance(s, ast.AugAssign):
                if type(s.op) not in _BINOPS:
                    self._warn(s, "unsupported augmented assignment")
                    continue
                value = ast.copy_location(ast.BinOp(left=s.target, op=s.op, right=s.value), s)
                self._assign(s, [s.target], value, env, lines)
            elif isinstance(s, ast.Pass):
                pass
            elif isinstance(s, ast.Expr):
                if isinstance(s.value, ast.Constant):
                    pass  # A docstring.
                elif (
                    isinstance(s.value, ast.Call)
                    and isinstance(s.value.func, ast.Attribute)
                    and isinstance(s.value.func.value, ast.Name)
                    and s.value.func.value.id == "self"
                ):
                    self._method_call(s.value.func, s.value, env, statement=True)  # self.update()
                else:
                    self._warn(s, "expression statement ignored")
            elif isinstance(s, ast.Assert):
                note = f"{self.filename}:{s.lineno}: assert dropped"
                if note not in self.notes:
                    self.notes.append(note)
            elif isinstance(s, ast.Return):
                if returns_flag and isinstance(s.value, ast.Constant) and isinstance(s.value.value, bool):
                    return "1" if s.value.value else "0"
                return self._expr_or_none(s.value, env)
            elif isinstance(s, ast.Try):
                self._warn(s, "try/except: only the try body is translated; a failure is an error in LLDB")
                stmts[i:i] = s.body
            elif isinstance(s, ast.If):
                lookup = self._try_lookup(s, stmts[i:] + list(outer), env, lines)
                if lookup is not None:
                    return lookup
                cond = self._expr(s.test, env)
                rest = stmts[i:] + list(outer)
                then_env, else_env = dict(env), dict(env)
                then_lines, then_result = self._lower(s.body, then_env, returns_flag, tuple(rest))
                else_lines, else_result = self._lower(s.orelse, else_env, returns_flag, tuple(rest))
                if then_result is not None and else_result is not None:
                    lines += then_lines + else_lines
                    return _Chain(cond, then_result, else_result)
                if then_result is not None:
                    # The else path carries on with the statements after the if.
                    tail_lines, tail_result = self._lower(rest, else_env, returns_flag)
                    lines += then_lines + else_lines + tail_lines
                    return _Chain(cond, then_result, tail_result if tail_result is not None else "null")
                if else_result is not None:
                    tail_lines, tail_result = self._lower(rest, then_env, returns_flag)
                    lines += then_lines + else_lines + tail_lines
                    return _Chain(cond, tail_result if tail_result is not None else "null", else_result)
                # Neither branch returns: merge what they assigned.
                lines += then_lines + else_lines
                for key in dict.fromkeys(list(then_env) + list(else_env)):
                    a, b = then_env.get(key), else_env.get(key)
                    if a == env.get(key) and b == env.get(key):
                        continue
                    if a is None or b is None:
                        if key.startswith("self."):
                            self._warn(s, f"{key} is not assigned on every path")
                        else:
                            env[key] = a or b  # Only meaningful where it was assigned.
                        continue
                    name = self._identifier(key.split(".")[-1])
                    lines.append(f"auto {name} = if_({_strip_parens(cond)}, {a}, {b});")
                    env[key] = name
            else:
                self._warn(s, f"unsupported statement ({type(s).__name__})")
        return None

    @staticmethod
    def _is_self_update(node: ast.expr) -> bool:
        return (
            isinstance(node, ast.Call)
            and isinstance(node.func, ast.Attribute)
            and isinstance(node.func.value, ast.Name)
            and node.func.value.id == "self"
            and node.func.attr == "update"
        )

    def _assign(self, node: ast.AST, targets: list[ast.expr], value: ast.expr, env: dict[str, str], lines: list[str]) -> None:
        for target in targets:
            if isinstance(target, ast.Tuple):
                if isinstance(value, ast.Tuple) and len(value.elts) == len(target.elts):
                    for t, v in zip(target.elts, value.elts):
                        self._assign(node, [t], v, env, lines)
                else:
                    self._warn(node, "unsupported tuple assignment")
                continue
            key = None
            if isinstance(target, ast.Name):
                key = target.id
            elif (
                isinstance(target, ast.Attribute)
                and isinstance(target.value, ast.Name)
                and target.value.id == "self"
            ):
                key = f"self.{target.attr}"
                if target.attr == "valobj" and isinstance(value, ast.Name) and value.id in env:
                    env[key] = env[value.id]  # self.valobj = valobj
                    continue
            if key is None:
                self._warn(node, "unsupported assignment target")
                continue
            code = self._expr_or_none(value, env)
            if isinstance(code, _Instance):
                env[key] = code
                continue
            name = self._identifier(key.split(".")[-1])
            lines.append(f"auto {name} = {_strip_parens(code)};")
            env[key] = name

    # String lookup.

    def _try_lookup(self, first: ast.If, rest: list[ast.stmt], env: dict[str, str], lines: list[str]) -> Optional[Any]:
        """An `if x == "a": return ...` chain becomes lookup(x, "a", ..., default).
        The bytecode cannot compare strings, but it can look one up."""
        subject = None
        entries: list[tuple[str, Any]] = []
        found: list[str] = []
        node: ast.stmt = first
        trailing: list[ast.stmt] = []
        while True:
            matched = self._string_comparison(node.test)  # type: ignore[attr-defined]
            if matched is None:
                return None
            key_node, literal = matched
            key_code = self._expr(key_node, env)
            if subject is not None and key_code != subject:
                return None
            subject = key_code
            body_lines, body_result = self._lower(node.body, dict(env))  # type: ignore[attr-defined]
            if body_result is None:
                return None
            found += body_lines
            entries.append((literal, body_result))
            orelse = node.orelse  # type: ignore[attr-defined]
            if len(orelse) == 1 and isinstance(orelse[0], ast.If):
                node = orelse[0]
                continue
            if not orelse and rest and isinstance(rest[0], ast.If):
                # `if x == "a": return 1` followed by `if x == "b": ...`
                node, rest = rest[0], rest[1:]
                continue
            trailing = list(orelse)
            break
        tail_lines, tail_result = self._lower(trailing + rest, dict(env))
        if self._index_method and tail_result == "null":
            tail_result = None  # An unknown name is simply not found.
        lines += found + tail_lines
        pairs = ", ".join(f"{_cpp_string(k)}, {self._format(v, 0)}" for k, v in entries)
        subject = _strip_parens(subject or "")
        if tail_result is None:
            return f"lookup({subject}, {pairs})"
        return f"lookup({subject}, {pairs}, {self._format(tail_result, 0)})"

    @staticmethod
    def _string_comparison(test: ast.expr) -> Optional[tuple[ast.expr, str]]:
        if isinstance(test, ast.Compare) and len(test.ops) == 1 and isinstance(test.ops[0], ast.Eq):
            left, right = test.left, test.comparators[0]
            if isinstance(right, ast.Constant) and isinstance(right.value, str):
                return left, right.value
            if isinstance(left, ast.Constant) and isinstance(left.value, str):
                return right, left.value
        return None

    # Expressions.

    def _format(self, code: Any, column: int) -> str:
        """Print an expression; if/elif chains become one if_(...)."""
        if not isinstance(code, _Chain):
            return _strip_parens(code)
        parts: list[str] = []
        node: Any = code
        while isinstance(node, _Chain):
            parts += [_strip_parens(node.cond), self._format(node.then, column + 4)]
            node = node.otherwise
        parts.append(self._format(node, column + 4))
        flat = f"if_({', '.join(parts)})"
        if len(flat) + column <= 100:
            return flat
        pad = " " * (column + 4)
        return "if_(\n" + pad + (",\n" + pad).join(parts) + ")"

    def _expr_or_none(self, node: Optional[ast.expr], env: dict[str, str]) -> str:
        return "null" if node is None else self._expr(node, env)

    def _expr(self, node: ast.expr, env: dict[str, str]) -> str:
        if isinstance(node, ast.Constant):
            value = node.value
            if value is None:
                return "null"
            if isinstance(value, bool):
                return "1" if value else "0"
            if isinstance(value, str):
                return _cpp_string(value)
            if isinstance(value, int):
                return str(value)
            return self._warn(node, f"unsupported constant {value!r}")
        if isinstance(node, ast.Name):
            name = env.get(node.id)
            if name is None and node.id == "dict":
                return _UNUSED_ARGUMENT  # Some formatters pass the builtin by mistake.
            if name is None:
                return self._warn(node, f"unknown name {node.id}")
            if isinstance(name, _Instance):
                return name
            self._referenced.add(name)
            return name
        if isinstance(node, ast.Attribute):
            if isinstance(node.value, ast.Name) and node.value.id == "self":
                name = env.get(f"self.{node.attr}")
                if name is None:
                    return self._warn(node, f"self.{node.attr} is not an attribute assigned in __init__ or update")
                self._referenced.add(name)
                return name
            owner = env.get(node.value.id) if isinstance(node.value, ast.Name) else None
            if isinstance(owner, _Instance):  # provider.size
                value = owner.attributes.get(f"self.{node.attr}")
                if value is None:
                    return self._warn(node, f"{owner.cls.name} has no attribute {node.attr}")
                return value
            if node.attr in _SB_PROPERTIES:
                return f"{self._wrap(node.value, env)}.{_SB_PROPERTIES[node.attr]}()"
            return self._warn(node, f"unsupported attribute .{node.attr}")
        if isinstance(node, ast.Call):
            return self._call(node, env)
        if isinstance(node, ast.Compare):
            return self._compare(node, env)
        if isinstance(node, ast.BoolOp):
            op = " && " if isinstance(node.op, ast.And) else " || "
            return "(" + op.join(self._expr(v, env) for v in node.values) + ")"
        if isinstance(node, ast.UnaryOp):
            if isinstance(node.op, ast.Not):
                return f"!{self._wrap(node.operand, env)}"
            if isinstance(node.op, ast.Invert):
                return f"~{self._wrap(node.operand, env)}"
            if isinstance(node.op, ast.USub) and isinstance(node.operand, ast.Constant):
                return str(-node.operand.value)
            return self._warn(node, "unsupported unary operator")
        if isinstance(node, ast.BinOp):
            return self._binop(node, env)
        if isinstance(node, ast.IfExp):
            return (
                f"if_({_strip_parens(self._expr(node.test, env))}, "
                f"{_strip_parens(self._expr(node.body, env))}, {_strip_parens(self._expr(node.orelse, env))})"
            )
        if isinstance(node, ast.JoinedStr):
            return self._joined(node, env)
        return self._warn(node, f"unsupported expression ({type(node).__name__})")

    def _wrap(self, node: ast.expr, env: dict[str, str]) -> str:
        """An expression safe to use as an operand or receiver."""
        text = self._expr(node, env)
        if re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*(\.[A-Za-z_]\w*\(.*\))*", text, re.S) or text.startswith("("):
            return text
        if re.fullmatch(r"\w+\(.*\)", text, re.S):
            return text
        return f"({text})"

    def _compare(self, node: ast.Compare, env: dict[str, str]) -> str:
        parts = []
        left = node.left
        for op, right in zip(node.ops, node.comparators):
            if isinstance(op, (ast.Is, ast.IsNot)) and isinstance(right, ast.Constant) and right.value is None:
                parts.append(f"{self._wrap(left, env)}.{'is_null' if isinstance(op, ast.Is) else 'is_valid'}()")
            elif type(op) in _CMPOPS:
                if self._string_comparison(ast.Compare(left=left, ops=[op], comparators=[right])) is not None:
                    parts.append(self._warn(node, "a string cannot be compared in bytecode; compare one value to string constants in an if/elif chain so it can become a Lookup"))
                else:
                    parts.append(f"({self._expr(left, env)} {_CMPOPS[type(op)]} {self._expr(right, env)})")
            else:
                parts.append(self._warn(node, "unsupported comparison"))
            left = right
        return parts[0] if len(parts) == 1 else "(" + " && ".join(parts) + ")"

    # Inlining of helper classes, methods and functions.

    def _bind(self, fn: ast.FunctionDef, node: ast.Call, env: dict[str, Any], skip_self: bool) -> Optional[dict[str, Any]]:
        params = fn.args.args[1:] if skip_self else fn.args.args
        if len(node.args) > len(params) or node.keywords:
            self._warn(node, f"cannot inline {fn.name}: unsupported arguments")
            return None
        bound: dict[str, Any] = {}
        defaults = fn.args.defaults
        for i, param in enumerate(params):
            if i < len(node.args):
                bound[param.arg] = self._expr(node.args[i], env)
            else:
                d = i - (len(params) - len(defaults))
                if d < 0:
                    self._warn(node, f"cannot inline {fn.name}: missing argument {param.arg}")
                    return None
                bound[param.arg] = self._expr(defaults[d], env)
        return bound

    def _inline(self, fn: ast.FunctionDef, bound: dict[str, Any], self_env: dict[str, Any], cls: Optional[ast.ClassDef], node: ast.AST, writeback: Optional[dict[str, Any]] = None) -> Any:
        if self._depth >= 12:
            return self._warn(node, f"inlining {fn.name} is nested too deeply")
        inner = dict(bound)
        inner.update(self_env)
        self._depth += 1
        self._class_stack.append(cls)  # type: ignore[arg-type]
        try:
            lines, result = self._lower(fn.body, inner)
        finally:
            self._class_stack.pop()
            self._depth -= 1
        self._sink.extend(lines)
        if writeback is not None:  # `self.update()`: the attributes it assigned
            writeback.update({k: v for k, v in inner.items() if k.startswith("self.")})
        if result is None and writeback is not None:
            return "null"
        if result is None:
            return self._warn(node, f"{fn.name} does not return a value on every path")
        if isinstance(result, _Instance):
            return result  # A factory method: the caller inlines on the object.
        return self._format(result, 0)

    def _class_constants(self, cls: ast.ClassDef) -> dict[str, Any]:
        """Class-level `NAME = 3` constants, readable as self.NAME."""
        constants: dict[str, Any] = {}
        for n in cls.body:
            if (
                isinstance(n, ast.Assign)
                and len(n.targets) == 1
                and isinstance(n.targets[0], ast.Name)
                and isinstance(n.value, (ast.Constant, ast.UnaryOp))
            ):
                constants[f"self.{n.targets[0].id}"] = self._expr(n.value, {})
        return constants

    def _instantiate(self, cls: ast.ClassDef, node: ast.Call, env: dict[str, Any]) -> Any:
        init = next((n for n in cls.body if isinstance(n, ast.FunctionDef) and n.name == "__init__"), None)
        if init is None:
            return _Instance(cls, self._class_constants(cls))
        bound = self._bind(init, node, env, skip_self=True)
        if bound is None:
            return self._warn(node, f"cannot construct {cls.name}")
        inner = dict(bound)
        inner.update(self._class_constants(cls))
        self._depth += 1
        self._class_stack.append(cls)
        try:
            lines, _ = self._lower(init.body, inner)
        finally:
            self._class_stack.pop()
            self._depth -= 1
        self._sink.extend(lines)
        return _Instance(cls, {k: v for k, v in inner.items() if k.startswith("self.")})

    def _method_call(self, func: ast.Attribute, node: ast.Call, env: dict[str, Any], statement: bool = False) -> Optional[Any]:
        """Inline `obj.method(...)` for helper-class objects, `Cls(...).method(...)`
        and `self.method(...)`. Returns None if this is not such a call."""
        receiver = func.value
        instance: Optional[_Instance] = None
        if isinstance(receiver, ast.Name) and isinstance(env.get(receiver.id), _Instance):
            instance = env[receiver.id]
        elif isinstance(receiver, ast.Call) and isinstance(receiver.func, ast.Name) and receiver.func.id in self.classes:
            made = self._instantiate(self.classes[receiver.func.id], receiver, env)
            if not isinstance(made, _Instance):
                return made
            instance = made
        elif isinstance(receiver, ast.Name) and receiver.id == "self" and self._class_stack and self._class_stack[-1] is not None:
            cls = self._class_stack[-1]
            instance = _Instance(cls, {k: v for k, v in env.items() if k.startswith("self.")})
        if instance is None:
            return None
        method = next((n for n in instance.cls.body if isinstance(n, ast.FunctionDef) and n.name == func.attr), None)
        if method is None:
            return self._warn(node, f"{instance.cls.name} has no method {func.attr}")
        bound = self._bind(method, node, env, skip_self=True)
        if bound is None:
            return self._warn(node, f"cannot inline {func.attr}")
        writeback = env if statement and isinstance(receiver, ast.Name) and receiver.id == "self" else None
        return self._inline(method, bound, instance.attributes, instance.cls, node, writeback)

    def _call(self, node: ast.Call, env: dict[str, Any]) -> Any:
        func = node.func
        if isinstance(func, ast.Name) and func.id in self.classes:
            return self._instantiate(self.classes[func.id], node, env)
        if isinstance(func, ast.Attribute):
            inlined = self._method_call(func, node, env)
            if inlined is not None:
                return inlined
        if isinstance(func, ast.Name) and func.id in self.functions:
            fn = self.functions[func.id]
            bound = self._bind(fn, node, env, skip_self=False)
            if bound is not None:
                return self._inline(fn, bound, {}, None, node)
        if node.keywords:
            return self._warn(node, "keyword arguments are not supported")
        if isinstance(func, ast.Attribute) and func.attr in _SB_METHODS:
            receiver = self._wrap(func.value, env)
            args = node.args
            if func.attr in _DEFAULTING_METHODS and args:
                note = (
                    f"{self.filename}:{node.lineno}: {func.attr}() default argument dropped; "
                    "an unreadable value is an error in bytecode"
                )
                if note not in self.notes:
                    self.notes.append(note)
                args = []
            rendered = ", ".join(_strip_parens(self._expr(a, env)) for a in args)
            return f"{receiver}.{_snake(func.attr)}({rendered})"
        if isinstance(func, ast.Name) and func.id == "len" and len(node.args) == 1:
            return f"{self._wrap(node.args[0], env)}.length()"
        if isinstance(func, ast.Name) and func.id in ("int", "bool") and len(node.args) == 1:
            return self._expr(node.args[0], env)
        if isinstance(func, ast.Name) and func.id == "str" and len(node.args) == 1:
            return f'format("%s", {_strip_parens(self._expr(node.args[0], env))})'
        name = ast.unparse(func) if hasattr(ast, "unparse") else "function"
        return self._warn(node, f"unsupported call to {name}")

    @staticmethod
    def _is_stringy(node: ast.expr, env: Optional[dict[str, Any]] = None) -> bool:
        """Whether the expression is known to be a string: a string literal, an
        f-string, str(), GetValue(), GetSummary(), or a local holding one. Unknown
        types count as not a string. Used to turn + on strings into format()."""
        if isinstance(node, ast.Constant):
            return isinstance(node.value, str)
        if isinstance(node, ast.JoinedStr):
            return True
        if isinstance(node, ast.Call):
            if isinstance(node.func, ast.Name):
                return node.func.id == "str"
            return isinstance(node.func, ast.Attribute) and node.func.attr in ("GetValue", "GetSummary")
        if isinstance(node, ast.Attribute):
            return node.attr in ("summary", "value")
        if isinstance(node, ast.Name) and env is not None:
            text = env.get(node.id)
            return isinstance(text, str) and (text.startswith('"') or text.startswith("format("))
        return False

    def _binop(self, node: ast.BinOp, env: dict[str, str]) -> str:
        # "..." % args
        if isinstance(node.op, ast.Mod) and isinstance(node.left, ast.Constant) and isinstance(node.left.value, str):
            args = node.right.elts if isinstance(node.right, ast.Tuple) else [node.right]
            rendered = "".join(", " + _strip_parens(self._expr(a, env)) for a in args)
            return f"format({_cpp_string(self._python_format(node.left.value, node))}{rendered})"
        # String concatenation becomes format().
        if isinstance(node.op, ast.Add):
            terms: list[ast.expr] = []

            def flatten(n: ast.expr) -> None:
                if isinstance(n, ast.BinOp) and isinstance(n.op, ast.Add):
                    flatten(n.left)
                    flatten(n.right)
                else:
                    terms.append(n)

            flatten(node)
            if any(self._is_stringy(t, env) for t in terms):
                text = ""
                args = []
                for t in terms:
                    if isinstance(t, ast.Constant) and isinstance(t.value, str):
                        text += t.value.replace("%", "%%")
                    elif isinstance(t, ast.Call) and isinstance(t.func, ast.Name) and t.func.id == "str" and len(t.args) == 1:
                        text += "%s"
                        args.append(self._expr(t.args[0], env))
                    else:
                        text += "%s"
                        args.append(self._expr(t, env))
                rendered = "".join(", " + _strip_parens(a) for a in args)
                return f"format({_cpp_string(text)}{rendered})"
        op = _BINOPS.get(type(node.op))
        if op is None:
            return self._warn(node, "unsupported operator")
        return f"({self._expr(node.left, env)} {op} {self._expr(node.right, env)})"

    def _python_format(self, text: str, node: ast.AST) -> str:
        for match in re.finditer(r"%[-#0 +]*\d*(?:\.\d+)?(.)", text):
            if match.group(1) not in "sdiu%":
                self._warn(node, f"format specifier {match.group(0)} is not supported (only %s %d %i %u %%)")
        return text

    def _joined(self, node: ast.JoinedStr, env: dict[str, str]) -> str:
        text = ""
        args = []
        for part in node.values:
            if isinstance(part, ast.Constant):
                text += str(part.value).replace("%", "%%")
            elif isinstance(part, ast.FormattedValue):
                if part.format_spec is not None or part.conversion not in (-1, 115):
                    return self._warn(node, "f-string format specs are not supported")
                text += "%s"
                args.append(self._expr(part.value, env))
        rendered = "".join(", " + _strip_parens(a) for a in args)
        return f"format({_cpp_string(text)}{rendered})"


################################################################################
# Helper functions.
################################################################################


def _to_uleb(value: int) -> bytes:
    """Encode an integer to ULEB128 bytes."""
    if value < 0:
        raise ValueError(f"negative number cannot be encoded to ULEB128: {value}")

    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value != 0:
            byte |= 0x80
        result.append(byte)
        if value == 0:
            break

    return bytes(result)


def _to_sleb(value: int) -> bytes:
    """Encode an integer to SLEB128 bytes."""
    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        done = (value == 0 and not byte & 0x40) or (value == -1 and byte & 0x40)
        result.append(byte if done else byte | 0x80)
        if done:
            return bytes(result)


def _from_sleb(stream: BinaryIO) -> int:
    """Decode a SLEB128 integer by reading bytes from the stream."""
    result = 0
    shift = 0
    while True:
        byte = stream.read(1)[0]
        result |= (byte & 0x7F) << shift
        shift += 7
        if not (byte & 0x80):
            if byte & 0x40:
                result -= 1 << shift
            return result


def _from_uleb(stream: BinaryIO) -> int:
    """Decode a ULEB128 integer by reading bytes from the stream."""
    result = 0
    shift = 0
    while True:
        byte = stream.read(1)[0]
        result |= (byte & 0x7F) << shift
        shift += 7
        if not (byte & 0x80):
            break

    return result


def _to_byte(n: int) -> bytes:
    return n.to_bytes(1, "big")


def _main():
    import argparse

    parser = argparse.ArgumentParser(
        description="""
    Assembler, disassembler, and interpreter for LLDB dataformatter bytecode.
    See https://lldb.llvm.org/resources/formatterbytecode.html for more details.
    """
    )
    parser.add_argument("input", help="input file")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "-c",
        "--compile",
        action="store_true",
        help="compile a Python LLDB data formatter into LLDB formatter bytecode",
    )
    mode.add_argument(
        "-a",
        "--assemble",
        action="store_true",
        help="assemble assembly into bytecode",
    )
    mode.add_argument(
        "--dump-section",
        action="store_true",
        help="print every formatter record in the formatter section of a built "
        "binary (Mach-O or ELF), disassembled",
    )
    mode.add_argument(
        "-d",
        "--disassemble",
        action="store_true",
        help="disassemble bytecode",
    )
    parser.add_argument("-n", "--type-name", help="source type of formatter")
    parser.add_argument(
        "-o",
        "--output",
        help="output file (required for --assemble)",
    )
    parser.add_argument(
        "--append", action="store_true", help="append to existing output file"
    )
    parser.add_argument(
        "-f",
        "--format",
        choices=("binary", "c", "swift", "cpp"),
        default="binary",
        help="output file format",
    )
    parser.add_argument("-t", "--test", action="store_true", help="run unit tests")

    args = parser.parse_args()

    if args.append and not args.compile:
        parser.error("--append is valid only with --compile")

    if args.compile and args.format == "cpp":
        with open(args.input) as f:
            source = f.read()
        translator = CppTranslator(source, args.input, args.type_name)
        output_text = translator.translate()
        for note in translator.notes:
            print(f"note: {note}", file=sys.stderr)
        for warning in translator.warnings:
            print(f"warning: {warning}", file=sys.stderr)
        if args.output:
            with open(args.output, "a" if args.append else "w") as output:
                output.write(output_text)
        else:
            sys.stdout.write(output_text)
        if translator.warnings:
            sys.exit(1)
        return

    if args.compile:
        if not args.type_name:
            parser.error("--type-name is required with --compile")
        if not args.output:
            parser.error("--output is required with --compile")
        compiler = Compiler()
        try:
            assembly = compiler.compile(args.input)
        except CompilerError as e:
            print(f"{args.input}:{e.lineno}: {e}", file=sys.stderr)
            return

        section = assemble_file(args.type_name, io.StringIO(assembly))
        if args.format == "binary":
            with open(args.output, "wb") as output:
                section.write_binary(output)
        else:
            mode = "a" if args.append else "w"
            with open(args.output, mode) as output:
                section.write_source(output, language=args.format)
    elif args.assemble:
        if not args.type_name:
            parser.error("--type-name is required with --assemble")
        if not args.output:
            parser.error("--output is required with --assemble")
        with open(args.input) as input:
            section = assemble_file(args.type_name, input)
        if args.format == "binary":
            with open(args.output, "wb") as output:
                section.write_binary(output)
        else:
            with open(args.output, "w") as output:
                section.write_source(output, language=args.format)
    elif args.dump_section:
        dump_section(args.input, sys.stdout)
    elif args.disassemble:
        if args.output:
            with (
                open(args.input, "rb") as input,
                open(args.output, "w") as output,
            ):
                disassemble_file(input, output)
        else:
            with open(args.input, "rb") as input:
                disassemble_file(input, sys.stdout)


if __name__ == "__main__":
    if not ("-t" in sys.argv or "--test" in sys.argv):
        _main()
        sys.exit()

    ############################################################################
    # Tests.
    ############################################################################
    import unittest

    class TestAssembler(unittest.TestCase):

        def test_assemble(self):
            self.assertEqual(assemble("1u dup").hex(), "200101")
            self.assertEqual(assemble('"1u dup"').hex(), "2206317520647570")
            self.assertEqual(assemble("16 < { dup } if").hex(), "24105210010111")
            self.assertEqual(assemble('{ { " } " } }').hex(), "100710052203207d20")

            def roundtrip(asm):
                self.assertEqual(disassemble(assemble(asm))[0], asm)

            roundtrip("1u dup")
            roundtrip("16 < { dup } if")
            roundtrip('{ { " } " } }')

            # String specific checks.
            roundtrip('1u "2u 3u"')
            roundtrip('"a  b"')
            roundtrip('"a \\" b"')

            # Null literal.
            roundtrip("null")
            self.assertEqual(interpret(assemble("null is_null"), [], []), 1)
            self.assertIsNone(interpret(assemble("null"), [], []))

            self.assertEqual(interpret(assemble("1 1 +"), [], []), 2)
            self.assertEqual(interpret(assemble("2 1 1 + *"), [], []), 4)
            self.assertEqual(
                interpret(assemble('2 1 > { "yes" } { "no" } ifelse'), [], []), "yes"
            )

        def test_assemble_file(self):
            def run_assemble(type_name, asm):
                out = io.BytesIO()
                section = assemble_file(type_name, io.StringIO(asm))
                section.write_binary(out)
                out.seek(0)
                return out

            def run_disassemble(binary):
                out = io.StringIO()
                disassemble_file(binary, out)
                out.seek(0)
                return out

            # assemble -> disassemble -> assemble round-trip: binary is identical.
            asm = "@summary: dup @get_value_as_unsigned call return\n@get_num_children: drop 5u return"
            binary1 = run_assemble("MyType", asm)
            dis = run_disassemble(binary1)
            binary2 = run_assemble("MyType", dis.read())
            self.assertEqual(binary1.getvalue(), binary2.getvalue())

            # disassemble -> assemble -> disassemble round-trip: text is identical.
            dis2 = run_disassemble(binary2)
            self.assertEqual(dis.getvalue(), dis2.getvalue())

            # disassemble output contains expected signatures.
            self.assertIn("@summary:", dis.getvalue())
            self.assertIn("@get_num_children:", dis.getvalue())

            # Duplicate signature is an error.
            with self.assertRaises(ValueError):
                run_assemble("MyType", "@summary: 1u return\n@summary: 2u return")

        def test_write_source(self):
            # Use the Account example from main.cpp as a reference, whose
            # exact byte values are known.
            section = BytecodeSection(
                type_name="Account",
                flags=0,
                signatures=[
                    ("get_num_children", bytes([0x20, 0x01])),
                    ("get_child_at_index", bytes([0x02, 0x20, 0x00, 0x23, 0x11, 0x60])),
                ],
            )
            out = io.StringIO()
            section.write_source(out, language="c")
            src = out.getvalue()

            self.assertIn("__attribute__((used, section(FORMATTER_SECTION)))", src)
            self.assertIn("unsigned char _Account_formatter[] =", src)
            self.assertIn('"\\x01"', src)  # version
            self.assertIn('"\\x15"', src)  # record size (21)
            self.assertIn('"\\x07"', src)  # type name size (7)
            self.assertIn('"Account"', src)  # type name
            self.assertIn('"\\x00"', src)  # flags
            self.assertIn('"\\x02"', src)  # sig_get_num_children
            self.assertIn('"\\x20\\x01"', src)  # program
            self.assertIn('"\\x04"', src)  # sig_get_child_at_index
            self.assertIn('"\\x06"', src)  # program size
            self.assertIn('"\\x02\\x20\\x00\\x23\\x11\\x60"', src)  # program
            self.assertIn("// version", src)
            self.assertIn("// type name", src)
            self.assertIn("// program", src)
            # Semicolon terminates the array initializer.
            self.assertEqual(src.count(";"), 1)

            # Non-identifier characters in the type name are replaced with '_'.
            out2 = io.StringIO()
            BytecodeSection("std::vector<int>", 0, []).write_source(out2, language="c")
            self.assertIn("_std__vector_int__formatter[] =", out2.getvalue())

            # Flags are ULEB128 encoded to allow values wider than 7 bits.
            flags = 1 << 10
            wide = BytecodeSection("T", flags, [("summary", bytes([0x13]))])
            out3 = io.StringIO()
            wide.write_source(out3, language="c")
            expected = "".join(f"\\x{b:02x}" for b in _to_uleb(flags))
            self.assertIn(f'"{expected}"', out3.getvalue())
            binary = io.BytesIO()
            wide.write_binary(binary)
            binary.seek(0)
            dis = io.StringIO()
            disassemble_file(binary, dis)
            self.assertEqual(dis.getvalue(), "@summary: return\n")


    class TestCppTranslator(unittest.TestCase):
        def translate(self, source, type_name="T"):
            translator = CppTranslator(textwrap.dedent(source), "test.py", type_name, quote_source=False)
            return translator.translate(), translator

        def test_summary(self):
            out, t = self.translate(
                """
                def Summary(valobj, dict):
                    x = valobj.GetChildMemberWithName("x").GetValueAsSigned()
                    return "(%d, %s)" % (x, valobj.GetChildMemberWithName("y").GetValue())
                """
            )
            self.assertEqual(t.warnings, [])
            self.assertIn('LLDBFMT_SCRIPT_SUMMARY(Summary, "T", [](auto valobj) {', out)
            self.assertIn('auto x = valobj.get_child_member_with_name("x").get_value_as_signed();', out)
            self.assertIn('return format("(%d, %s)", x, valobj.get_child_member_with_name("y").get_value());', out)

        def test_string_concatenation_and_properties(self):
            out, t = self.translate(
                """
                def Summary(valobj, dict):
                    return "n = " + str(valobj.num_children) + "!"
                """
            )
            self.assertEqual(t.warnings, [])
            self.assertIn('format("n = %s!", valobj.get_num_children())', out)

        def test_if_chain_and_early_return(self):
            out, t = self.translate(
                """
                def Summary(valobj, dict):
                    n = valobj.GetNumChildren()
                    if n == 0:
                        return "none"
                    elif n == 1:
                        return "one"
                    return "many"
                """
            )
            self.assertEqual(t.warnings, [])
            self.assertIn('return if_(n == 0, "none", n == 1, "one", "many");', out)

        def test_branch_merge(self):
            out, t = self.translate(
                """
                def Summary(valobj, dict):
                    storage = valobj.GetChildMemberWithName("s")
                    if not storage:
                        storage = valobj
                    return storage.GetValue()
                """
            )
            self.assertEqual(t.warnings, [])
            self.assertIn("auto storage_2 = if_(!storage, storage_1, storage);", out)
            self.assertIn("return storage_2.get_value();", out)

        def test_synthetic_attributes_and_lookup(self):
            out, t = self.translate(
                """
                class Provider:
                    def __init__(self, valobj, dict):
                        self.valobj = valobj
                    def update(self):
                        self.size = self.valobj.GetChildMemberWithName("size").GetValueAsUnsigned()
                        return True
                    def num_children(self):
                        return self.size + 1
                    def get_child_index(self, name):
                        if name == "size":
                            return 0
                        if name == "other":
                            return 1
                        return None
                    def get_child_at_index(self, index):
                        return self.valobj.GetChildAtIndex(index)
                    def has_children(self):
                        return True
                """
            )
            self.assertEqual(t.warnings, [])
            self.assertIn("return state_reuse(size);", out)
            self.assertIn("num_children([]([[maybe_unused]] auto valobj, auto size) {", out)
            self.assertIn('return lookup(name, "size", 0, "other", 1);', out)
            self.assertIn("return valobj.get_child_at_index(index);", out)

        def test_registrations_give_type_names(self):
            out, t = self.translate(
                """
                def __lldb_init_module(debugger, d):
                    debugger.HandleCommand('type summary add -F m.Summary -x "Foo<.+>"')
                    debugger.HandleCommand('type summary add -F m.Other Bar::Baz')
                def Summary(valobj, dict):
                    return "a"
                def Other(valobj, dict):
                    return "b"
                """,
                type_name=None,
            )
            self.assertEqual(t.warnings, [])
            self.assertIn('LLDBFMT_SCRIPT_SUMMARY(Summary, "^.*Foo<.+>",', out)
            self.assertIn('LLDBFMT_SCRIPT_SUMMARY(Other, "Bar::Baz",', out)

        def test_unsupported_constructs_are_reported(self):
            out, t = self.translate(
                """
                def Summary(valobj, dict):
                    for i in range(3):
                        pass
                    return helper(valobj)
                """
            )
            self.assertEqual(len(t.warnings), 2)
            self.assertIn("FIXME_UNSUPPORTED", out)

        def test_helper_classes_and_functions_are_inlined(self):
            out, t = self.translate(
                """
                class Point:
                    KIND = 3
                    def __init__(self, valobj, dict):
                        self.valobj = valobj
                        self.update()
                    def update(self):
                        self.depth = self.valobj.GetChildMemberWithName("d").GetValueAsUnsigned()
                    def x(self):
                        return self.valobj.GetChildMemberWithName("x").GetValueAsSigned()
                    def twice(self):
                        return self.x() * 2 + self.KIND

                def label(valobj, prefix):
                    return prefix + valobj.GetValue()

                def Summary(valobj, dict):
                    p = Point(valobj, dict)
                    return "%s/%s/%s" % (p.twice(), p.depth, label(valobj, "v="))
                """
            )
            self.assertEqual(t.warnings, [])
            self.assertIn('auto depth = valobj.get_child_member_with_name("d").get_value_as_unsigned();', out)
            self.assertIn(
                'format("%s/%s/%s", (valobj.get_child_member_with_name("x").get_value_as_signed() * 2) + 3, depth, '
                'format("%s%s", "v=", valobj.get_value()))', out,
            )

        def test_registration_options_become_type_options(self):
            out, t = self.translate(
                """
                def __lldb_init_module(debugger, d):
                    debugger.HandleCommand('type summary add -C no -p -F m.A Foo')
                    debugger.HandleCommand('type summary add -F m.B Bar')
                def A(valobj, d):
                    return "a"
                def B(valobj, d):
                    return "b"
                """,
                type_name=None,
            )
            self.assertEqual(t.warnings, [])
            self.assertIn('LLDBFMT_SCRIPT_SUMMARY_WITH_FLAGS(A, "Foo", ::lldbfmt::type_options::skip_pointers, ', out)
            self.assertIn('LLDBFMT_SCRIPT_SUMMARY(B, "Bar", ', out)

        def test_names_do_not_collide_with_cpp(self):
            out, _ = self.translate(
                """
                def Summary(valobj, dict):
                    new = valobj.GetValue()
                    return new
                """
            )
            self.assertIn("auto new_ = valobj.get_value();", out)
            self.assertIn("return new_;", out)

    unittest.main(argv=[__file__])
