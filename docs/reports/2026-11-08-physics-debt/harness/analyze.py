import collections,csv,json,pathlib,statistics,sys
p=pathlib.Path(__file__).resolve().parent
tag=sys.argv[1]
def rows(name): return [{k:float(v) for k,v in r.items()} for r in csv.DictReader((p/(tag+'-'+name+'.csv')).open())]
steps=rows('steps'); frames=rows('frames'); measured=[r for r in steps if r['frame']>=121]
ticks={int(r['tick']) for r in measured}
summary={'frames':len(frames),'steps':len(measured),'metrics_per_step':{}}
for k in ['pre_ms','callback_ms','backend_ms','post_ms','process_cpu_ms','active','bodies']:
    vals=[r[k] for r in measured];summary['metrics_per_step'][k]={'mean':statistics.mean(vals),'p95':sorted(vals)[int(.95*(len(vals)-1))]}
summary['wall_limit_frames']=sum(r['wall_limit'] for r in measured)
summary['ticks_histogram']=dict(sorted(collections.Counter(collections.Counter(r['frame'] for r in measured).values()).items()))
summary['sync_probe']={k:{'median':statistics.median(r[k] for r in rows('sync-probe')),'mean':statistics.mean(r[k] for r in rows('sync-probe'))} for k in ['scheduled_ms','direct_ms']}
summary['jobs_all_frames']=rows('jobs')
events=collections.defaultdict(list)
for r in csv.DictReader((p/(tag+'-jolt-events.csv')).open()):
    if int(r['tick']) in ticks:events[int(r['tick']),int(r['thread'])].append((r['name'],int(r['start_cycle']),int(r['end_cycle'])))
stats=collections.defaultdict(lambda:[0,0,0]);bad=0
for key,event_rows in events.items():
    stack=[]
    for name,start,end in event_rows:
        assert end>=start,(name,start,end)
        while stack and start>=stack[-1][2]:stack.pop()
        if stack:
            if end>stack[-1][2]:bad+=1
            else:stats[stack[-1][0]][1]-=end-start
        stats[name][0]+=end-start;stats[name][1]+=end-start;stats[name][2]+=1
        stack.append((name,start,end))
assert bad==0,('Invalid event nesting',bad)
total=sum(x[1] for x in stats.values())
summary['profiled_ticks']=len(set(t for t,_ in events))
summary['jolt_scopes']=[{'name':name,'inclusive_cycles':inc,'exclusive_cycles':exc,'exclusive_percent':exc/total*100 if total else 0,'calls':count} for name,(inc,exc,count) in sorted(stats.items(),key=lambda kv:-kv[1][1])]
h=1000*0.01666666753590107
acc=0;worst=0
byframe=collections.Counter(r['frame'] for r in steps)
for r in frames:
    acc+=r['dt_ms']-h*byframe[r['frame']]
    if byframe[r['frame']]:worst=max(worst,abs(acc-r['accumulator_ms']))
summary['accumulator_max_error_ms']=worst
summary['windows']=[]
for first in range(121,len(frames),300):
    fs=[r for r in frames if first<=r['frame']<first+300];ss=[r for r in measured if first<=r['frame']<first+300]
    summary['windows'].append({'first':first,'frames':len(fs),'frame_ms':statistics.mean(r['dt_ms'] for r in fs),'required_ticks_frame':sum(r['dt_ms'] for r in fs)/h/len(fs),'done_ticks_frame':len(ss)/len(fs),'debt_end_ms':fs[-1]['accumulator_ms'],'backend_ms_step':statistics.mean(r['backend_ms'] for r in ss),'post_ms_step':statistics.mean(r['post_ms'] for r in ss),'wall_limits':sum(r['wall_limit'] for r in ss)})
(p/(tag+'-summary.json')).write_text(json.dumps(summary,indent=2))
print(json.dumps({k:v for k,v in summary.items() if k not in ['jolt_scopes','jobs_all_frames']},indent=2))
print('job totals',{k:sum(r[k] for r in summary['jobs_all_frames']) for k in ['create_ms','queue_ms','free_ms','drain_ms','barrier_ms','creates','queues']})
print('Jolt exclusive work (parallel/nested, not wall frame costs):')
for r in summary['jolt_scopes'][:16]:print(r['name'],round(r['exclusive_percent'],2),r['calls'])
