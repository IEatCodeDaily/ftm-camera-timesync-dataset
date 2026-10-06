"""Constant-level acquisition controls, including sample-rate sensitivity."""
import csv,datetime,json
from acquire import ROOT,api,fleet,save,automation
nodes=sorted(api('nodes'),key=lambda x:x['node_id'])
root=ROOT/'new-acquisition'/datetime.datetime.now().strftime('%Y%m%d-%H%M%S-static');root.mkdir()
try:
    save(root/'stopped.json',fleet(nodes,'mode off --nomesh'))
    with automation.Manager.connect(address='127.0.0.1',port=10430,connect_timeout_seconds=5) as manager:
        devices=manager.get_devices(include_simulation_devices=False);assert len(devices)==1
        for rate in [16000000,8000000]:
            for level in ['low','high']:
                folder=root/f'{rate}-{level}';folder.mkdir();save(folder/'command.json',fleet(nodes,'timing pin '+level))
                result=dict(rate=rate,level=level,seconds=2)
                try:
                    with manager.start_capture(device_id=devices[0].device_id,
                        device_configuration=automation.LogicDeviceConfiguration(enabled_digital_channels=[0,1,2,3],digital_sample_rate=rate),
                        capture_configuration=automation.CaptureConfiguration(capture_mode=automation.TimedCaptureMode(duration_seconds=2))) as cap:
                        cap.wait();cap.save_capture(filepath=str(folder/'capture.sal'));cap.export_raw_data_csv(directory=str(folder),digital_channels=[0,1,2,3])
                    counts=[0]*4;prev={};last=[0]*4
                    with (folder/'digital.csv').open() as f:
                        for row in csv.DictReader(f):
                            for c in range(4):
                                v=int(row[f'Channel {c}'])
                                if c in prev and v!=prev[c]:counts[c]+=1;last[c]=float(row['Time [s]'])
                                prev[c]=v
                    result.update(transitions=counts,last_transition_s=last)
                except Exception as e:result['error']=repr(e)
                save(folder/'result.json',result);print(json.dumps(result),flush=True)
finally:save(root/'restored-low.json',fleet(nodes,'timing pin low'))
print(root,flush=True)
