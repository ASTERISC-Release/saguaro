#!/usr/bin/env python3
# Copyright (C) 2026 The ASTERISC Research Lab
# Non-regression check of the x86_64 VBS work for AArch64 without an aarch64 toolchain: preprocesses
# every vbs PAL source for aarch64 (clang -E -P --target=aarch64-linux-gnu) in a base tree and in this
# tree, with the include paths of an x86_64 vbs build (build-vbs-x64/compile_commands.json, arch
# directories mapped to aarch64), and reports any difference. Usage (from the checkout root, with the
# base tree unpacked by `git archive origin/main | tar -x -C /tmp/oldtree`):
#   aarch64-pp-compare.py [OLD_TREE] [PATH_PATTERN]
import json, shlex, subprocess, os, sys
NEW=os.getcwd(); OLD=sys.argv[1] if len(sys.argv) > 1 else '/tmp/oldtree'; CB='/root/gramine-multiarch'
BUILD=os.path.join(NEW,'build-vbs-x64')
res=subprocess.check_output(['clang','-print-resource-dir'],text=True).strip()+'/include'
cc=json.load(open(os.path.join(BUILD,'compile_commands.json')))
pat=sys.argv[2] if len(sys.argv) > 2 else '/pal/src/host/vbs/'
ents=[e for e in cc if pat in e['file'] and e['file'].endswith('.c') and '/arch/' not in e['file']]
def mapinc(p, root):
    ab=os.path.normpath(os.path.join(CB+'/build-vbs-x64', p)) if not p.startswith('/') else p
    ab=ab.replace('arch/x86_64','arch/aarch64')
    if ab.startswith(CB+'/build-vbs-x64'):
        return BUILD+ab[len(CB+'/build-vbs-x64'):]
    if ab.startswith(CB+'/subprojects'): return NEW+ab[len(CB):]
    return root+ab[len(CB):] if ab.startswith(CB) else ab
bad=0
for e in ents:
    args=shlex.split(e['command']); keep=[]; it=iter(args[1:])
    for a in it:
        if a in ('-o','-MF','-MQ','-MT'): next(it); continue
        if a=='-c' or a.startswith(('-m','-fstack-protector','-fcf-protection','-W','-g','-O','-MD','-f')): continue
        if not a.startswith('-'): continue
        keep.append(a)
    rel=os.path.relpath(os.path.normpath(os.path.join(CB+'/build-vbs-x64', e['file'])), CB)
    outs=[]
    for root in (OLD, NEW):
        incs=[]
        for k in keep:
            if k.startswith('-I'): incs.append('-I'+mapinc(k[2:], root))
            elif k.startswith('-isystem'): incs.append(k)
            else: incs.append(k)
        cmd=['clang','--target=aarch64-linux-gnu','-E','-P','-nostdinc','-isystem',res,'-idirafter','/usr/include']+incs+[os.path.join(root,rel)]
        r=subprocess.run(cmd,capture_output=True,text=True,cwd=BUILD)
        outs.append(r.stdout if r.returncode==0 else 'ERR '+r.stderr[-300:])
        open('/tmp/pp_'+('old' if root==OLD else 'new')+'_'+os.path.basename(rel)+'.i','w').write(outs[-1])
    same=outs[0]==outs[1] and not outs[0].startswith('ERR')
    bad+= not same
    print(('same ' if same else 'DIFF ')+rel+('' if same else ' '+(outs[0] if outs[0].startswith('ERR') else outs[1] if outs[1].startswith('ERR') else '(text differs)')[:300]))
print('sources:',len(ents),'differences:',bad)
