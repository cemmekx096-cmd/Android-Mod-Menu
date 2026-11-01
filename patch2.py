p = 'app/src/main/jni/Menu/Jni.cpp'
s = open(p).read()
i = s.index('void Dialog(')
j = s.index('{\n', i) + 2
if 'dialog dimatikan' in s:
    print('sudah dipatch')
else:
    s = s[:j] + '    return; // dialog dimatikan: context Service tidak punya window token\n' + s[j:]
    open(p, 'w').write(s)
    print('selesai')
