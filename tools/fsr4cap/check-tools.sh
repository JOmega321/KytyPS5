#!/usr/bin/env bash
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
bin=$here/prebuilt/bin
[[ -f $bin/fsr4cap.exe ]] || { echo 'Bundled capture executable missing' >&2; exit 4; }
"$bin/kyty-python3" - <<'PY'
import hashlib, json, math, os, re, struct, subprocess, sys, ssl, ctypes
from pathlib import Path
exe = Path(sys.prefix).parent / 'bin/fsr4cap.exe'
assert exe.read_bytes()[:2] == b'MZ', 'invalid capture executable'
assert hashlib.sha256(b'Kyty').hexdigest()
print('Bundled Python, standard library and capture executable: OK')
PY
"$bin/dxil-spirv" --help > /dev/null
"$bin/spirv-as" --version
"$bin/spirv-dis" --version
"$bin/flock" --version
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
"$bin/kyty-python3" - "$work/input.spvasm" <<'PY'
import sys
from pathlib import Path
Path(sys.argv[1]).write_text('''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%main = OpFunction %void None %fn
%label = OpLabel
OpReturn
OpFunctionEnd
''')
PY
"$bin/spirv-as" "$work/input.spvasm" -o "$work/test.spv"
"$bin/spirv-dis" "$work/test.spv" -o "$work/output.spvasm"
"$bin/flock" -n "$work/check.lock" true
printf '%s\n' 'Bundled translator and SPIR-V round trip: OK' 'No host Python, compiler, CMake, Ninja, Git, or SPIR-V tools required.'
