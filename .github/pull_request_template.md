## Description
<!-- Provide a brief description of the changes introduced by this pull request. -->

## Subsystem(s) Affected
- [ ] Pluma (Compiler / Bytecode VM / AOT / Language Server)
- [ ] Cuervo Core (Fixed timestep / Math / Determinism)
- [ ] RHI & Render (Vulkan / OpenGL / WebGL2 / SDF Batcher)
- [ ] Script Prelude (Nodes / UI Widgets / Animation / Particles)
- [ ] Editor & Tools (`cuervo-editor` / `cuervo-tools` / VS Code extension)
- [ ] Documentation / Performance Baselines

## Checklist
- [ ] Code builds without warnings using CMake presets (`cmake --preset debug && cmake --build --preset debug`)
- [ ] All automated tests pass (`SDL_VIDEO_DRIVER=offscreen ctest --preset debug`)
- [ ] Deterministic replay hashes match (if physics, particles, or core math were touched)
- [ ] Added unit or integration tests for new features or bug fixes
- [ ] Public prelude APIs include documentation comments (enforced by `api_docs_coverage`)
- [ ] Every commit is signed off (`git commit -s`, Developer Certificate of Origin; see CONTRIBUTING.md)
- [ ] A change to a settled decision updates `docs/DESIGN.md` (a Decision/Clarified note) and, for the milestones table, `docs/ROADMAP.md` (`python3 -I tools/gen_roadmap.py --write`)
