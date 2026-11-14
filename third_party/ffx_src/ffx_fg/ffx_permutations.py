#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the shader permutation headers AMD's FidelityFX SDK 1.1.4 blob accessors include
(src/backends/shared/blob_accessors/*_shaderblobs.cpp), in place of the SDK's Windows-only
FidelityFX-SC tool.

usage: ffx_permutations.py OUT_DIR PASS OPTION[,OPTION...] SPV0 [SPV1 ...]

PASS is the pass file's name without .glsl (ffx_frameinterpolation_setup_pass). OPTION names the
permutation key's bits in order (bit 0 first, "-" for none), and SPVi is the pass compiled with
bit k of i as option k. The bindings, their names and array sizes are read from the SPIR-V. Only
the wave32, 32-bit permutations exist: the wave64 and 16-bit headers name the same tables, so a
device asking for them gets these (the shaders read FFX_HALF and the wave size at compile time
only).
"""

import os
import struct
import sys

# SPIR-V opcodes and enums used here.
OP_NAME, OP_DECORATE, OP_MEMBER_DECORATE = 5, 71, 72
OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE = 25, 26, 27
OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER = 28, 29, 30, 32
OP_CONSTANT, OP_VARIABLE, OP_TYPE_ACCELERATION_STRUCTURE = 43, 59, 5341
DEC_BLOCK, DEC_BUFFER_BLOCK, DEC_NON_WRITABLE, DEC_BINDING, DEC_SET = 2, 3, 24, 33, 34
SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_STORAGE_BUFFER = 0, 2, 12
DIM_BUFFER = 5

KINDS = ("cbv", "srv_texture", "uav_texture", "srv_buffer", "uav_buffer", "sampler", "rt_accel")
# The PermutationInfo field names POPULATE_SHADER_BLOB_FFX reads (ffx_types.h), per kind.
FIELDS = {
    "cbv": ("numConstantBuffers", "constantBuffer"),
    "srv_texture": ("numSRVTextures", "srvTexture"),
    "uav_texture": ("numUAVTextures", "uavTexture"),
    "srv_buffer": ("numSRVBuffers", "srvBuffer"),
    "uav_buffer": ("numUAVBuffers", "uavBuffer"),
    "sampler": ("numSamplers", "sampler"),
    "rt_accel": ("numRTAccelerationStructures", "rtAccelerationStructure"),
}


def parse_string(words):
    data = struct.pack("<%dI" % len(words), *words)
    return data.split(b"\0", 1)[0].decode("utf-8")


def reflect(code):
    """[(kind, name, binding, set, count)] for every bound resource of a SPIR-V module."""
    words = struct.unpack("<%dI" % (len(code) // 4), code)
    if words[0] != 0x07230203:
        raise ValueError("not SPIR-V")
    names, decorations, member_non_writable, member_counts = {}, {}, {}, {}
    types, constants, variables = {}, {}, []
    i = 5
    while i < len(words):
        count, opcode = words[i] >> 16, words[i] & 0xFFFF
        ops = words[i + 1:i + count]
        if count == 0:
            raise ValueError("bad instruction")
        if opcode == OP_NAME:
            names[ops[0]] = parse_string(ops[1:])
        elif opcode == OP_DECORATE:
            decorations.setdefault(ops[0], {})[ops[1]] = ops[2] if len(ops) > 2 else True
        elif opcode == OP_MEMBER_DECORATE:
            if ops[2] == DEC_NON_WRITABLE:
                member_non_writable.setdefault(ops[0], set()).add(ops[1])
        elif opcode in (OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_SAMPLED_IMAGE, OP_TYPE_ARRAY,
                        OP_TYPE_RUNTIME_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER,
                        OP_TYPE_ACCELERATION_STRUCTURE):
            types[ops[0]] = (opcode, ops[1:])
            if opcode == OP_TYPE_STRUCT:
                member_counts[ops[0]] = len(ops) - 1
        elif opcode == OP_CONSTANT:
            constants[ops[1]] = ops[2] if len(ops) > 2 else 0
        elif opcode == OP_VARIABLE:
            variables.append((ops[1], ops[0], ops[2]))  # id, pointer type, storage class
        i += count

    resources = []
    for var, pointer, storage in variables:
        dec = decorations.get(var, {})
        if DEC_BINDING not in dec:
            continue
        pointee = types[pointer][1][1]
        array = 1
        while types[pointee][0] in (OP_TYPE_ARRAY, OP_TYPE_RUNTIME_ARRAY):
            opcode, ops = types[pointee]
            array *= constants[ops[1]] if opcode == OP_TYPE_ARRAY else 0
            pointee = ops[0]
        opcode, ops = types[pointee]
        if opcode == OP_TYPE_IMAGE:
            dim, sampled = ops[1], ops[5]
            if dim == DIM_BUFFER:
                kind = "srv_buffer" if sampled == 1 else "uav_buffer"
            else:
                kind = "srv_texture" if sampled == 1 else "uav_texture"
        elif opcode == OP_TYPE_SAMPLED_IMAGE:
            kind = "srv_texture"
        elif opcode == OP_TYPE_SAMPLER:
            kind = "sampler"
        elif opcode == OP_TYPE_ACCELERATION_STRUCTURE:
            kind = "rt_accel"
        elif opcode == OP_TYPE_STRUCT:
            type_dec = decorations.get(pointee, {})
            if storage == SC_UNIFORM and DEC_BLOCK in type_dec:
                kind = "cbv"
            elif storage in (SC_UNIFORM, SC_STORAGE_BUFFER):
                read_only = DEC_NON_WRITABLE in dec or (
                    len(member_non_writable.get(pointee, ())) == member_counts[pointee])
                kind = "srv_buffer" if read_only else "uav_buffer"
            else:
                raise ValueError("struct resource in storage class %d" % storage)
        else:
            raise ValueError("unknown resource type opcode %d" % opcode)
        # Blocks are named by their instance, as FidelityFX-SC names them (cbFI, not cbFI_t).
        name = names.get(var) or names.get(pointee, "")
        if not name:
            raise ValueError("unnamed resource at binding %d" % dec[DEC_BINDING])
        resources.append((kind, name, dec[DEC_BINDING], dec.get(DEC_SET, 0), max(array, 1)))
    resources.sort(key=lambda r: (KINDS.index(r[0]), r[2]))
    return resources


def c_array(ctype, name, values):
    return "static const %s %s[] = { %s };\n" % (ctype, name, ", ".join(values))


def main():
    out_dir, pass_name, options = sys.argv[1], sys.argv[2], sys.argv[3]
    spv_paths = sys.argv[4:]
    options = [] if options == "-" else options.split(",")
    if len(spv_paths) != 1 << len(options):
        raise SystemExit("%s: %d options need %d SPIR-V files, got %d" %
                         (pass_name, len(options), 1 << len(options), len(spv_paths)))

    out = ["// Generated by ffx_permutations.py from %s.glsl. Do not edit.\n" % pass_name,
           "#pragma once\n#include <cstdint>\n\n"]
    infos = []
    for index, path in enumerate(spv_paths):
        with open(path, "rb") as f:
            code = f.read()
        resources = reflect(code)
        prefix = "g_%s_%d" % (pass_name, index)
        out.append("static const unsigned char %s_data[] = {\n" % prefix)
        for start in range(0, len(code), 24):
            out.append("    " + ", ".join("0x%02x" % b for b in code[start:start + 24]) + ",\n")
        out.append("};\n")
        fields = ["%d" % len(code), "%s_data" % prefix]
        for kind in KINDS:
            entries = [r for r in resources if r[0] == kind]
            count_field, stem = FIELDS[kind]
            fields.append("%d" % len(entries))
            if not entries:
                fields += ["nullptr"] * 4
                continue
            base = "%s_%s" % (prefix, kind)
            out.append(c_array("char*", base + "_names", ['"%s"' % e[1] for e in entries]))
            out.append(c_array("uint32_t", base + "_bindings", ["%d" % e[2] for e in entries]))
            out.append(c_array("uint32_t", base + "_counts", ["%d" % e[4] for e in entries]))
            out.append(c_array("uint32_t", base + "_spaces", ["%d" % e[3] for e in entries]))
            fields += [base + "_names", base + "_bindings", base + "_counts", base + "_spaces"]
        infos.append(fields)
        out.append("\n")

    bits = "".join("        uint32_t %s : 1;\n" % option for option in options) or \
        "        uint32_t unused : 1;\n"
    out.append("typedef union %s_PermutationKey {\n    struct {\n%s    };\n    uint32_t index;\n"
               "} %s_PermutationKey;\n\n" % (pass_name, bits, pass_name))
    members = ["const uint32_t blobSize", "const unsigned char* blobData"]
    for kind in KINDS:
        count_field, stem = FIELDS[kind]
        members += ["const uint32_t %s" % count_field, "const char** %sNames" % stem,
                    "const uint32_t* %sBindings" % stem, "const uint32_t* %sCounts" % stem,
                    "const uint32_t* %sSpaces" % stem]
    out.append("typedef struct %s_PermutationInfo {\n%s} %s_PermutationInfo;\n\n" %
               (pass_name, "".join("    %s;\n" % m for m in members), pass_name))
    out.append(c_array("int32_t", "g_%s_IndirectionTable" % pass_name,
                       ["%d" % i for i in range(len(spv_paths))]))
    out.append("static const %s_PermutationInfo g_%s_PermutationInfo[] = {\n" %
               (pass_name, pass_name))
    for fields in infos:
        out.append("    { %s },\n" % ", ".join(fields))
    out.append("};\n")

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "%s_permutations.h" % pass_name), "w") as f:
        f.write("".join(out))
    for variant in ("wave64", "16bit", "wave64_16bit"):
        with open(os.path.join(out_dir, "%s_%s_permutations.h" % (pass_name, variant)), "w") as f:
            f.write("// Generated by ffx_permutations.py: the wave32, 32-bit permutations.\n"
                    "#pragma once\n#include \"%s_permutations.h\"\n"
                    "#define g_%s_%s_IndirectionTable g_%s_IndirectionTable\n"
                    "#define g_%s_%s_PermutationInfo g_%s_PermutationInfo\n" %
                    (pass_name, pass_name, variant, pass_name, pass_name, variant, pass_name))


if __name__ == "__main__":
    main()
