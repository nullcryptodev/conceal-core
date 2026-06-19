#!/usr/bin/env bash
# Local C++11 drift checker.
# Scans src/ for common C++17 constructs using ripgrep and optional clang++.
#
# Usage:
#   ./tools/check-cpp11-drift.sh
#   ./tools/check-cpp11-drift.sh --clang    # also run clang -Wc++17-extensions
#
# For the clang pass, generate compile_commands.json first:
#   cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#   ln -sf build/compile_commands.json compile_commands.json

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="${ROOT}/src"
TESTS="${ROOT}/tests"
RUN_CLANG=0
FAILED=0

usage() {
  sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --clang) RUN_CLANG=1; shift ;;
    -h|--help) usage 0 ;;
    *) echo "Unknown option: $1" >&2; usage 1 ;;
  esac
done

if [[ ! -d "$SRC" ]]; then
  echo "ERROR: src/ not found at ${SRC}" >&2
  exit 1
fi

if ! command -v rg >/dev/null 2>&1; then
  echo "ERROR: ripgrep (rg) is required." >&2
  exit 1
fi

echo "== C++11 drift check (project standard: C++11) =="
echo "Scanning: ${SRC} and ${TESTS}"
echo

# label|regex|note
PATTERNS=(
  'structured bindings|auto &\[|C++17 structured bindings'
  'if-init (const)|if \(const [^)]+;|C++17 init-statement in if'
  'if-init (auto)|if \(auto [^)]+;|C++17 init-statement in if'
  'std::optional|std::optional<|C++17 std::optional'
  'std::variant|std::variant<|C++17 std::variant'
  'std::string_view|std::string_view|C++17 std::string_view'
  'std::filesystem|std::filesystem::|C++17 std::filesystem'
  'if constexpr|if constexpr|C++17 if constexpr'
  'nested namespace|^\s*namespace\s+[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)+\s*\{|C++17 nested namespace definition'
  'inline static member|inline static |C++17 inline variables'
)

echo "-- Static pattern scan (rg) --"
for entry in "${PATTERNS[@]}"; do
  IFS='|' read -r label regex note <<<"$entry"
  matches="$(rg -n --glob '*.{cpp,h,hpp,hxx,c++}' --glob '!**/external/**' "$regex" "$SRC" "$TESTS" 2>/dev/null || true)"
  if [[ -n "$matches" ]]; then
    echo "FAIL [${label}] (${note})"
    echo "$matches"
    echo
    FAILED=1
  fi
done

if [[ $FAILED -eq 0 ]]; then
  echo "OK: no suspicious C++17 patterns found by rg."
else
  echo "rg found possible C++17 drift (review matches above)."
fi

if [[ $RUN_CLANG -eq 0 ]]; then
  echo
  echo "Tip: run with --clang for a compile_commands.json-based clang scan."
  exit "$FAILED"
fi

echo
echo "-- Clang extension scan (-std=c++11 -Wc++17-extensions) --"

if ! command -v clang++ >/dev/null 2>&1; then
  echo "SKIP: clang++ not found." >&2
  exit "$FAILED"
fi

COMPILE_DB="${ROOT}/compile_commands.json"
if [[ ! -f "$COMPILE_DB" ]]; then
  echo "SKIP: ${COMPILE_DB} not found." >&2
  echo "Generate it with:" >&2
  echo "  cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON" >&2
  echo "  ln -sf build/compile_commands.json compile_commands.json" >&2
  exit "$FAILED"
fi

CLANG_FAILED=0
export ROOT COMPILE_DB CLANG_FAILED

python3 - <<'PY'
import json
import os
import shlex
import subprocess
import sys

root = os.environ["ROOT"]
db_path = os.environ["COMPILE_DB"]
clang_failed = 0

with open(db_path, encoding="utf-8") as f:
    db = json.load(f)

src_root = os.path.join(root, "src") + os.sep
seen = set()
files = []

for entry in db:
    path = os.path.normpath(entry["file"])
    if not path.startswith(src_root):
        continue
    if not path.endswith((".cpp", ".c++", ".cxx", ".cc")):
        continue
    if path in seen:
        continue
    seen.add(path)
    files.append((path, entry))

files.sort()
if not files:
    print("SKIP: no src/*.cpp entries in compile_commands.json")
    sys.exit(0)

print(f"Checking {len(files)} translation unit(s) with clang++ ...")

def build_command(entry):
    if "arguments" in entry:
        args = list(entry["arguments"])
    else:
        args = shlex.split(entry["command"])

    out = []
    skip_next = False
    for i, arg in enumerate(args):
        if skip_next:
            skip_next = False
            continue
        if arg in ("-c", "-o", "-M", "-MM", "-MF", "-MT", "-MQ"):
            skip_next = True
            continue
        if arg.endswith((".o", ".obj")):
            continue
        out.append(arg)

    compiler = out[0] if out else "clang++"
    if "clang" not in os.path.basename(compiler):
        out[0] = "clang++"

    extra = ["-std=c++11", "-Wc++17-extensions", "-Werror=c++17-extensions", "-fsyntax-only"]
    if "-std=" not in " ".join(out):
        out[1:1] = extra
    else:
        out.extend(["-Wc++17-extensions", "-Werror=c++17-extensions", "-fsyntax-only"])

    out.append(entry["file"])
    return out

for path, entry in files:
    cmd = build_command(entry)
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        clang_failed = 1
        print(f"FAIL: {path}")
        err = proc.stderr.strip() or proc.stdout.strip()
        for line in err.splitlines()[:12]:
            print(f"  {line}")
        if err.count("\n") > 12:
            print("  ...")
        print()

if clang_failed:
    print("clang found C++17 extensions under -std=c++11.")
else:
    print("OK: clang found no C++17 extensions in checked translation units.")

sys.exit(clang_failed)
PY

CLANG_EXIT=$?
if [[ $CLANG_EXIT -ne 0 ]]; then
  FAILED=1
fi

exit "$FAILED"
