"""Install, archive and execute the actual POSIX release package."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
parser = argparse.ArgumentParser()
parser.add_argument('--build-dir', default='build')
parser.add_argument('--name', required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
stage = root/'dist'/args.name
if stage.exists(): raise SystemExit(f'Refusing to mix package contents with {stage}')
stage.mkdir(parents=True)
subprocess.run(['cmake','--install',args.build_dir,'--prefix',str(stage)],check=True)
subprocess.run([sys.executable,str(root/'test/test_cli.py'),str(stage/'bin/ytcui-dl')],check=True)
archive = Path(shutil.make_archive(str(stage),'gztar',root_dir=stage.parent,base_dir=stage.name))
digest = hashlib.sha256(archive.read_bytes()).hexdigest()
archive.with_name(archive.name+'.sha256').write_text(f'{digest}  {archive.name}\n')
unpacked = root/'dist'/('verify-'+args.name)
shutil.unpack_archive(archive,unpacked)
subprocess.run([sys.executable,str(root/'test/test_cli.py'),str(unpacked/args.name/'bin/ytcui-dl')],check=True)
print(f'Packaged and verified {archive.name}: {digest}')
