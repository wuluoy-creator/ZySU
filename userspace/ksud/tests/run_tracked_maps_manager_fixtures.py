"""Generate real C++ tracked Manager fixtures with mbed TLS SHA-256.

Filesystem, clock and ioctl are deterministic models. Android errno values are
explicitly normalized for protocol fixtures; this is not Android execution.
Every output directory must be new. Earlier batch evidence is never modified.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', required=True)
    parser.add_argument('--mbedtls-source', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    source = root / 'userspace/ksud/sumhp/src'
    tests = root / 'userspace/ksud/tests'
    mbed = args.mbedtls_source.resolve()
    assert (mbed/'library/sha256.c').is_file()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    fixtures = output/'fixtures'; fixtures.mkdir()
    fingerprints = {}
    def read(path):
        path = path.resolve(); data = path.read_bytes()
        fingerprints[str(path)] = hashlib.sha256(data).hexdigest()
        return data.decode('utf-8')
    # Bind headers and transitive inline production helpers too, not just the
    # source ranges extracted below. Keep all fixture attempts reproducible.
    for directory in (root/'userspace/common', source, source.parent/'include', mbed/'include', mbed/'library'):
        for path in directory.rglob('*'):
            if path.is_file() and path.suffix in ('.h', '.hpp', '.c', '.cpp'):
                fingerprints[str(path.resolve())] = hashlib.sha256(path.read_bytes()).hexdigest()
    read(Path(__file__))
    commands = []
    def run(command):
        commands.append([str(item) for item in command])
        result = subprocess.run(command, capture_output=True)
        with (output/'build.log').open('ab') as log:
            log.write(result.stdout+result.stderr)
        if result.returncode:
            print((result.stdout+result.stderr).decode('utf-8', errors='replace'))
            raise subprocess.CalledProcessError(result.returncode, command)
        return result
    with tempfile.TemporaryDirectory(prefix='tracked-maps-manager-') as temporary:
        build = Path(temporary)
        def put(name, value): (build/name).write_text(value, encoding='utf-8')
        ledger = read(source/'core/state_ledger.cpp')
        put('state_ledger_under_test.inc', ledger[ledger.index('namespace sumhp::state {'):]
            .replace('std::chrono::steady_clock', 'fixture_clock::Clock'))
        query = read(source/'core/operation_query.cpp')
        put('operation_query_under_test.inc', query[query.index('namespace sumhp::state {'):]
            .replace('std::chrono::steady_clock', 'fixture_clock::Clock')
            .replace('std::this_thread::sleep_for', 'fixture_clock::sleep_for'))
        maps = read(source/'mount/maps_snapshot.cpp')
        put('maps_comparison_under_test.inc', maps[maps.index('MapsComparison compare_maps('):])
        command = read(source/'core/command.cpp'); start=command.index('CommandResult run_command_capture(')
        put('state_capture_under_test.inc', command[start:command.index('\n}  // namespace sumhp', start)])
        client = read(source/'mount/sumh_client.cpp')
        def function(signature):
            start=client.index(signature);return client[start:client.index('\n}',start)+2]
        feature=function('FeatureCapabilities feature_capabilities(').replace('feature_capabilities(', 'tracked_maps_feature_capabilities(')
        deletion=function('bool delete_temporary_maps_rule(').replace('feature_capabilities()', 'tracked_maps_feature_capabilities()')
        put('tracked_maps_client_under_test.inc', function('int execute(unsigned long cmd,')+'\n'+feature+'\n'+deletion)
        embedded=read(source/'embedded.cpp'); start=embedded.index('                state::OperationResponse response;')
        end=embedded.index('\n            });',start)
        put('tracked_maps_manager_dispatch_under_test.inc',embedded[start:end])
        transport=read(root/'userspace/common/maps_preview_transport.cpp');start=transport.index('std::string envelope(')
        put('temporary_maps_envelope_under_test.inc',transport[start:transport.index('\n}',start)+2])
        if os.name=='nt':
            (build/'linux').mkdir();(build/'sys').mkdir()
            put('linux/types.h','#pragma once\n#include <stdint.h>\ntypedef uint32_t __u32;typedef int32_t __s32;\n#define __aligned_u64 uint64_t __attribute__((aligned(8)))\n')
            put('sys/ioctl.h','#pragma once\n#define _IOC(d,t,n,s) (((unsigned long)(d)<<30)|((unsigned long)(s)<<16)|((unsigned long)(t)<<8)|(n))\n#define _IO(t,n) _IOC(0,t,n,0)\n#define _IOR(t,n,s) _IOC(2,t,n,sizeof(s))\n#define _IOW(t,n,s) _IOC(1,t,n,sizeof(s))\n#define _IOWR(t,n,s) _IOC(3,t,n,sizeof(s))\n')
        put('fixture_config.h','#define MBEDTLS_SHA256_C\n#define MBEDTLS_PLATFORM_C\n')
        put('sha_bridge.c','#include <mbedtls/sha256.h>\nint fixture_real_sha256(const unsigned char* p,size_t n,unsigned char* o,int s) {return mbedtls_sha256(p,n,o,s);}\n')
        objects=[]
        for path in (mbed/'library/sha256.c',mbed/'library/platform_util.c',mbed/'library/platform.c',build/'sha_bridge.c'):
            if path.parent!=build:read(path)
            object_file=build/(path.stem+'.o');objects.append(object_file)
            run([args.zig,'cc','-std=c99','-DMBEDTLS_CONFIG_FILE="fixture_config.h"',
                 '-I'+str(build),'-I'+str(mbed/'include'),'-I'+str(mbed/'library'),'-c',str(path),'-o',str(object_file)])
        files=[tests/'tracked_maps_manager_fixture.cpp',source/'core/state_ledger_model.cpp',source/'core/operation_query_input.cpp',
               source/'core/operation_resource_model.cpp',source/'core/user_hide_model.cpp',source/'core/operation_submit.cpp',
               source/'core/operation_submit_input.cpp',root/'userspace/ksud/src/cli_args.cpp',root/'userspace/ksud/src/terminal.cpp']
        for path in files:read(path)
        for path in (tests/'sumh_state_ledger_test.cpp',tests/'tracked_temporary_maps_test.inc',tests/'temporary_maps_android_errno.hpp',
                     source/'core/operation_manager_input.hpp',root/'userspace/common/temporary_maps_command.hpp'):
            read(path)
        binary=build/('tracked_maps_manager_fixture.exe' if os.name=='nt' else 'tracked_maps_manager_fixture')
        run([args.zig,'c++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-missing-field-initializers','-DSUMH_TRACKED_MAPS_NATIVE_FIXTURES',
             '-include',str(tests/'temporary_maps_android_errno.hpp'),'-I'+str(root),'-I'+str(build),'-I'+str(source),
             '-I'+str(source.parent/'include'),'-I'+str(root/'userspace/ksud/src'),*map(str,files),*map(str,objects),'-o',str(binary)])
        run([str(binary),str(fixtures)])
    # Verify every emitted preparation revision against an independent SHA-256.
    for path in fixtures.glob('*-prepare.json'):
        name=path.name.removesuffix('.json')
        request=json.loads((fixtures/(name+'-request.json')).read_text(encoding='utf-8'))
        arguments=json.dumps(request['input']['args'],ensure_ascii=False,separators=(', ', ':'))
        expected='sha256-v1:'+hashlib.sha256(('sumhp-operation-v1\n'+arguments).encode()).hexdigest()
        assert json.loads(path.read_text(encoding='utf-8'))['result']['request_revision']==expected
    changed=[name for name,digest in fingerprints.items() if hashlib.sha256(Path(name).read_bytes()).hexdigest()!=digest]
    assert not changed, changed
    manifest={'passed':True,'device_execution':False,'storage_durability':False,'errno':'synthetic Android/Linux ABI',
              'sha256':'real mbed TLS, empty/abc vectors plus Python hashlib per preparation',
              'commands':commands,'inputs':fingerprints,'outputs':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(fixtures.iterdir())}}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({'passed':True,'fixtures':len(manifest['outputs']),'output':str(output)}))


if __name__=='__main__':main()
