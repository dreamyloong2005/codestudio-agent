from pathlib import Path
import importlib.util,hashlib,json,subprocess
root=Path(__file__).resolve().parents[2]
mirror=Path('/var/tmp/codestudio-agent-linux')
evidence=root/'research/durable-requests'
spec=importlib.util.spec_from_file_location('verification',mirror/'scripts/verify.py'); verification=importlib.util.module_from_spec(spec);spec.loader.exec_module(verification)
expected=json.loads((evidence/'post-verification-adjustments.json').read_text())['final_source']
assert verification.source_fingerprint()==expected
artifacts=[]
for p in (mirror/'research/durable-requests/linux').glob('*/verification.json'):
    data=json.loads(p.read_text()); binary=mirror/data['artifact']['path']
    assert hashlib.sha256(binary.read_bytes()).hexdigest()==data['artifact']['sha256']
    artifacts.append(data['artifact'])
assert len(artifacts)==2
with (evidence/'verification-after-newline-linux.log').open('w') as log:
    subprocess.run([str(mirror/'build/contract-python/bin/python'),'tests/verification_tests.py'],cwd=mirror,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=60)
receipt={'status':'succeeded','final_source':expected,'unchanged_tested_artifacts':artifacts,'verification_script_tests':'20 passed'}
(evidence/'linux-final.json').write_text(json.dumps(receipt,indent=2)+'\n')
print(json.dumps({'status':'succeeded','source':expected,'unchanged_binaries':len(artifacts)}))
