"""Short-run rejection check; passing does not replace a multi-minute soak."""
import json,sys
from pathlib import Path
d=json.loads(Path(sys.argv[1]).read_text())
growth=d['physics_debt_end']-d['physics_debt_start']
assert abs(growth-d['physics_requested_seconds']+d['physics_simulated_seconds'])<.002
fps=1000/d['frame_ms']
failures=[]
if fps<20:failures.append('FPS below 20')
if growth>1/60:failures.append('net physics debt increased by more than one step')
print(json.dumps({'fps':fps,'debt_growth_seconds':growth,'failures':failures}))
raise SystemExit(bool(failures))
