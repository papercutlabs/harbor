"""Packaging contract fixtures. These do not claim a native Harbor build passed."""
import hashlib
import importlib.util
import json
import pathlib
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('package_harbor', pathlib.Path(__file__).resolve().parents[2] / 'utils/dist/package-harbor.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PackageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = pathlib.Path(self.tmp.name)
        self.stage = self.root / 'stage'
        for name, text in [('bin/harbor', 'fixture Harbor executable'), ('share/doc/harbor/LICENSE', 'license'), ('share/doc/harbor/THIRD_PARTY_LICENSES', 'notices'), ('share/harbor/demo/sample.txt', 'resource'), ('share/harbor/magic.mgc', 'fixture magic data'), ('bin/ffmpeg', 'fixture media executable'), ('share/doc/harbor/ffmpeg/source.tar.xz', 'fixture source'), ('share/doc/harbor/ffmpeg/COPYING.LGPLv2.1', 'fixture license')]:
            path = self.stage / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
        (self.stage / 'bin/harbor').chmod(0o755)

    def test_actual_harbor_layout_and_exact_pin(self):
        pin = module.package(self.stage, self.root, 'a' * 40, '0.1.19')
        asset = pin['artifacts']['macos-arm64']
        archive = self.root / asset['asset']
        self.assertEqual(asset['sha256'], hashlib.sha256(archive.read_bytes()).hexdigest())
        with tarfile.open(archive) as tar:
            self.assertIn('harbor-runtime/bin/harbor', tar.getnames())
            self.assertIn('harbor-runtime/share/harbor/demo/sample.txt', tar.getnames())
            manifest = json.load(tar.extractfile('harbor-runtime/manifest.json'))
            self.assertEqual(manifest['capabilities'], {'telegramRouting': 1, 'targetSurface': 1})
            self.assertEqual(pin['telegramRouting'], 1)
            self.assertEqual(pin['targetSurface'], 1)
            self.assertEqual(manifest['revision'], pin['revision'])
            self.assertEqual(manifest['version'], pin['version'])
            self.assertEqual(manifest['target'], 'macos-arm64')
            self.assertEqual(manifest['executableSha256'], hashlib.sha256((self.stage / 'bin/harbor').read_bytes()).hexdigest())
            self.assertTrue(all(member.isfile() or member.isdir() for member in tar))

    def test_upstream_nchat_does_not_satisfy_harbor(self):
        (self.stage / 'bin/harbor').rename(self.stage / 'bin/nchat')
        with self.assertRaisesRegex(ValueError, 'actual bin/harbor'):
            module.package(self.stage, self.root, 'a' * 40, '0.1.19')

    def test_links_and_invalid_source_target_are_refused(self):
        for revision, version, target in [('main', '0.1.19', 'macos-arm64'), ('a' * 40, 'latest', 'macos-arm64'), ('a' * 40, '0.1.19', 'macos-x64')]:
            with self.assertRaises(ValueError):
                module.package(self.stage, self.root, revision, version, target)
        (self.stage / 'leak').symlink_to('/etc/passwd')
        with self.assertRaisesRegex(ValueError, 'link'):
            module.package(self.stage, self.root, 'a' * 40, '0.1.19')


if __name__ == '__main__':
    unittest.main()
