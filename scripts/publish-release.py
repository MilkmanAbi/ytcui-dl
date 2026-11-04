"""Retry-safe release publication. Existing archive bytes are never replaced."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from urllib.parse import quote

repo = os.environ['GITHUB_REPOSITORY']
tag = os.environ['RELEASE_TAG']
commit = os.environ['RELEASE_COMMIT']
directory = Path(sys.argv[1]).resolve()

def gh(*args):
    result = subprocess.run(['gh', *args], text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return result.stdout

actual_commit = json.loads(gh('api', f'repos/{repo}/commits/{quote(tag, safe="")}'))['sha']
if actual_commit != commit:
    raise SystemExit('Tag moved after validation; refusing to publish')
pairs = []
for checksum in sorted(directory.glob('*.sha256')):
    expected, name = checksum.read_text().strip().split(None, 1)
    if Path(name).name != name or name.startswith('.'):
        raise SystemExit('Invalid archive name in checksum')
    archive = directory/name
    if hashlib.sha256(archive.read_bytes()).hexdigest() != expected:
        raise SystemExit(f'Local archive checksum mismatch: {name}')
    pairs.append((archive, checksum))
if not pairs:
    raise SystemExit('No verified archive/checksum pairs found')
result = subprocess.run(['gh','api',f'repos/{repo}/releases/tags/{quote(tag, safe="")}'],
                        text=True,capture_output=True)
if result.returncode:
    if 'HTTP 404' not in result.stderr:
        raise RuntimeError(result.stderr.strip())
    paths = [str(path) for pair in pairs for path in pair]
    print(gh('release','create',tag,*paths,'--repo',repo,'--verify-tag',
             '--title',f'{repo.split("/")[-1]} {tag}','--generate-notes'))
else:
    release = json.loads(result.stdout)
    if release['draft']:
        raise SystemExit('Existing release is a draft; refusing to publish it implicitly')
    existing = {asset['name']:asset for asset in release['assets']}
    with tempfile.TemporaryDirectory(prefix='release-verify-') as temporary:
        scratch = Path(temporary)
        for archive, checksum in pairs:
            if archive.name not in existing:
                if checksum.name in existing:
                    raise SystemExit(f'Orphaned published checksum requires review: {checksum.name}')
                gh('release','upload',tag,str(archive),str(checksum),'--repo',repo)
                print(f'Uploaded missing pair: {archive.name}')
                continue
            # Rebuilds can differ in archive timestamps. Preserve published
            # bytes and validate against their own published checksum.
            digest = existing[archive.name].get('digest')
            if not digest:
                gh('release','download',tag,'--repo',repo,'--pattern',archive.name,'--dir',str(scratch))
                digest = 'sha256:' + hashlib.sha256((scratch/archive.name).read_bytes()).hexdigest()
            if not digest.startswith('sha256:'):
                raise SystemExit(f'Unknown published digest format: {archive.name}')
            expected = digest.removeprefix('sha256:')
            if checksum.name in existing:
                gh('release','download',tag,'--repo',repo,'--pattern',checksum.name,'--dir',str(scratch))
                saved, name = (scratch/checksum.name).read_text().strip().split(None,1)
                if saved != expected or name != archive.name:
                    raise SystemExit(f'Published checksum mismatch: {archive.name}')
            else:
                restored = scratch/checksum.name
                restored.write_text(f'{expected}  {archive.name}\n')
                gh('release','upload',tag,str(restored),'--repo',repo)
            print(f'Verified existing archive: {archive.name}')
    print(release['html_url'])
