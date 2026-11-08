from pathlib import Path
import csv,json,statistics
out=Path(__file__).resolve().parent
all_rows=[{k:float(v) for k,v in r.items()} for r in csv.DictReader((out/'frame-trace.csv').open())]
rows=[r for r in all_rows if r['frame_index']>=121]
d=json.loads((out/'wait-profile.json').read_text())
assert len(rows)==d['sampled_frames']==1200
assert sum(r['ticks'] for r in rows)==d['physics_ticks']
assert abs(rows[0]['debt_ms']-all_rows[120]['debt_ms']-rows[0]['dt_ms']+rows[0]['ticks']*(1000*0.01666666753590107))<.001
for r in rows:
    assert abs(r['wait_total_ms']-r['slot_wait_ms']-r['other_wait_ms']-r['timer_wait_ms'])<.05
    assert r['slot_wait_ms']<=r['module_update_ms']+.01
metrics=['frame_ms','gpu_ms','physics_ms','module_update_ms','slot_wait_ms','wait_total_ms','timer_wait_ms','other_wait_ms','present_ms','submit_ms','begin_frame_ms','ui_begin_ms','backbuffer_begin_ms','render_ms','snapshot_ms','pipeline_prepare_ms','graph_record_ms','ecs_gpu_ms','scene_additions_ms','movement_ms','skeletal_ms']
summary={'measured_frames':len(rows),'metrics':{},'windows':[],'blocking_slot_frames':sum(r['slot_target']>r['slot_completed'] for r in rows),'actual_wait_calls':sum(r['wait_calls'] for r in rows),'max_early_wait_ms':max(r['slot_wait_ms'] for r in rows),'frames_with_slot_wait_above_1ms':sum(r['slot_wait_ms']>1 for r in rows)}
for k in metrics:
    vals=sorted(r[k] for r in rows);summary['metrics'][k]={'mean':statistics.mean(vals),'median':statistics.median(vals),'p95':vals[int(.95*(len(vals)-1))],'max':max(vals)}
for first in range(121,1321,300):
    rs=[r for r in rows if first<=r['frame_index']<first+300]
    result={k:statistics.mean(r[k] for r in rs) for k in ['dt_ms','gpu_ms','physics_ms','slot_wait_ms','wait_total_ms','present_ms','render_ms','begin_frame_ms','backbuffer_begin_ms','ecs_gpu_ms','scene_additions_ms','movement_ms']}
    result.update(first=first,debt_end_ms=rs[-1]['debt_ms'],ticks_per_frame=sum(r['ticks'] for r in rs)/len(rs),blocking_slots=sum(r['slot_target']>r['slot_completed'] for r in rs))
    summary['windows'].append(result)
# H1 upper bound for moving only the measured frame-slot wait; not a speedup prediction.
summary['h1_ideal_mean_saving_bound_ms']=summary['metrics']['slot_wait_ms']['mean']
summary['h1_ideal_fps_bound']=1000/(d['frame_ms']-summary['h1_ideal_mean_saving_bound_ms'])
total_physics=sum(r['physics_ms'] for r in rows);ticks=sum(r['ticks'] for r in rows)
summary['observed_physics_ms_per_tick_including_loop_overhead']=total_physics/ticks
sim_budget=60*total_physics/ticks
# Optimistic one-owner-thread capacity: omit several smaller costs; no prediction of an implemented loop.
render_cost=summary['metrics']['render_ms']['mean']-summary['metrics']['present_ms']['mean']-summary['metrics']['timer_wait_ms']['mean']
other_render_cost=summary['metrics']['frame_ms']['mean']-summary['metrics']['physics_ms']['mean']-summary['metrics']['present_ms']['mean']
summary['capacity_estimate']={'physics_owner_ms_per_second_at_60hz':sim_budget,'render_cpu_ms_excluding_present':render_cost,'nonphysics_nonpresent_ms_per_iteration':other_render_cost,'conditional_single_owner_render_fps':max(0,(1000-sim_budget)/other_render_cost),'owner_ms_required_for_60_ticks_and_20_frames':sim_budget+20*other_render_cost,'gpu_ms_mean':summary['metrics']['gpu_ms']['mean'],'assumptions':'Fixed observed mean costs, all Present cost removable, no other waits; not a runtime prediction. Moving game systems to another cadence changes these costs.'}
(out/'summary.json').write_text(json.dumps(summary,indent=2))
print(json.dumps(summary,indent=2))
