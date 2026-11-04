"""Deterministic transfer, resume and redirect regressions; never contacts YouTube."""
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import threading
import time
import ssl
import shutil

exe = Path(sys.argv[1]).resolve()
data = b''.join(hashlib.sha256(str(i).encode()).digest() * 32768 for i in range(3)) + b'fixture-end'
requests = []
failing = True
other_port = 0
class Server(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'
    def log_message(self, *args): pass
    def do_HEAD(self):
        self.send_response(200); self.send_header('Content-Length', str(len(data))); self.end_headers()
    def do_GET(self):
        requests.append((self.path, self.headers.get('Range')))
        if self.path == '/redirect':
            self.send_response(302); self.send_header('Location', f'http://127.0.0.1:{other_port}/headers')
            self.send_header('Content-Length', '0'); self.end_headers(); return
        if self.path == '/downgrade':
            self.send_response(302); self.send_header('Location', f'http://127.0.0.1:{other_port}/range')
            self.send_header('Content-Length', '0'); self.end_headers(); return
        if self.path == '/oversized':
            self.send_response(200); self.send_header('Content-Length', str(1024**3)); self.end_headers(); return
        if self.path == '/headers':
            body = json.dumps({key:self.headers.get(key) for key in ('Cookie','Authorization')}).encode()
            self.send_response(200); self.send_header('Content-Length', str(len(body))); self.end_headers()
            self.wfile.write(body); return
        body = data
        header = self.headers.get('Range')
        start, end = 0, len(body)-1
        ranged = header and self.path not in ('/ignore','/chunked')
        if ranged:
            match = re.fullmatch(r'bytes=(\d+)-(\d*)', header)
            assert match, header
            start = int(match[1]); end = min(int(match[2]) if match[2] else len(body)-1, len(body)-1)
            body = body[start:end+1]
        self.send_response(206 if ranged else 200)
        if ranged:
            wrong = 1 if self.path == '/wrong' else 0
            self.send_header('Content-Range', f'bytes {start+wrong}-{end}/{len(data)}')
        if self.path == '/chunked': self.send_header('Transfer-Encoding', 'chunked')
        else: self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            if self.path == '/chunked':
                for offset in range(0,len(body),7001):
                    chunk = body[offset:offset+7001]
                    self.wfile.write(f'{len(chunk):x};fixture=yes\r\n'.encode()+chunk+b'\r\n')
                self.wfile.write(b'0\r\nX-Fixture: complete\r\n\r\n'); self.wfile.flush(); return
            if self.path == '/short' or (self.path == '/fail' and failing and start == 1048576):
                self.wfile.write(body[:len(body)//2]); self.wfile.flush(); self.close_connection=True; return
            for offset in range(0,len(body),65536):
                self.wfile.write(body[offset:offset+65536]); self.wfile.flush()
                if self.path == '/slow': time.sleep(.035)
        except (BrokenPipeError,ConnectionResetError): pass

servers = [http.server.ThreadingHTTPServer(('127.0.0.1',0),Server) for _ in range(2)]
for server in servers:
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
other_port = servers[1].server_port
with tempfile.TemporaryDirectory(prefix='ytcui transfer space ') as temporary:
    root = Path(temporary)
    def run(path, name, ok=True, extra=()):
        output = root/name
        result = subprocess.run([str(exe),f'http://127.0.0.1:{servers[0].server_port}{path}',str(output),*extra],
                                capture_output=True, timeout=35)
        assert (result.returncode == 0) == ok, (path,result.stderr)
        return output,result
    for path in ('/range','/ignore','/chunked'):
        output,_=run(path,path[1:]+'.bin')
        assert output.read_bytes() == data,path
    for path in ('/wrong','/short'):
        output,_=run(path,path[1:]+'.bin',False)
        assert not output.exists(),'failed transfer finalized its output'
    output,_=run('/fail','resume.bin',False)
    assert not output.exists()
    ledger=Path(str(output)+'.part.ranges')
    committed = {int(index) for index in ledger.read_text().splitlines()[1:]}
    assert committed, 'fixture should commit at least one complete chunk before failure'
    failing=False; requests.clear()
    output,_=run('/fail','resume.bin')
    assert output.read_bytes()==data
    for index in committed:
        header=f'bytes={index*1048576}-{min((index+1)*1048576,len(data))-1}'
        assert ('/fail',header) not in requests,'committed chunk downloaded again'
    # A preallocated file without proof of completed ranges cannot count as complete.
    stale=root/'stale.bin.part'; stale.write_bytes(bytes(len(data)))
    output,_=run('/range','stale.bin')
    assert output.read_bytes()==data
    output,_=run('/slow','cancel.bin',False,('cancel',))
    assert not output.exists()
    output,_=run('/range','range.bin',False)
    assert output.read_bytes()==data,'existing final output changed'
    output,result=run('/redirect','unused',True,('get',))
    assert json.loads(result.stdout)=={'Cookie':None,'Authorization':None},'credentials followed cross-origin redirect'
    run('/short','unused-short',False,('get',))
    run('/oversized','unused-large',False,('get',))
    openssl=shutil.which('openssl')
    assert openssl,'OpenSSL CLI required for local TLS test fixtures'
    (root/'tls.cnf').write_text('[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n[dn]\nCN=localhost\n[v3]\nsubjectAltName=DNS:localhost\nbasicConstraints=critical,CA:TRUE\n')
    subprocess.run([openssl,'req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(root/'key.pem'),
                    '-out',str(root/'cert.pem'),'-days','1','-config',str(root/'tls.cnf')],
                   check=True,capture_output=True,timeout=30)
    tls=http.server.ThreadingHTTPServer(('127.0.0.1',0),Server); tls.daemon_threads=True
    context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(root/'cert.pem',root/'key.pem')
    tls.socket=context.wrap_socket(tls.socket,server_side=True)
    threading.Thread(target=tls.serve_forever,daemon=True).start()
    trusted=dict(os.environ,SSL_CERT_FILE=str(root/'cert.pem'))
    for host,ok in [('localhost',True),('127.0.0.1',False)]:
        result=subprocess.run([str(exe),f'https://{host}:{tls.server_port}/range',str(root/f'tls-{host}.bin')],
                              env=trusted,capture_output=True,timeout=35)
        assert (result.returncode==0)==ok,(host,result.stderr)
        if ok: assert (root/f'tls-{host}.bin').read_bytes()==data
    result=subprocess.run([str(exe),f'https://localhost:{tls.server_port}/downgrade',str(root/'downgrade.bin')],
                          env=trusted,capture_output=True,timeout=35)
    assert result.returncode!=0 and not (root/'downgrade.bin').exists(),'HTTPS downgrade accepted'
    tls.shutdown(); tls.server_close()
    # Fake ffmpeg checks the real process boundary without requiring media tools.
    tools = root/'tools'; tools.mkdir()
    ffmpeg = tools/'ffmpeg'
    ffmpeg.write_text('#!'+sys.executable+'\nimport pathlib,sys\nif "-version" not in sys.argv:\n pathlib.Path(sys.argv[-1]).write_bytes(b"encoded-fixture")\n')
    ffmpeg.chmod(0o755)
    process_env = dict(os.environ, PATH=str(tools)+os.pathsep+os.environ.get('PATH',''))
    source=root/"audio';touch injected;'.m4a"; source.write_bytes(b'source')
    target=root/"converted';touch injected;'.mp3"
    def native(args,ok=True):
        result=subprocess.run([str(exe),*map(str,args)],cwd=root,env=process_env,capture_output=True,timeout=20)
        assert (result.returncode==0)==ok,result.stderr
    native([source,target,'convert'])
    assert target.read_bytes()==b'encoded-fixture' and not (root/'injected').exists()
    target.write_bytes(b'preserve-existing')
    native([source,target,'convert'],False)
    assert target.read_bytes()==b'preserve-existing'
    muxed=root/'mux.mkv'
    native([source,source,'mux',muxed])
    assert muxed.read_bytes()==b'encoded-fixture'
    muxed.write_bytes(b'preserve-muxed')
    native([source,source,'mux',muxed],False)
    assert muxed.read_bytes()==b'preserve-muxed'
    streamed=root/'stream.mp3'
    legacy=Path(str(streamed)+'.src-audio.tmp'); legacy.write_bytes(b'preserve-foreign-temp')
    native([f'http://127.0.0.1:{servers[0].server_port}/range',streamed,'stream'])
    assert streamed.read_bytes()==b'encoded-fixture' and legacy.read_bytes()==b'preserve-foreign-temp'
    assert not list(root.glob('*.work.*')),'owned staging directories should be cleaned'
for server in servers: server.shutdown(); server.server_close()
print('Ranges, chunks, truncation, resume, cancellation, existing files, redirects, metadata limits and TLS checks passed')
