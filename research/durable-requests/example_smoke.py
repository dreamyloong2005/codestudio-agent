import sys,subprocess,tempfile,json,hashlib
from pathlib import Path
root=Path(__file__).resolve().parents[2]
exe=Path(sys.argv[1]).resolve(strict=True)
output=Path(sys.argv[2])
results=[]
with tempfile.TemporaryDirectory(prefix='codestudio-request-example-') as temporary:
    workspace=Path(temporary).resolve()
    for mode in ('readonly','demo'):
        command=[sys.executable,str(root/'examples/task_client.py'),'--agent',str(exe),'--workspace',str(workspace)]
        if mode=='demo': command.append('--demo')
        done=subprocess.run(command,capture_output=True,text=True,timeout=30,check=True)
        results.append({'mode':mode,'exit_code':done.returncode,'stdout':done.stdout})
        if mode=='readonly': assert not (workspace/'.codestudio').exists()
        else: assert (workspace/'CODESTUDIO_DEMO.txt').read_text()=='Hello from CodeStudio-Agent!\n'
receipt={'status':'succeeded','binary':str(exe),'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'results':results}
output.write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
print(json.dumps({'status':'succeeded','example_modes':len(results),'receipt':str(output)}))
