import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

exe = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='ytcui cli space ') as temporary:
    root = Path(temporary)
    env = dict(os.environ, HOME=str(root), XDG_CONFIG_HOME=str(root/'config'),
               PATH=str(exe.parent)+os.pathsep+os.environ.get('PATH', ''), BROWSER='fixture-browser %s')
    os.environ['PATH'] = env['PATH']
    def run(args, ok=True):
        result = subprocess.run([str(exe), *args], cwd=root, env=env, capture_output=True, timeout=25)
        assert (result.returncode == 0) == ok, (args, result.stdout, result.stderr)
        return result.stdout.decode()
    assert 'ytcui-dl' in run(['--help'])
    assert run(['--version']).strip() == (Path(__file__).resolve().parents[1]/'VERSION').read_text().strip()
    run(['--this-option-does-not-exist'], False)
    assert 'ytcui-dl session' in run(['session', '--help'])
    assert 'fixture-browser' in run(['session', 'login', '--print-command'])
    run(['session', 'export', '--profile', str(root/'missing')], False)
    run(['session', 'check'], False)
    shutil.copy2(Path(__file__).resolve().parent/'fixtures/browser_cookies.sqlite', root/'cookies.sqlite')
    run(['session','export','--browser','firefox','--profile',str(root/'cookies.sqlite'),'-o',str(root/'jar.txt')])
    jar = (root/'jar.txt').read_text()
    assert 'ff-sapisid-value' in jar and 'example.com' not in jar
    assert (root/'jar.txt').stat().st_mode & 0o777 == 0o600
    assert not (root/'config').exists(), 'explicit export should not touch default credential location'
    result = subprocess.run(['ytcui-dl','--version'], cwd=root, env=env, capture_output=True, timeout=10)
    assert result.returncode == 0
print('CLI and synthetic browser session checks passed')
