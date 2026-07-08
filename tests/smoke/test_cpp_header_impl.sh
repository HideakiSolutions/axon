#!/usr/bin/env bash
# Smoke: get_callers / get_tests_for must see through the C/C++ header/impl
# split. A symbol *defined* in foo.cpp is consumed by files that include
# foo.hpp — before the definition-surface expansion both tools returned
# empty for exactly this shape (evidence: ccr_store_artifact in this repo
# has 4 call sites and 1 unit test, all importing ccr.hpp; both tools
# reported none).
set -euo pipefail

axon_bin="${1:?axon binary path required}"
case "$axon_bin" in /*) ;; *) axon_bin="$(pwd)/$axon_bin" ;; esac

trap 'echo "[test_cpp_header_impl] FAILED at line $LINENO" >&2' ERR

tmpdir="$(mktemp -d)"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

export HOME="$tmpdir"
sandbox="$tmpdir/cpp-mini"
mkdir -p "$sandbox/.git" "$sandbox/src" "$sandbox/tests"
echo "ref: refs/heads/main" > "$sandbox/.git/HEAD"

cat > "$sandbox/src/foo.hpp" <<'CPP'
#pragma once
int foo_add(int a, int b);
CPP

cat > "$sandbox/src/foo.cpp" <<'CPP'
#include "foo.hpp"
int foo_add(int a, int b) { return a + b; }
CPP

cat > "$sandbox/src/user.cpp" <<'CPP'
#include "foo.hpp"
int use_foo() { return foo_add(1, 2); }
CPP

cat > "$sandbox/tests/test_foo.cpp" <<'CPP'
#include "../src/foo.hpp"
int test_foo_add() { return foo_add(2, 2) == 4 ? 0 : 1; }
CPP

cd "$sandbox"
"$axon_bin" init >/dev/null
"$axon_bin" index >/dev/null 2>&1

response="$( { printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_callers","arguments":{"symbol_name":"foo_add","file_path":"src/foo.cpp"}}}'
               printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_tests_for","arguments":{"files":["src/foo.cpp"]}}}'
             } | "$axon_bin" serve 2>/dev/null )"

callers="$(printf '%s\n' "$response" | head -1)"
tests_for="$(printf '%s\n' "$response" | tail -1)"

printf '%s' "$callers" | grep -q 'src/user.cpp' \
  || { echo "get_callers(foo_add) missed src/user.cpp (header/impl split):" >&2
       printf '%s\n' "$callers" >&2; exit 1; }
printf '%s' "$callers" | grep -q 'tests/test_foo.cpp' \
  || { echo "get_callers(foo_add) missed tests/test_foo.cpp:" >&2
       printf '%s\n' "$callers" >&2; exit 1; }
# The impl importing its own header must not be listed as a caller.
if printf '%s' "$callers" | grep -q '"src/foo.cpp"'; then
  echo "get_callers listed the defining file itself as a caller:" >&2
  printf '%s\n' "$callers" >&2
  exit 1
fi

printf '%s' "$tests_for" | grep -q 'tests/test_foo.cpp' \
  || { echo "get_tests_for(src/foo.cpp) missed tests/test_foo.cpp:" >&2
       printf '%s\n' "$tests_for" >&2; exit 1; }
