"""Reject mixing two spaces for the intended compile-time diagnostic."""
import argparse
import pathlib
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--compiler', required=True)
p.add_argument('--id', required=True)
p.add_argument('--include', required=True, action='append')
p.add_argument('--source', required=True)
p.add_argument('--generator', default='Visual Studio 18 2026')
p.add_argument('--platform', default='x64')
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='tele-compile-fail-') as tmp:
    if a.id == 'MSVC':
        # Visual Studio supplies the STL/SDK environment through its generator;
        # ctest need not be started from a Developer Command Prompt.
        source = pathlib.Path(a.source).as_posix()
        include = " ".join("\"" + pathlib.Path(entry).as_posix() + "\"" for entry in a.include)
        pathlib.Path(tmp, 'CMakeLists.txt').write_text(
            'cmake_minimum_required(VERSION 3.16)\nproject(negative LANGUAGES CXX)\n'
            'set(CMAKE_CXX_STANDARD 20)\nadd_library(negative OBJECT "' + source + '")\n'
            'target_include_directories(negative PRIVATE ' + include + ')\n')
        configure = subprocess.run(['cmake', '-S', tmp, '-B', tmp + '/build',
                                    '-G', a.generator, '-A', a.platform],
                                   capture_output=True, text=True)
        if configure.returncode:
            raise SystemExit(configure.stdout + configure.stderr)
        command = ['cmake', '--build', tmp + '/build', '--config', 'Release']
    else:
        command = [a.compiler, '-std=c++20', *('-I' + entry for entry in a.include), '-c', a.source,
                   '-o', str(pathlib.Path(tmp) / 'test.o')]
    result = subprocess.run(command, capture_output=True, text=True)
    output = result.stdout + result.stderr
    if result.returncode == 0 or 'telemetry tag belongs to another space' not in output:
        raise SystemExit('Expected space ownership error, got:\n' + output)
    print('Wrong-space tag rejected as expected')
