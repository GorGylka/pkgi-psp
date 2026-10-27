#!/usr/bin/env python3
import os,struct,subprocess,tempfile
from pathlib import Path
from cryptography.hazmat.primitives.ciphers import Cipher,algorithms,modes
root=Path(__file__).resolve().parents[2];test=Path(__file__).resolve().parent
key=bytes.fromhex('07f2c68290b50d2c33818d709b60e62b');iv=bytes.fromhex('fffffffffffffffffffffffffffffff0')
files={'EBOOT.PBP':bytes((i*29)%256 for i in range(2*1024*1024+31)), 'SUB/DATA.BIN':bytes(range(256))*2048+b'end', 'EMPTY.BIN':b'', 'TINY.BIN':b'7 bytes', 'EXACT.BIN':bytes(256*1024)}
def fixture(kind='ok'):
 names=['USRDIR','USRDIR/CONTENT','USRDIR/CONTENT/SUB']+['USRDIR/CONTENT/'+n for n in files]
 body=bytearray(32*len(names));entries=[]
 for i,name in enumerate(names):
  if kind=='path' and i==3:name='USRDIR/CONTENT/../../escape'
  nb=name.encode();off=len(body);body+=nb;body+=b'\0'*(-len(body)%16)
  e=bytearray(32);struct.pack_into('>II',e,0,off,len(nb));e[24]=0x90;e[27]=4 if i<3 else 1;entries.append(e)
 for e,data in zip(entries[3:],files.values()):
  struct.pack_into('>QQ',e,8,len(body),len(data));body+=data;body+=b'\0'*(-len(body)%16)
 if kind=='late':struct.pack_into('>I',entries[3],0,len(body)-16)
 if kind=='overlap':struct.pack_into('>Q',entries[4],8,struct.unpack_from('>Q',entries[3],8)[0])
 if kind=='duplicate':struct.pack_into('>II',entries[4],0,*struct.unpack_from('>II',entries[3],0))
 # Deliberately shuffled file entries; decoder must sort by data offset.
 entries[3],entries[4]=entries[4],entries[3]
 body[:32*len(names)]=b''.join(entries)
 header=bytearray(512);header[:8]=bytes.fromhex('7f504b4780000002')
 for off,v in [(8,0x100),(0x14,len(names)),(0x114,1)]:struct.pack_into('>I',header,off,v)
 struct.pack_into('>QQQ',header,0x18,512+len(body)+64,512,len(body))
 header[0x70:0x80]=iv;header[0x148:0x151]=b'TEST00001'
 if kind=='length':struct.pack_into('>Q',header,0x18,9999)
 encrypt=Cipher(algorithms.AES(key),modes.CTR(iv)).encryptor()
 return header+encrypt.update(bytes(body))+encrypt.finalize()+bytes(64)
with tempfile.TemporaryDirectory() as tmp:
 tmp=Path(tmp);exe=tmp/'test'
 subprocess.run(['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-DPKGI_PROFILE_HOST','-Wno-deprecated-declarations','-I'+str(root/'tests/me/stubs'),'-I'+str(root/'include'),str(test/'host_stream.c'),str(root/'source/pkgi_stream_unpack.c'),str(root/'source/depackager.c'),str(root/'source/pkgi_pair_crypto.c'),str(root/'source/pkgi_install_profile.c'),'-Wl,--wrap=stat','-Wl,--wrap=mkdir','-Wl,--wrap=fwrite','-Wl,--wrap=fclose','-Wl,--wrap=rename','-lcrypto','-o',str(exe)],check=True)
 cases=[('ok',m,int(m in (0,1,3))) for m in (0,1,3,4,5,6,7,8,9,10,15,17)]+[(k,1,0) for k in ('path','late','overlap','duplicate','length')]
 for i,(kind,mode,expected) in enumerate(cases):
  pkg=tmp/f'{i}.pkg';pkg.write_bytes(fixture(kind));dest=tmp/str(i);target=dest/'PSP/GAME/TEST00001';target.mkdir(parents=True);(target/'old.txt').write_text('keep existing game')
  subprocess.run([str(exe),str(pkg),str(dest),str(mode),str(expected)],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'),check=True)
  if expected:
   assert not (target/'old.txt').exists()
   for name,data in files.items():assert (target/name).read_bytes()==data,name
  else:
   assert (target/'old.txt').read_text()=='keep existing game'
   assert list(target.iterdir())==[target/'old.txt']
  assert not list((dest/'PSP/GAME').glob('.pkgi-stream-*'))
  assert not (dest/'escape').exists()
 print('PASS: streaming output, partial reads/tails, reordered entries, cancellation, truncation, bad EOF, crypto/write/close errors, commit rollback, path/range rejection, full staging cleanup and old game preservation.')

 # Exercise the actual producer/consumer code against pthread/semaphore PSP shims.
 pipe=tmp/'pipe'
 subprocess.run(['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-DPKGI_PROFILE_HOST','-Wno-deprecated-declarations','-I'+str(test/'stubs'),'-I'+str(root/'tests/me/stubs'),'-I'+str(root/'include'),str(test/'host_pipe.c'),str(root/'source/pkgi_stream_download.c'),str(root/'source/pkgi_stream_unpack.c'),str(root/'source/depackager.c'),str(root/'source/pkgi_pair_crypto.c'),str(root/'source/pkgi_install_profile.c'),'-Wl,--wrap=stat','-Wl,--wrap=mkdir','-Wl,--wrap=fwrite','-Wl,--wrap=fclose','-Wl,--wrap=rename','-lcrypto','-pthread','-o',str(pipe)],check=True)
 files['EBOOT.PBP']=files['EBOOT.PBP']*6  # > 8 MiB queue; forces wraparound/backpressure.
 pkg=tmp/'large.pkg';pkg.write_bytes(fixture())
 for mode in [0,1,3,7,8,9,16]:
  expected=int(mode in (0,1,3,16));dest=tmp/f'pipe{mode}';target=dest/'PSP/GAME/TEST00001';target.mkdir(parents=True);(target/'old.txt').write_text('keep')
  if mode==0:
   (target/'old.txt').unlink();target.rmdir()
  subprocess.run([str(pipe),str(pkg),str(dest),str(mode),str(expected)],cwd=dest,timeout=30,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'),check=True)
  if expected:
   for name,data in files.items():assert (target/name).read_bytes()==data
  else:assert list(target.iterdir())==[target/'old.txt'] and (target/'old.txt').read_text()=='keep'
  assert not list((dest/'PSP/GAME').glob('.pkgi-stream-*'))
 print('PASS: actual streaming queue with irregular HTTP chunks, >8 MiB wraparound, slow network, slow consumer, cancellation, network error and unsuccessful EOF.')

 # PSP paths: stat/mkdir on bare device names are rejected by the host shim.
 for device in ['ms0:', 'ef0:']:
  cwd=tmp/device.replace(':','');cwd.mkdir();disk=cwd/device;disk.mkdir()
  target=disk/'PSP/GAME/TEST00001';target.mkdir(parents=True);(target/'old.txt').write_text('keep')
  subprocess.run([str(pipe),str(pkg),device,'1','1'],cwd=cwd,timeout=30,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0',UBSAN_OPTIONS='halt_on_error=1'),check=True)
  for name,data in files.items():assert (target/name).read_bytes()==data
  assert not list((disk/'PSP/GAME').glob('.pkgi-stream-*'))
 print('PASS: ms0:/ and ef0:/ device roots, full streaming install and staging cleanup.')
