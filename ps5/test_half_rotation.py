#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Execute the translator's HalfPair selector against a bit-exact host IR adapter.

This checks operand/lane ordering, including abs and neg; it is not a GPU oracle.
Only the actual method is extracted. No game inputs or generated fixtures are used.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dotnet", default="dotnet")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = (root / "tools/shaders/ampere/frontend/AmpereEmit.cs").read_text()
    method = source[source.index("        static IrOperand[] HalfPair("):source.index("        static void EmitHsetAmpere(")]
    adapter = r'''
using System;
using System.Collections.Generic;
using IrOperand = System.UInt32;
enum OperandKind { Reg, FImm, Imm }
readonly record struct Op(OperandKind Kind, uint ImmBits, string Suffix, bool Abs, bool Neg);
class EmitterContext {
    public uint UnpackHalf2x16Low(uint v) => BitConverter.SingleToUInt32Bits((float)BitConverter.UInt16BitsToHalf((ushort)v));
    public uint UnpackHalf2x16High(uint v) => UnpackHalf2x16Low(v >> 16);
    public uint FPAbsNeg(uint v, bool abs, bool neg) => (abs ? v & 0x7fffffffU : v) ^ (neg ? 0x80000000U : 0);
}
static class Check {
    static bool HasSuffix(Op op, string suffix) => op.Suffix == suffix;
    static uint Raw(EmitterContext ctx, Op op) => op.ImmBits;
    static uint[] GetHalfSrc(EmitterContext ctx, int v) => [ctx.UnpackHalf2x16Low((uint)v), ctx.UnpackHalf2x16High((uint)v)];
    static uint[] FPAbsNeg(EmitterContext ctx, uint[] v, bool abs, bool neg) => [ctx.FPAbsNeg(v[0],abs,neg),ctx.FPAbsNeg(v[1],abs,neg)];
'''
    checks = r'''
    static void Pair(uint input, bool abs, bool neg, uint low, uint high) {
        int at = 0;
        var actual = HalfPair(new EmitterContext(), new[] {new Op(OperandKind.Reg,input,".H0_NH1",abs,neg)}, ref at);
        if(at != 1 || actual[0] != low || actual[1] != high)
            throw new Exception($"half rotation input={input:x8} abs={abs} neg={neg}: {actual[0]:x8},{actual[1]:x8}");
    }
    static void Main() {
        // Packed inputs (-2,3) and (2,-3). Expected outputs are (-high,low)
        // after source modifiers, expressed as exact float32 bit patterns.
        Pair(0x4200c000, false,false, 0xc0400000,0xc0000000);
        Pair(0x4200c000, true, false, 0xc0400000,0x40000000);
        Pair(0x4200c000, false,true,  0x40400000,0x40000000);
        Pair(0x4200c000, true, true,  0x40400000,0xc0000000);
        Pair(0xc2004000, false,false, 0x40400000,0x40000000);
        Pair(0xc2004000, true, false, 0xc0400000,0x40000000);
        Pair(0xc2004000, false,true,  0xc0400000,0xc0000000);
        Pair(0xc2004000, true, true,  0x40400000,0xc0000000);
        Console.WriteLine("HalfPair: eight signed-lane and modifier cases passed");
    }
}
'''
    with tempfile.TemporaryDirectory(prefix="pokopia-half-selector-") as directory:
        path = Path(directory)
        (path / "test.csproj").write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework></PropertyGroup></Project>')
        (path / "Program.cs").write_text(adapter + method + checks)
        subprocess.run([args.dotnet, "build", str(path / "test.csproj"), "-c", "Release", "--nologo", "--disable-build-servers", "-v:q"], check=True)
        subprocess.run([args.dotnet, str(path / "bin/Release/net10.0/test.dll")], check=True)


if __name__ == "__main__":
    main()
