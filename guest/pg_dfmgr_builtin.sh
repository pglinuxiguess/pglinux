#!/bin/sh
# pg_dfmgr_builtin.sh — Patch dfmgr.c for built-in extension support.
#
# Adds two hooks using sed:
# 1. In load_external_function: check pg_builtin_lookup() before stat/dlopen
# 2. In load_file: skip file loading for built-in modules

DFMGR="$1/src/backend/utils/fmgr/dfmgr.c"

if [ ! -f "$DFMGR" ]; then
    echo "ERROR: $DFMGR not found"
    exit 1
fi

# Use awk to patch load_external_function:
# Find the opening { of load_external_function and insert builtin check after it.
# Also patch load_file to skip builtin modules.
awk '
/^load_external_function\(/ { in_lef = 1 }
in_lef && /^{/ {
    print
    print "\t/* Static build: check built-in extension table first */"
    print "\t{ extern void *pg_builtin_lookup(const char *, const char *);"
    print "\t  extern void *PG_BUILTIN_HANDLE;"
    print "\t  const char *_bn = filename;"
    print "\t  if (strncmp(_bn, \"$libdir/\", 8) == 0) {"
    print "\t    const char *_sl = first_dir_separator(_bn + 8);"
    print "\t    if (!_sl) _bn += 8;"
    print "\t  }"
    print "\t  void *_ret = pg_builtin_lookup(_bn, funcname);"
    print "\t  if (_ret) { if (filehandle) *filehandle = PG_BUILTIN_HANDLE; return _ret; } }"
    in_lef = 0
    next
}
/^load_file\(/ { in_lf = 1 }
in_lf && /fullname = expand_dynamic_library_name/ {
    print "\t/* Static build: skip file loading for built-in modules */"
    print "\t{ extern bool pg_builtin_is_module(const char *);"
    print "\t  const char *_bn2 = filename;"
    print "\t  if (strncmp(_bn2, \"$libdir/\", 8) == 0) {"
    print "\t    const char *_sl2 = first_dir_separator(_bn2 + 8);"
    print "\t    if (!_sl2) _bn2 += 8;"
    print "\t  }"
    print "\t  if (pg_builtin_is_module(_bn2)) return; }"
    print ""
    in_lf = 0
}
/^lookup_external_function/ { in_lookup = 1 }
in_lookup && /return dlsym/ {
    print "\t/* Static build: check builtin registry for builtin modules */"
    print "\textern void *PG_BUILTIN_HANDLE;"
    print "\textern void *pg_builtin_lookup_sym(const char *);"
    print "\tif (filehandle == PG_BUILTIN_HANDLE) return pg_builtin_lookup_sym(funcname);"
    in_lookup = 0
}
{ print }
' "$DFMGR" > "${DFMGR}.patched" && mv "${DFMGR}.patched" "$DFMGR"

echo "dfmgr.c patched successfully"
