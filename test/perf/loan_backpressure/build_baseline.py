#!/usr/bin/env python3
"""从指定 Git 版本构建匹配源码和头文件的性能基线，保存编译身份。"""
import argparse
import hashlib
import json
import pathlib
import subprocess


def digest(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--revision', default='HEAD')
    parser.add_argument('--directory', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--measurement-only', action='store_true', help='复用已存证的旧库，仅重编统一测量程序')
    parser.add_argument('--library-identity', help='复用旧库时提供原始构建身份文件')
    parser.add_argument('--binary-name', default='loan_phase_latency_baseline')
    args = parser.parse_args()
    repo = pathlib.Path(__file__).resolve().parents[3]
    destination = pathlib.Path(args.directory).resolve()
    out = pathlib.Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    revision = subprocess.check_output(['git', 'rev-parse', args.revision], cwd=repo, text=True).strip()
    build = destination / 'build'
    library = build / 'lib/libipc.so.1.6.0'
    if args.measurement_only:
        previous = json.loads(pathlib.Path(args.library_identity or out / 'baseline_identity.json').read_text())
        if previous['revision'] != revision or previous['directory'] != str(destination) or \
                previous['library_sha256'] != digest(library):
            raise RuntimeError('复用基线的版本或库身份不一致')
    else:
        destination.mkdir(parents=True, exist_ok=False)
        archive = subprocess.Popen(['git', 'archive', revision], cwd=repo, stdout=subprocess.PIPE)
        try:
            subprocess.run(['tar', '-x', '-C', str(destination)], stdin=archive.stdout, check=True)
        finally:
            archive.stdout.close()
        if archive.wait() != 0:
            raise RuntimeError('导出修改前源码失败')
    commands = [
        ['cmake', '-S', str(destination), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release',
         '-DLIBIPC_BUILD_TESTS=OFF', '-DLIBIPC_BUILD_PYTHON=OFF', '-DUPDATA_MSG_SRV_GENERATOR=ON',
         '-DDZIPC_BUILD_SHARED_NET=ON'],
        ['cmake', '--build', str(build), '--target', 'ipc', f'-j{args.jobs}'],
    ]
    source = repo / 'test/loan_phase_latency_benchmark.cpp'
    binary = build / 'bin' / args.binary_name
    commands.append([
        '/usr/bin/c++', '-std=c++17', '-O3', '-DNDEBUG', '-O2', '-fPIE',
        '-DLIBIPC_LIBRARY_SHARED_USING__', '-DDZIPC_LOAN_PERF_BASELINE=1',
        '-DUNICODE', '-D_UNICODE', '-Wno-attributes',
        '-I' + str(destination / 'include'), '-I' + str(destination / 'src'),
        str(source), '-L' + str(build / 'lib'), '-Wl,-rpath,' + str(build / 'lib'),
        '-lipc', '-pthread', '-lrt', '-o', str(binary),
    ])
    with (out / 'baseline_build.txt').open('a' if args.measurement_only else 'w') as log:
        for command in commands[-1:] if args.measurement_only else commands:
            log.write(json.dumps(command, ensure_ascii=False) + '\n')
            log.flush()
            result = subprocess.run(command, cwd=repo, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f'基线构建失败，见 {out / "baseline_build.txt"}')
    identity = {
        'revision': revision, 'directory': str(destination), 'binary': str(binary),
        'commands': commands, 'compiler': subprocess.check_output(['/usr/bin/c++', '--version'], text=True),
        'measurement_source_sha256': digest(source), 'binary_sha256': digest(binary),
        'library_sha256': digest(library), 'library': str(library),
        'include_roots': [str(destination / 'include'), str(destination / 'src')],
        'library_flags': (build / 'src/CMakeFiles/ipc.dir/flags.make').read_text(),
        'adapter': 'raw 使用同一 loan(size, reason) 与 try_publish_loan；旧库无有界 loan，仅池耗尽时进行带截止时间的 yield 重试。DZFlat object 使用原有 publish_blocking，loaned 使用同一 B 级 loan/publish_loaned API 并有界重试；两种入口必须分别验收且均核对无 TLV 回退。',
    }
    (out / 'baseline_identity.json').write_text(json.dumps(identity, indent=2, ensure_ascii=False) + '\n')
    print(f'基线构建完成：{binary}', flush=True)


if __name__ == '__main__':
    main()
