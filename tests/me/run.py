#!/usr/bin/env python3
"""Host integration test. Requires gcc, OpenSSL dev headers, Python cryptography.
Mocks ME scheduling and PSP I/O; does NOT emulate ME hardware/cache/ABI.
"""
import os,struct,subprocess,tempfile,re
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes
root=Path(__file__).resolve().parents[2]
test=Path(__file__).resolve().parent
key=bytes.fromhex('07f2c68290b50d2c33818d709b60e62b')
iv=bytes.fromhex('fffffffffffffffffffffffffffffff0')
names=['EBOOT.PBP','DOCUMENT.DAT','SMALL.BIN','EMPTY.BIN']
sizes=[2*1024*1024+31, 512*1024+7, 17, 0]
payloads=[bytes((i*29+j)%256 for i in range(n)) for j,n in enumerate(sizes)]
body=bytearray(32*len(names));entries=[]
for name,payload in zip(names,payloads):
 name_bytes=b'X'*15+name.encode()+b'\0';name_off=len(body)
 body+=name_bytes;body+=b'\0'*(-len(body)%16)
 data_off=len(body);body+=payload;body+=b'\0'*(-len(body)%16)
 entry=bytearray(32)
 for off,value in [(0,name_off),(4,len(name_bytes)),(12,data_off),(20,len(payload))]:struct.pack_into('>I',entry,off,value)
 entry[24]=0x90;entry[27]=1
 entries.append(entry)
body[:32*len(entries)]=b''.join(entries)
header=bytearray(512);header[:8]=bytes.fromhex('7f504b4780000002')
for off,value in [(8,0x100),(0x14,len(names)),(0x1c,512+len(body)),(0x24,512),(0x2c,len(body)),(0x114,1)]:struct.pack_into('>I',header,off,value)
header[0x70:0x80]=iv;header[0x148:0x151]=b'TEST00001'
encryptor=Cipher(algorithms.AES(key),modes.CTR(iv)).encryptor()
with tempfile.TemporaryDirectory() as tmp:
 tmp=Path(tmp);pkg=tmp/'test.pkg';pkg.write_bytes(header+encryptor.update(bytes(body))+encryptor.finalize())
 exe=tmp/'installer'
 subprocess.run(['gcc','-std=gnu11','-DPKGI_PROFILE_HOST','-O1','-g','-fsanitize=address,undefined','-Wno-deprecated-declarations',
  '-I'+str(test/'stubs'),'-I'+str(root/'include'), str(test/'host_installer.c'),
  str(root/'source/depackager.c'),str(root/'source/pkgi_pair_crypto.c'),str(root/'source/pkgi_install_profile.c'),'-lcrypto','-o',str(exe)],check=True)
 env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1')
 for mode in [0,1,3,4,5,6,7]:
  dest=tmp/str(mode);dest.mkdir()
  subprocess.run([str(exe),str(pkg),str(dest),str(mode)],check=True,env=env,cwd=dest)
  report=(dest/"pkgi-install-cpu-profile.txt").read_text()
  assert f"install_ok: {int(mode < 4)}" in report
  assert "session_us:" in report and "decrypt_total_us:" in report
  values={k:int(v) for k,v in re.findall(r'^(\w+): (\d+)',report,re.M)}
  outer=sum(values[k+'_us'] for k in ['payload_read','decrypt_total','payload_write','progress_ui','me_start_selftests','me_shutdown','other'])
  assert outer == values['session_us'], (outer,values)
  assert values['cpu_crypto_us']+values['me_submit_copy_cache_us']+values['me_join_total_us'] <= values['decrypt_total_us']
  if mode<4:
   assert values['payload_bytes'] == sum(sizes)

   for name,expected in zip(names,payloads):
    path=dest/'PSP/GAME/TEST00001'/name
    if expected:assert path.read_bytes()==expected,(mode,name)
  else:
   assert not (dest/'PSP/GAME/TEST00001/EBOOT.PBP').exists(),mode
 print('PASS: CPU-only, CPU+mock-ME, short I/O, submit/join/write failures; output byte equality; partial tails and multi-pair files.')
