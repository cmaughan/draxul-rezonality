# Avoid rereading unchanged watched projects

**Summary:** Detect project edits without repeatedly rereading every file so idle Rezonality panes use less disk activity and processing.

**Source:** `plugins/rezonality/src/live_project.cpp`  
**Priority:** P1; static, high confidence. **Reported by:** Claude, Codex. `ProjectPipeline::fingerprint()` at lines 1238–1277 enumerates, sorts, reads, and hashes every file; lines 1544–1555 repeat after 100 ms even without edits. Its worker protects GUI latency, but idle I/O, CPU, and power scale with project bytes and pane count.

- [x] **Baseline:** Measure unchanged bytes read, worker CPU, and edit-to-rebuild latency for one and several panes on an asset-rich project.
  - `examples/pbr_robot` (28 files, 48,281,899 bytes), macOS Debug, loaded
    machine. The old poll read and hashed every byte, which is the same work
    as the new index's first scan: 546–581 ms cold and about 111–131 ms
    warm for one pane, and 4 × 48 MB (193 MB) per 100 ms poll for four panes.
    An edit could wait one poll interval plus one full scan before debounce.
- [x] **Implement:** Use native invalidation or a bounded per-file index, with overflow and missed-event rescans.
  - `src/project_file_index.{h,cpp}`: `ProjectFileIndex` keeps one
    `{size, mtime, content hash}` entry per file, rebuilt from every scan so
    it stays bounded by the current file count. Each poll still enumerates
    the tree (no missed events or overflow to recover from), stats each file
    by path, and rereads a file only when it is new, its stamp changed, it
    could not be read before, or its stamp is still racy. The fingerprint
    still covers paths and contents, so a touch alone does not rebuild.
    `ProjectPipeline::watch_counters()` exposes scans, files seen, files
    hashed, and bytes hashed.
- [x] **Functional safety:** Detect same-size edits, arbitrary-suffix shader includes, break/repair, and failed scans; preserve last-good builds.
  - Every file is still watched regardless of suffix. A stamp is trusted
    only once its mtime is more than `kRacyStampWindow` (2 s, covering HFS+
    and FAT granularity) older than the scan, so a same-size rewrite that
    keeps the same coarse mtime is reread and detected. Scan failures keep
    the old messages and the existing watcher recovery path; break/repair and
    last-good behaviour are unchanged (existing live watch tests pass).
    Known limit: a tool that rewrites an old file with identical size and
    deliberately restores its old mtime is not detected; `rezonality_reload`
    still forces a rebuild.
- [x] **Compare:** Require no repeated full-content reads while unchanged and bounded edit latency.
  - Same project: steady scans read 0 bytes and take 0.75–0.81 ms (one
    pane) or about 1–1.9 ms (four panes in parallel), versus 111–581 ms and
    48 MB per pane before. Edit detection is now one poll interval plus a
    stat-only scan.
  - `Rezonality project watch rereads only changed files` checks exact
    files/bytes hashed for idle, touch, same-size edit, racy same-stamp
    rewrite, delete/restore, and failed-scan recovery.
    `Rezonality live watch is idle without content reads` checks a running
    `LiveProject` completes at least four polls without rereading anything
    and still detects an edit.
- [x] **Platforms:** Verify native watcher behavior and Rezonality output on Windows/Vulkan and macOS/Metal.
  - No native watcher was added; the index is portable `std::filesystem`
    code with no backend dependency. macOS/Metal: aggregate and smoke pass
    locally. Windows/Vulkan is covered by routine CI; stamps are read by
    path rather than from iteration data, which can be stale on Windows for
    a file still open for writing.
- [x] **Acceptance:** Idle project contents are not reread every poll.
