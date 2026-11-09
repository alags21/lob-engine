# Visualizer

Open `index.html` in a browser (double-click works; no server, no network). Controls: Play/Pause (space),
Step (left/right arrows, one recorded snapshot), Start/End (Home/End), speed, and the scrub bar.
`index.html#at=0.5&play=0` starts paused at 50% (used for screenshots).

Data:
- `data/sample_run.js`: a synthetic run exported by `lob_sim` (`window.LOB_RUN`).
- `data/latency.js`: histograms and percentiles copied from `bench/results/*.json` (`window.LOB_LATENCY`).
- "Load run..." accepts any `lob-run-v1` export (`.json`, or the `.js` form).

Regenerate: `python scripts/make_viz_data.py --sim build-rel/tools/lob_sim` (absolute path on Windows).
Test: `node viz/test/viz_test.js`.
