p = 'app/src/main/jni/Main.cpp'
s = open(p).read()
decl = '''bool IsCraftFree(void*, void*, void*);
bool IsLongUnlockFree(void*, void*, void*);
bool IsCharProtected(void*, void*, void*);
bool IsSkillFree(void*, void*, void*);
extern bool (*old_IsCraftFree)(void*, void*, void*);
extern bool (*old_IsLongUnlockFree)(void*, void*, void*);
extern bool (*old_IsCharProtected)(void*, void*, void*);
extern bool (*old_IsSkillFree)(void*, void*, void*);
static bool h0 = false, h1 = false, h2 = false, h3 = false;

'''
m = 'void Changes('
assert m in s, 'Changes tidak ketemu'
s = s.replace(m, decl + m, 1)
R = {
 'case 0: fCraft = boolean; break;': 'case 0: fCraft = boolean; if (boolean && !h0) { h0 = true; HOOK(targetLibName, "0x265CEFC", IsCraftFree, old_IsCraftFree); } break;',
 'case 1: fUnlock = boolean; break;': 'case 1: fUnlock = boolean; if (boolean && !h1) { h1 = true; HOOK(targetLibName, "0x265CF80", IsLongUnlockFree, old_IsLongUnlockFree); } break;',
 'case 2: fProtect = boolean; break;': 'case 2: fProtect = boolean; if (boolean && !h2) { h2 = true; HOOK(targetLibName, "0x2659004", IsCharProtected, old_IsCharProtected); } break;',
 'case 3: fSkill = boolean; break;': 'case 3: fSkill = boolean; if (boolean && !h3) { h3 = true; HOOK(targetLibName, "0x265D054", IsSkillFree, old_IsSkillFree); } break;',
}
for a, b in R.items():
    assert a in s, a
    s = s.replace(a, b, 1)
open(p, 'w').write(s)
print('selesai')
