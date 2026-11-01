p = 'app/src/main/jni/Main.cpp'
s = open(p).read()
if 'ENABLE_HOOKS' in s:
    print('sudah dipatch')
    raise SystemExit
s = s.replace('#include "dobby.h"\n', '#include "dobby.h"\n\n#define ENABLE_HOOKS 0\n', 1)
a = '#if defined(__aarch64__)\n    HOOK('
assert a in s, 'blok hook tidak ketemu'
s = s.replace(a, '#if defined(__aarch64__) && ENABLE_HOOKS\n    HOOK(', 1)
open(p, 'w').write(s)
print('selesai')
