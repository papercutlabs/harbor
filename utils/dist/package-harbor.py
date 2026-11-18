#!/usr/bin/env python3
"""Package the actual HarborRuntime install tree; emit Office's exact asset pin."""
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tarfile


def package(stage, output, revision, version, target='macos-arm64'):
    stage, output = pathlib.Path(stage), pathlib.Path(output)
    if not re.fullmatch('[0-9a-f]{40}', revision) or not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise ValueError('Expected exact source revision and native Harbor version')
    if target != 'macos-arm64':
        raise ValueError('Unsupported Harbor target')
    binary = stage / 'bin/harbor'
    required = [binary, stage / 'share/doc/harbor/LICENSE', stage / 'share/doc/harbor/THIRD_PARTY_LICENSES', stage / 'share/harbor/magic.mgc', stage / 'bin/ffmpeg', stage / 'share/doc/harbor/ffmpeg/source.tar.xz', stage / 'share/doc/harbor/ffmpeg/COPYING.LGPLv2.1']
    if any(not path.is_file() or path.is_symlink() for path in required):
        raise ValueError('HarborRuntime is incomplete (actual bin/harbor and licenses required)')
    for path in stage.rglob('*'):
        if path.is_symlink() or not (path.is_file() or path.is_dir()):
            raise ValueError('HarborRuntime contains a link or special entry')
    manifest = dict(schema=1, revision=revision, version=version, target=target,
                    capabilities=dict(telegramRouting=1),
                    executableSha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
                    mediaSha256=hashlib.sha256((stage / 'bin/ffmpeg').read_bytes()).hexdigest())
    (stage / 'manifest.json').write_text(json.dumps(manifest) + '\n')
    (stage / 'share/harbor').mkdir(parents=True, exist_ok=True)
    (stage / 'share/harbor/version').write_text(version + '\n')
    name = f'harbor-{version}-{revision}-{target}.tar.gz'
    tag = f'harbor-{version}-{revision}'
    output.mkdir(parents=True, exist_ok=True)
    archive = output / name
    # USTAR, regular files and directories only: no PAX/GNU extensions or links needed by the consumer.
    with tarfile.open(archive, 'w:gz', format=tarfile.USTAR_FORMAT) as tar:
        tar.add(stage, arcname='harbor-runtime')
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    pin = dict(revision=revision, version=version, telegramRouting=1, artifacts={target: dict(tag=tag, asset=name, sha256=digest)})
    (output / 'harbor-release.json').write_text(json.dumps(pin, indent=2) + '\n')
    (output / (name + '.sha256')).write_text(digest + '  ' + name + '\n')
    return pin


if __name__ == '__main__':
    repo = pathlib.Path(__file__).resolve().parents[2]
    revision = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    if subprocess.check_output(['git', '-C', str(repo), 'status', '--porcelain', '--untracked-files=no'], text=True).strip():
        raise SystemExit('Harbor source has tracked edits; commit before packaging')
    # Native output is the authority for version; do not guess from a tag or an upstream header.
    native = subprocess.check_output([str(pathlib.Path(sys.argv[1]).resolve() / 'bin/harbor'), '--version'], text=True)
    match = re.match(r'^Harbor (\d+\.\d+\.\d+)(?:\s|$)', native)
    if not match:
        raise SystemExit('Staged executable is not Harbor')
    pin = package(sys.argv[1], sys.argv[2], revision, match[1])
    print(json.dumps(pin, indent=2))
