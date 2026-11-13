#!/usr/bin/env python3
import argparse,subprocess,tempfile,sys,json
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from decode_raw import decode,load_map
p=argparse.ArgumentParser();p.add_argument('--exe');p.add_argument('--reindexed-exe');a=p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
 ids=[]
 for n,exe in enumerate((a.exe,a.reindexed_exe)):
  m=Path(tmp)/f'{n}.json';f=Path(tmp)/f'{n}.raw'
  subprocess.run([exe,'--map',str(m)],check=True);subprocess.run([exe,'--fixture',str(f)],check=True)
  meta=load_map(m);rows,used,truncated=decode(f.read_bytes(),meta)
  assert meta['layout']=='flat_records' and meta['absolute_time_tag']==0
  assert len(rows)==2 and not truncated and used==f.stat().st_size
  assert rows[0]['time_us']==1000 and rows[0]['duration_us']==1
  assert rows[1]['metric']['count']==1
  ids.append(rows[0]['index'])
 assert ids[0]!=ids[1]

 # Ranking must never propose the reserved command index zero.
 with tempfile.TemporaryDirectory() as rankdir:
  m=Path(rankdir)/'map.json';f=Path(rankdir)/'data.raw'
  subprocess.run([a.exe,'--map',str(m)],check=True)
  subprocess.run([a.exe,'--fixture',str(f)],check=True)
  tool=Path(__file__).resolve().parents[2]/'tools'/'rank_raw_tags.py'
  result=subprocess.run([sys.executable,str(tool),'--map',str(m),str(f)],capture_output=True,text=True,check=True)
  assert all(row['new_index']>=1 for row in json.loads(result.stdout)['proposed_indices'])
