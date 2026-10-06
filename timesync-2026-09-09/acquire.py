"""Four-channel physical comparison; retain failures and restore OFF/FTM.
Run with the project's saleae-automation-env Python. No simulated devices.
"""
import argparse, concurrent.futures, csv, datetime, json, time
from pathlib import Path
from urllib.request import Request, urlopen
from urllib.parse import quote
from saleae import automation

ROOT=Path(__file__).resolve().parent
def api(route, body=None, timeout=15):
    req=Request('http://127.0.0.1:3001/api/'+route,
                data=None if body is None else json.dumps(body).encode(),
                headers={'Content-Type':'application/json'})
    with urlopen(req,timeout=timeout) as r:
        raw=r.read().decode()
        try:return json.loads(raw)
        except ValueError:return raw
def save(path, value):path.write_text(json.dumps(value,indent=2),encoding='utf-8')
def command(n, cmd):return api('nodes/'+quote(n['port'],safe='')+'/command',{'cmd':cmd})
def fleet(nodes,cmd):
    def one(n):
        try:return str(n['node_id']),command(n,cmd)
        except Exception as e:return str(n['node_id']),{'error':repr(e)}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:return dict(ex.map(one,nodes))
def health(nodes):
    out={}
    for n in nodes:
        try:
            with urlopen('http://'+n['ip_address']+':8082/health',timeout=5) as r:out[str(n['node_id'])]=json.load(r)
        except Exception as e:out[str(n['node_id'])]={'error':repr(e)}
    return out
def main():
    p=argparse.ArgumentParser();p.add_argument('--seconds',type=float,default=60)
    p.add_argument('--methods',nargs='+',default=['ftm','mac','tsf','frozen'])
    p.add_argument('--repeats',type=int,default=1);p.add_argument('--restart',action='store_true')
    p.add_argument('--build',default='fdb0d3b4ab514a2d');p.add_argument('--rate',type=int,default=16000000)
    args=p.parse_args();folder=ROOT/'new-acquisition'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S-campaign');folder.mkdir(parents=True)
    save(folder/'settings.json',vars(args));nodes=sorted(api('nodes'),key=lambda n:n['node_id'])
    assert [n['node_id'] for n in nodes]==[0,1,2,3]
    save(folder/'nodes.json',nodes);h=health(nodes);save(folder/'health-before.json',h)
    assert all(h[str(i)].get('build_id')==args.build for i in range(4)),h
    save(folder/'mode-before.json',fleet(nodes,'mode'))
    assert all(not n.get('camera_running') for n in nodes),'Camera already running'
    try:
        if args.restart:
            save(folder/'restart.json',fleet(nodes,'restart'));time.sleep(20)
            save(folder/'health-restarted.json',health(nodes));save(folder/'mode-restarted.json',fleet(nodes,'mode'))
        with automation.Manager.connect(address='127.0.0.1',port=10430,connect_timeout_seconds=5) as manager:
            devices=manager.get_devices(include_simulation_devices=False);assert len(devices)==1
            save(folder/'device.json',{'id':devices[0].device_id,'type':str(devices[0].device_type)})
            for repeat in range(args.repeats):
                # Rotate order across repeats to reduce simple order confounding.
                order=args.methods[repeat%len(args.methods):]+args.methods[:repeat%len(args.methods)]
                for idx,method in enumerate(order):
                    run=folder/f'{repeat:02d}-{idx:02d}-mcpwm-{method}';run.mkdir()
                    result={'method':'mcpwm:'+method,'repeat':repeat,'duration':args.seconds,'sample_rate':args.rate,'build':args.build,'network_load':'no deliberate health polling during capture'}
                    try:
                        save(run/'stopped-before.json',fleet(nodes,'mode off --nomesh'))
                        if method=='ntp':
                            save(run/'ntp-pre-reset.json',fleet(nodes,'timing config mcpwm ftm'))
                            ref=next(n for n in nodes if n['node_id']==2)
                            save(run/'ntp-endpoints.json',{str(n['node_id']):command(n,'timing ntp server' if n['node_id']==2 else 'timing ntp '+ref['ip_address']) for n in nodes})
                        configured=fleet(nodes,'timing config mcpwm '+method);save(run/'configured.json',configured)
                        assert all('backend=mcpwm source='+method in str(v) and 'ERROR' not in str(v) for v in configured.values()),configured
                        armed=fleet(nodes,'mode 1hz --nomesh');save(run/'armed.json',armed)
                        time.sleep(12 if method=='ntp' else 3)
                        save(run/'mode-armed.json',fleet(nodes,'mode'))
                        save(run/'timing-armed.json',fleet(nodes,'timing'))
                        print('CAPTURE '+str(run),flush=True)
                        with manager.start_capture(device_id=devices[0].device_id,
                            device_configuration=automation.LogicDeviceConfiguration(enabled_digital_channels=[0,1,2,3],digital_sample_rate=args.rate),
                            capture_configuration=automation.CaptureConfiguration(capture_mode=automation.TimedCaptureMode(duration_seconds=args.seconds))) as cap:
                            cap.wait();cap.save_capture(filepath=str(run/'capture.sal'))
                            cap.export_raw_data_csv(directory=str(run),digital_channels=[0,1,2,3])
                        result['acquisition']='completed'
                    except Exception as e:result['error']=repr(e)
                    finally:
                        save(run/'stopped.json',fleet(nodes,'mode off --nomesh'))
                        save(run/'telemetry.json',fleet(nodes,'timing dump'));save(run/'health-after.json',health(nodes))
                        save(run/'result.json',result)
                    print(json.dumps(result),flush=True)
                    if 'error' in result: return
    finally:
        save(folder/'final-stop.json',fleet(nodes,'mode off --nomesh'))
        save(folder/'final-config.json',fleet(nodes,'timing config mcpwm ftm'))
        save(folder/'final-health.json',health(nodes))
    print('SAVED '+str(folder),flush=True)
if __name__=='__main__':main()
