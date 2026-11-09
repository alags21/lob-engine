# Release checklist: merging the stacked PRs and tagging v0.1.0

Nothing has been merged or tagged. The work is a stack of eight pull requests, each based on the
previous stage's branch:

| PR | Branch | Base |
|---|---|---|
| #1 | `stage-0-scaffold` | `main` |
| #2 | `stage-1-core` | `stage-0-scaffold` |
| #3 | `stage-2-correctness` | `stage-1-core` |
| #4 | `stage-3-performance` | `stage-2-correctness` |
| #5 | `stage-4-concurrency` | `stage-3-performance` |
| #6 | `stage-5-visualizer` | `stage-4-concurrency` |
| #7 | `stage-6-polish` | `stage-5-visualizer` |
| #8 | `stage-7-real-data` | `stage-6-polish` |

Do **not** tag until all eight are on `main`: a tag on an unmerged stacked branch points at a commit
that is not part of the project's mainline history.

## Before merging (owner decisions)

1. **License.** Add a `LICENSE` file (the repository has none) before making it public.
2. **Pages.** `.github/workflows/pages.yml` is manual-trigger only. To publish the visualizer, enable
   Pages (Settings > Pages > Source: GitHub Actions) and run the workflow by hand. Skip if not wanted.
3. **Visibility.** The repository is private; changing that is a separate, deliberate step.
4. Confirm CI is green on the tip of PR #8 (it contains everything below it).
5. The raw Nasdaq ITCH file is not in the repository and must never be added (its redistribution terms are unclear); `scripts/check_repo_size.py` enforces this in CI.

## Merge order

Use **merge commits** (not squash, not rebase-merge) so that each stage's commits keep their
identity; then each following PR needs only a base change and stays clean.

```
# 1. PR #1 into main
gh pr merge 1 --merge

# 2. for each next PR N = 2..8, retarget it to main and merge it, in order
for n in 2 3 4 5 6 7 8; do
  gh pr edit $n --base main
  # wait for CI on the retargeted PR if you want to be careful, then:
  gh pr merge $n --merge
done
```

GitHub also retargets a child PR automatically when its base branch is deleted after the merge; the
explicit `gh pr edit --base main` makes the step independent of that setting.

If you must use **squash** merges instead, every later branch has to be rebased onto the new `main`
after each merge, because the squashed commit has a different hash from the commits it replaced:

```
git fetch origin
# after PR #1 was squash-merged:
git rebase --onto origin/main stage-0-scaffold stage-1-core
git push --force-with-lease origin stage-1-core
gh pr edit 2 --base main
# repeat for stage-2-correctness (old base stage-1-core) ... stage-7-real-data (old base stage-6-polish)
```

(Force-pushing is only appropriate here because these are private stacked branches.)

## After everything is on main

Verify first:

```
git checkout main && git pull
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
ctest --test-dir build --output-on-failure
python scripts/make_report.py --check && python scripts/make_mt_report.py --check
node viz/test/viz_test.js
gh run list --branch main --limit 3      # CI green on the merge commit
```

Then tag (annotated) and push the tag:

```
git tag -a v0.1.0 -m "lob-engine v0.1.0: matching engine, reference-model fuzzing, benchmarks, SPSC pipeline, visualizer"
git push origin v0.1.0
```

Optionally create a GitHub release from the tag:

```
gh release create v0.1.0 --title "v0.1.0" --notes-file PROGRESS.md
```

If a mistake is found before the tag is pushed, delete it locally with `git tag -d v0.1.0`. Do not
move or delete a tag after it has been pushed or released.
