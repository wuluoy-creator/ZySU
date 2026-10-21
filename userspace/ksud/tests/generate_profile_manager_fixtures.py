"""Generate/check paired production native responses for Kotlin interoperability.

Uses real production profile/controller/store, extracted config/module validators
and SHA provider with locally supplied mbedTLS. Only filesystem/namespace/lock/
durable IO boundaries are doubles; no Android, persistence or runtime evidence.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from run_profile_model_host_tests import extract_function, write_validators


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--mbedtls-source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--check', action='store_true', help='Regenerate in temporary directory and compare; never rewrite fixtures.')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    source = root/'userspace/ksud/sumhp/src'
    mbed = args.mbedtls_source.resolve()
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    tracked = ['userspace/ksud/tests/profile_manager_fixture_generator.cpp',
        'userspace/ksud/tests/generate_profile_manager_fixtures.py',
        'userspace/ksud/tests/run_profile_model_host_tests.py',
        'userspace/ksud/sumhp/src/core/profile.cpp', 'userspace/ksud/sumhp/src/core/profile.hpp',
        'userspace/ksud/sumhp/src/core/profile_input.cpp', 'userspace/ksud/sumhp/src/core/profile_model.cpp',
        'userspace/ksud/sumhp/src/core/profile_store.cpp', 'userspace/ksud/sumhp/src/core/user_hide_model.cpp',
        'userspace/ksud/sumhp/src/core/config.cpp', 'userspace/ksud/sumhp/src/mount/backend.cpp',
        'userspace/ksud/sumhp/src/core/state_ledger.cpp', 'userspace/common/json.hpp', 'userspace/common/utf8.hpp']
    before = {name:sha(root/name) for name in tracked}
    with tempfile.TemporaryDirectory(prefix='profile-native-fixtures-') as temp:
        build=Path(temp); generated=build/'generated'; generated.mkdir()
        if os.name == 'nt':
            (build/'linux').mkdir(); (build/'sys').mkdir()
            (build/'linux/types.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;\ntypedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            (build/'sys/ioctl.h').write_text('#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        write_validators(root,build)
        ledger=(source/'core/state_ledger.cpp').read_text(encoding='utf-8')
        digest=extract_function(ledger,'std::string hex(')+extract_function(ledger,'std::string revision_for_bytes(')
        (build/'profile_real_digest.inc').write_text(digest,encoding='utf-8')
        for part, filename, calls in [('store','profile_store.cpp',('open','close','fstat','flock')),
                ('controller','profile.cpp',('open','close','fstat','setns'))]:
            body=(source/'core'/filename).read_text(encoding='utf-8')
            body=body[body.index('namespace sumhp::profiles {'):]
            for call in calls: body=body.replace('::'+call+'(', 'fixture_io::'+call+'(')
            body=body.replace('struct stat ', 'struct fixture_io::Metadata ')
            (build/('profile_fixture_'+part+'.inc')).write_text(body,encoding='utf-8')
        body=(source/'core/profile_input.cpp').read_text(encoding='utf-8')
        body=body[body.index('namespace sumhp::profiles {'):body.index('bool read_profile_input(')]+'\n}\n'
        (build/'profile_fixture_arguments.inc').write_text(body,encoding='utf-8')
        # Real mbedTLS implementation; minimal host configuration enables SHA256
        # and its platform zeroization, not an alternate/test digest function.
        (build/'fixture_mbedtls_config.h').write_text('#define MBEDTLS_SHA256_C\n#define MBEDTLS_PLATFORM_C\n',encoding='utf-8')
        ccompiler=[args.zig,'cc'] if args.zig else [args.cc]
        cxx=[args.zig,'c++'] if args.zig else [args.cxx]
        includes=['-I'+str(root),'-I'+str(source),'-I'+str(source.parent/'include'),
            '-I'+str(root/'userspace/ksud/src'),'-I'+str(build),'-I'+str(mbed/'include'),'-I'+str(mbed/'library')]
        config=['-DMBEDTLS_CONFIG_FILE="fixture_mbedtls_config.h"']
        objects=[]
        for name in ('sha256','platform_util','platform'):
            obj=build/(name+'.o');objects.append(obj)
            subprocess.run([*ccompiler,'-std=c99','-O1',*includes,*config,'-c',str(mbed/'library'/(name+'.c')),'-o',str(obj)],check=True)
        binary=build/('fixture.exe' if os.name=='nt' else 'fixture')
        subprocess.run([*cxx,'-std=c++17','-O1','-Wall','-Wextra','-Werror',*includes,*config,
            str(root/'userspace/ksud/tests/profile_manager_fixture_generator.cpp'),
            str(source/'core/profile_model.cpp'),str(source/'core/user_hide_model.cpp'),*map(str,objects),'-o',str(binary)],check=True)
        subprocess.run([str(binary),str(generated)],check=True,timeout=60)
        vectors=json.loads((generated/'sha256-vectors.json').read_text(encoding='utf-8'))
        for row in vectors:
            assert row['revision']=='sha256-v1:'+hashlib.sha256(row['input'].encode('utf-8')).hexdigest()
        for name in ('default','unicode'):
            canonical=(generated/(name+'.profile.json')).read_bytes()
            expected='sha256-v1:'+hashlib.sha256(b'sumhp-profile-document-v1\n'+canonical).hexdigest()
            assert (generated/(name+'.revision.txt')).read_text(encoding='utf-8')==expected
        # Every profile-bearing native response must bind exactly to the real
        # canonical document digest; no hand-built response fixture is accepted.
        pairs=json.loads((generated/'index.json').read_text(encoding='utf-8'))
        for pair in pairs:
            response=json.loads((generated/pair['response_file']).read_text(encoding='utf-8'))
            if 'profile' in response:
                native_name='default' if pair['name']=='show-default' else 'unicode'
                assert response['profile_revision']==(generated/(native_name+'.revision.txt')).read_text(encoding='utf-8')
        after={name:sha(root/name) for name in tracked}
        assert before==after, 'Production sources changed during fixture generation'
        fixture_hashes={path.name:sha(path) for path in sorted(generated.iterdir())}
        manifest={'schema_version':1,'kind':'sumh.profile.native.fixtures','pairs':len(pairs),
            'production_sources':before,'mbedtls_sources':{name:sha(mbed/name) for name in ('library/sha256.c','library/platform_util.c','library/platform.c','include/mbedtls/sha256.h')},
            'sha256_python_verified_vectors':len(vectors),'profile_revisions_python_verified':2,
            'boundaries':'Filesystem, namespace, locks and durable writes are doubles; production parser, controller, store, diff and real mbedTLS SHA256 execute.',
            'files':fixture_hashes}
        (generated/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
        if args.check:
            missing=[p.name for p in generated.iterdir() if not (args.output/p.name).is_file() or (args.output/p.name).read_bytes()!=p.read_bytes()]
            if missing: raise SystemExit('Fixture mismatch: '+', '.join(sorted(missing)))
            print('Production native fixtures reproduce exactly; independent Python SHA256 checks PASS.')
        else:
            args.output.mkdir(parents=True,exist_ok=True)
            for path in generated.iterdir(): shutil.copyfile(path,args.output/path.name)
            print('Generated paired fixtures; independent Python SHA256 checks PASS: '+str(args.output))


if __name__=='__main__':main()
