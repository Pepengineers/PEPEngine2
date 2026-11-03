# Bistro street NPCs and interior fox

Integrate four animated human NPCs walking on the street and one animated Khronos
Fox walking inside Bistro. The user explicitly requested reusing human models and
animations from `D:/Project/Multi-GPU-Render-DX12`; inspect that project read-only,
copy only required assets and retain their existing provenance/license records.
Use the existing KhronosFox asset for the fox. Preserve each model's correct scale,
orientation, ground height and walking animation. Simple bounded loops suffice;
navigation, avoidance and AI are outside scope.

Own `Apps/App/src/BistroCharacters.h`, required character assets/provenance and a
dedicated focused check header/source. Public interface is in README.md.
`BistroCharacters::Scene` manages both populations. Defaults: `App.NPC.Count=4`
and `App.Fox.Count=1`. Zero removes the corresponding population. Runtime count
changes reconcile independent entities; choose validated, documented upper bounds.
Coordinate before changing renderer code or shared project/test files.

Inspect `SkeletalMeshRenderComponent`, `RenderModuleSkinning`, scene loading APIs
and `SecondGroupTests` fox fixtures. Respect pause and world time scale; movement
must match animation and remain inside safe street/interior areas. Verify exact
default/zero/changed counts, shared source assets, changing transforms AND skeletal
poses across frames, valid skinning draw output, pause, and finite bounds. Report
changed paths, checks/results, asset provenance and limitations. Leave shared
builds, GPU scheduling, App.cpp hooks and commits to the coordinator.
