"""Installer decisions with fake apt/sudo/build tools; never changes the host packages."""
from pathlib import Path
import json
import os
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]


class DependencyInstallTest(unittest.TestCase):
    def run_script(self, installed='', custom_server='', install=True):
        with tempfile.TemporaryDirectory(prefix='qo100-deps-') as temp:
            root=Path(temp);project=root/'project';project.mkdir();tools=root/'tools';tools.mkdir()
            shutil.copy2(ROOT/'build_all.sh',project/'build_all.sh')
            arch={'amd64':'x86_64','arm64':'aarch64'}.get(platform.machine().lower(),platform.machine().lower())
            for bundle,header,archive in [('liquid-dsp-1.8.3','liquid/liquid.h','libliquid.a'),('speexdsp-1.2.1','speex/speex_preprocess.h','libspeexdsp.a')]:
                for relative in ('include/'+header,f'lib/linux-{arch}/'+archive):
                    p=project/'external'/bundle/relative;p.parent.mkdir(parents=True,exist_ok=True);p.touch()
            for name in ('dirname','getconf','uname'):(tools/name).symlink_to(shutil.which(name))
            for name in ('cmake','pkg-config','cc','c++','gfortran','make','python3'):(tools/name).symlink_to('/usr/bin/true')
            log=root/'apt.jsonl'
            scripts={
                'apt-get': '#!/usr/bin/python3\nimport json,os,sys\nwith open(os.environ["QO100_TEST_APT_LOG"],"a") as f:f.write(json.dumps(sys.argv[1:])+"\\n")\n',
                'sudo': '#!/bin/sh\nexec "$@"\n',
                'dpkg-query': '#!/usr/bin/python3\nimport os,sys\nif sys.argv[-1]==os.environ.get("QO100_TEST_INSTALLED"):print("install ok installed")\nelse:sys.exit(1)\n'}
            if custom_server:scripts[custom_server]='#!/bin/sh\nexit 0\n'
            for name,body in scripts.items():p=tools/name;p.write_text(body);p.chmod(0o755)
            env={**os.environ,'PATH':str(tools),'QO100_TEST_APT_LOG':str(log),'QO100_TEST_INSTALLED':installed}
            result=subprocess.run(['/bin/bash',str(project/'build_all.sh'),*(['--install-deps'] if install else [])],
                env=env,text=True,capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr+result.stdout)
            calls=[json.loads(line) for line in log.read_text().splitlines()] if log.exists() else []
            return calls

    def test_minimal_system_gets_sound_server_and_adi_libraries(self):
        calls=self.run_script();self.assertEqual(calls[0],['update'])
        packages=calls[1]
        for package in ('pulseaudio','pulseaudio-utils','libasound2-plugins','dbus-user-session',
                        'libiio-dev','libad9361-dev','libiio-utils','python3-numpy','python3-scipy'):
            self.assertIn(package,packages)

    def test_existing_pipewire_is_preserved(self):
        self.assertNotIn('pulseaudio',self.run_script(installed='pipewire-pulse')[1])

    def test_existing_pulseaudio_is_preserved(self):
        self.assertNotIn('pulseaudio',self.run_script(installed='pulseaudio')[1])

    def test_custom_sound_server_is_preserved(self):
        self.assertNotIn('pulseaudio',self.run_script(custom_server='pipewire-pulse')[1])

    def test_regular_build_never_installs_packages(self):
        self.assertEqual(self.run_script(install=False),[])


if __name__=='__main__':unittest.main()
