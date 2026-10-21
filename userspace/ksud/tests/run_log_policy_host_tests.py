"""Exercise production repeat aggregation and detached daemon descriptor setup."""
import argparse, os, subprocess, tempfile
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig')
    parser.add_argument('--cxx',default=os.environ.get('CXX','c++'))
    args=parser.parse_args()
    ksud=Path(__file__).resolve().parents[1]
    compiler=[args.zig,'c++'] if args.zig else [args.cxx]
    with tempfile.TemporaryDirectory(prefix='sumhp-log-policy-') as tmp:
        build=Path(tmp)
        source=(ksud/'sumhp/src/core/daemon.cpp').read_text(encoding='utf-8')
        start=source.index('bool promote_background_pipe(')
        section=source[start:source.index('\nvoid startup_error(',start)]
        for name in ('open','fcntl','close','dup2'):
            section=section.replace(name+'(', 'fixture::'+name+'(')
        (build/'log_daemon_stdio_under_test.inc').write_text(section,encoding='utf-8')
        utils=(ksud/'src/utils.cpp').read_text(encoding='utf-8')
        start=utils.index('bool reset_stdio_to_devnull(')
        reset=utils[start:utils.index('\nProcessDaemonizeResult daemonize_process(', start)]
        for name in ('open','close','dup2'):
            reset=reset.replace(name+'(', 'fixture::'+name+'(')
        (build/'log_daemon_reset_under_test.inc').write_text(reset,encoding='utf-8')
        # Guard the production caller and startup diagnostic routing, in addition
        # to the extracted function's descriptor fault injection.
        background=source[source.index('int start_background('):source.index('\n}  // namespace',source.index('int start_background('))]
        if ('if (!detach_background_stdio())' not in background or
                'open(runtime_log_file()' in background or 'dup2(log_fd' in background or
                'logging::flush();' not in source):
            raise ValueError('production background stdio/flush routing changed; review extraction')
        if background.index('if (!promote_background_pipe(ready_pipe))') > background.index('ksud::daemonize_process(false)'):
            raise ValueError('readiness promotion must precede daemonization')
        for name,extra in [('log_repeat', [ksud/'sumhp/src/core/log_repeat.cpp']), ('log_daemon_stdio',[])]:
            binary=build/(name+('.exe' if os.name=='nt' else ''))
            command=[*compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                     '-I'+str(ksud/'sumhp/src'),'-I'+str(build),
                     str(ksud/'tests'/f'{name}_test.cpp'),*[str(p) for p in extra],'-o',str(binary)]
            subprocess.run(command,check=True)
            subprocess.run([str(binary)],check=True,timeout=30)

if __name__=='__main__':main()
