"""Run production package CLI orchestration with controlled OS/store/query seams."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx',default=os.environ.get('CXX','c++'))
    args=parser.parse_args()
    root=Path(__file__).resolve().parents[3]
    ksud=root/'userspace/ksud';source=ksud/'sumhp/src'
    body=(source/'core/diagnostic_package.cpp').read_text(encoding='utf-8')
    body=body[body.index('namespace sumhp::diagnostic_package {'):]
    for name in ('open','close','fstat','stat','setns'):
        body=body.replace('::'+name+'(','package_os::'+name+'(')
    body=body.replace('struct stat','struct package_os::Metadata')
    cli=(ksud/'src/cli.cpp').read_text(encoding='utf-8')
    embedded=(source/'embedded.cpp').read_text(encoding='utf-8')
    dispatch=(source/'core/command.cpp').read_text(encoding='utf-8')
    grammar=(ksud/'src/cli_args.cpp').read_text(encoding='utf-8')
    if not (cli.index('const bool diagnostics =') < cli.index('log_init("KernelSU")') and
            'diagnostic_package::command(args)' in embedded and
            embedded.index('diagnostic_package::command(args)') < embedded.index('diagnostics::parse_channel_arguments') and
            embedded.index('diagnostic_package_manager::parse_channel_arguments') < embedded.index('diagnostic_package::command(args)') and
            'diagnostic_package::manager_dispatch(request)' in embedded and
            'preview_transport::Service::DiagnosticPackages' in embedded and
            'if (!args.empty() && args[0] == "doctor")' in dispatch and
            'current->path.compare(0, 13, "sumhp doctor ") == 0' in grammar):
        raise RuntimeError('Exact local package routing changed; review grammar/dispatch')
    with tempfile.TemporaryDirectory(prefix='diagnostic-package-controller-') as tmp:
        build=Path(tmp)
        (build/'diagnostic_package_controller_under_test.inc').write_text(body,encoding='utf-8')
        binary=build/('controller.exe' if os.name=='nt' else 'controller')
        compiler=[args.zig,'c++'] if args.zig else [args.cxx]
        subprocess.run([*compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                        '-I'+str(root),'-I'+str(source),'-I'+str(build),
                        str(ksud/'tests/diagnostic_package_controller_test.cpp'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=30)

if __name__=='__main__':main()
