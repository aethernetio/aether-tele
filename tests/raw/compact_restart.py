#!/usr/bin/env python3
"""Healthy process restarts append flat records and continue metric deltas."""
import argparse,json,subprocess,tempfile,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from decode_raw import decode,load_map,restore_metrics
p=argparse.ArgumentParser();p.add_argument('--exe',required=True);a=p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
 path=Path(tmp)/'data';mapping=Path(tmp)/'map.json'
 subprocess.run([a.exe,'--map',str(mapping)],check=True)
 previous=b''
 for boot in range(5):
  subprocess.run([a.exe,'--boot',str(path),str(boot)],check=True)
  contents=path.read_bytes();assert contents.startswith(previous)
  subprocess.run([a.exe,'--verify',str(path),str(boot+1)],check=True)
  assert path.read_bytes()==contents
  previous=contents
 metadata=load_map(mapping);rows,used,truncated=decode(previous,metadata)
 assert used==len(previous) and not truncated
 assert len(rows)==10
 assert [r['time_us'] for r in rows if r['kind']=='log']==list(range(1000,1005))
 index=metadata['tags'][0]['index']
 assert restore_metrics(rows)[index]==dict(count=5,samples=5,sum=25,min=5,max=5)
