"""Installer decisions with fake apt/sudo/build tools; never changes the host packages."""
from contextlib import contextmanager
from pathlib import Path
import json
import os
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

BUILD_TOOL = '''#!/usr/bin/python3
import json
import os
from pathlib import Path
import sys

tool = Path(sys.argv[0]).name
args = sys.argv[1:]
with open(os.environ["QO100_TEST_BUILD_LOG"], "a") as log:
    print(json.dumps([tool, *args]), file=log)
if tool == "git":
    if args == ["hash-object", "--stdin"]:
        git = os.environ["QO100_TEST_REAL_GIT"]
        os.execv(git, [git, *args])
    if args[:2] == ["init", "--quiet"]:
        sys.exit(0)
    if args[0] == "-C" and args[2] == "fetch":
        sys.exit(int(os.environ.get("QO100_TEST_FAIL_FETCH", "0")))
    if args[0] == "-C" and args[2] == "checkout":
        (Path(args[1]) / "CMakeLists.txt").touch()
    else:
        sys.exit(1)
'''


class DependencyInstallTest(unittest.TestCase):
    @contextmanager
    def installer(self, installed='', custom_server=''):
        with tempfile.TemporaryDirectory(prefix='qo100-deps-') as temp:
            root=Path(temp);project=root/'sat'/'project';project.mkdir(parents=True);tools=root/'tools';tools.mkdir()
            shutil.copy2(ROOT/'build_all.sh',project/'build_all.sh')
            arch={'amd64':'x86_64','arm64':'aarch64'}.get(platform.machine().lower(),platform.machine().lower())
            for bundle,header,archive in [('liquid-dsp-1.8.3','liquid/liquid.h','libliquid.a'),('speexdsp-1.2.1','speex/speex_preprocess.h','libspeexdsp.a')]:
                for relative in ('include/'+header,f'lib/linux-{arch}/'+archive):
                    p=project/'external'/bundle/relative;p.parent.mkdir(parents=True,exist_ok=True);p.touch()
            for name in ('dirname','getconf','mkdir','mktemp','mv','rm','uname'):(tools/name).symlink_to(shutil.which(name))
            for name in ('pkg-config','cc','c++','gfortran','make','python3'):(tools/name).symlink_to('/usr/bin/true')
            log=root/'apt.jsonl'
            scripts={
                'cmake': BUILD_TOOL,
                'git': BUILD_TOOL,
                'apt-get': '#!/usr/bin/python3\nimport json,os,sys\nwith open(os.environ["QO100_TEST_APT_LOG"],"a") as f:f.write(json.dumps(sys.argv[1:])+"\\n")\n',
                'sudo': '#!/bin/sh\nexec "$@"\n',
                'dpkg-query': '#!/usr/bin/python3\nimport os,sys\nif sys.argv[-1]==os.environ.get("QO100_TEST_INSTALLED"):print("install ok installed")\nelse:sys.exit(1)\n'}
            if custom_server:scripts[custom_server]='#!/bin/sh\nexit 0\n'
            for name,body in scripts.items():p=tools/name;p.write_text(body);p.chmod(0o755)
            env={**os.environ,'LIBFT8_ROOT':str(root/'libft8-package'),'PATH':str(tools),'QO100_TEST_APT_LOG':str(log),'QO100_TEST_INSTALLED':installed}
            env.update(QO100_TEST_BUILD_LOG=str(root/'build.jsonl'), QO100_TEST_REAL_GIT=shutil.which('git'))
            env.pop('LIBFT8_SOURCE_DIR', None)
            env.pop('QO100_TEST_FAIL_FETCH', None)
            yield project, root, env

    @staticmethod
    def invoke(project, env, args=()):
        return subprocess.run(['/bin/bash',str(project/'build_all.sh'),*args],
            cwd=project,env=env,text=True,capture_output=True)

    @staticmethod
    def read_calls(path):
        return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []

    def run_script(self, installed='', custom_server='', args=()):
        with self.installer(installed, custom_server) as (project, root, env):
            result=self.invoke(project,env,args)
            self.assertEqual(result.returncode,0,result.stderr+result.stdout)
            return self.read_calls(root/'apt.jsonl')

    def test_minimal_system_gets_sound_server_and_adi_libraries(self):
        calls=self.run_script();self.assertEqual(calls[0],['update'])
        packages=calls[1]
        for package in ('pulseaudio','pulseaudio-utils','libasound2-plugins','dbus-user-session',
                        'libiio-dev','libad9361-dev','libiio-utils','libcurl4-openssl-dev','python3-numpy','python3-scipy'):
            self.assertIn(package,packages)

    def test_existing_pipewire_is_preserved(self):
        self.assertNotIn('pulseaudio',self.run_script(installed='pipewire-pulse')[1])

    def test_existing_pulseaudio_is_preserved(self):
        self.assertNotIn('pulseaudio',self.run_script(installed='pulseaudio')[1])

    def test_custom_sound_server_is_preserved(self):
        self.assertNotIn('pulseaudio',self.run_script(custom_server='pipewire-pulse')[1])

    def test_legacy_flag_uses_the_same_package_installation(self):
        self.assertEqual(self.run_script(args=('--install-deps',)), self.run_script())

    def test_help_does_not_install_packages(self):
        self.assertEqual(self.run_script(args=('--help',)), [])

    def test_libft8_download_can_be_retried_and_reused(self):
        with self.installer() as (project, root, env):
            del env['LIBFT8_ROOT']
            offline = {**env, 'QO100_TEST_FAIL_FETCH': '1'}
            failed = self.invoke(project, offline)
            self.assertNotEqual(failed.returncode, 0)
            self.assertEqual(list((project/'build'/'libft8').iterdir()), [])
            self.assertFalse(any(call[0] == 'cmake' for call in self.read_calls(root/'build.jsonl')))

            downloaded = self.invoke(project, env)
            self.assertEqual(downloaded.returncode, 0, downloaded.stderr + downloaded.stdout)
            reused = self.invoke(project, offline)
            self.assertEqual(reused.returncode, 0, reused.stderr + reused.stdout)

            calls = self.read_calls(root/'build.jsonl')
            fetches = [call for call in calls if call[:2] == ['git', '-C'] and call[3] == 'fetch']
            self.assertEqual(len(fetches), 2)
            self.assertEqual(fetches[1][-2], 'https://github.com/dj0abr/libft8.git')
            revision = fetches[1][-1]
            self.assertRegex(revision, r'^[0-9a-f]{40}$')
            source = project/'build'/'libft8'/f'source-{revision}'
            self.assertTrue((source/'CMakeLists.txt').is_file())
            self.assertEqual(sum(call[:3] == ['cmake', '-S', str(source)] for call in calls), 2)


if __name__=='__main__':unittest.main()
