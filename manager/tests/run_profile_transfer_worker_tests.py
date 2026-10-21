"""Exercise extracted production JNI scheduling/FD ownership with syscall doubles.

This does not load JNI, grant Android root, or prove real filesystem semantics.
"""
from pathlib import Path
import argparse, os, re, subprocess, tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx', default=os.environ.get('CXX','c++'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    text = (root/'manager/app/src/main/cpp/sumhp_jni.cpp').read_text(encoding='utf-8')
    body = text.split('// PROFILE_TRANSFER_WORKER_BEGIN',1)[1].split('// PROFILE_TRANSFER_WORKER_END',1)[0]
    body = body.replace('struct stat', 'struct ProfileStat')
    for function in ('fcntl','fstat','getuid','geteuid'):
        body = re.sub(r'(?<![\w:])'+function+r'\(', 'model::'+function+'(', body)
    with tempfile.TemporaryDirectory(prefix='profile-jni-worker-') as directory:
        build=Path(directory)
        (build/'profile_transfer_worker_under_test.inc').write_text(body,encoding='utf-8')
        binary=build/('test.exe' if os.name=='nt' else 'test')
        compiler=[args.zig,'c++'] if args.zig else [args.cxx]
        subprocess.run([*compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pthread','-I'+str(build),
            str(root/'manager/tests/profile_transfer_worker_test.cpp'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=10)

if __name__=='__main__':main()
