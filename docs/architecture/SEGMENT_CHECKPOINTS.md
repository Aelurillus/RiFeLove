# Safe segment checkpoints and recovery

Status: checkpoint writing, verification, queue recovery, explicit resume, final stream-copy assembly and startup adoption of verified published outputs are wired end to end. The queue uses 256 output frames per segment by default. Segments contain video only; audio and other supported source streams are copied once during final assembly. Every committed segment is hashed and journaled in SQLite, then re-hashed and decoded again before reuse. The pipeline reads the source from the beginning to rebuild RIFE input context, skips already committed output indices, generates the remaining frames and assembles the completed timeline. This is verified on bounded local GPU crash/recovery fixtures; multi-hour and broad hardware/content qualification remains open.

## Identity boundary

Each task has one immutable, versioned checkpoint manifest. It binds the source content hash and selected stream, exact source interval and rational target rate, model ID/version/content hash, inference backend and precision, color profile, encoder/container/options, output time base, and the relevant application and media-runtime versions. A path or task ID alone never establishes compatibility. A changed source, model, processing configuration, color contract, encoder, or runtime invalidates reuse; the UI must explain the mismatch and offer a new run from frame zero.

The manifest is committed before the first segment. A second manifest for the same task must match byte-for-byte. It is not edited to make an old segment appear compatible.

## Segment boundary and file contract

Segments own half-open output-frame ranges `[firstOutputFrame, endOutputFrame)`. The first range starts at frame zero. Every committed segment must start exactly where the preceding segment ended; gaps, overlaps and out-of-order records are rejected. The intended timestamps are derived from the global integer-index rational CFR clock, never from a segment-local floating-point counter.

Each segment is a separate, finalized video-only container file. Audio, subtitles, chapters and other auxiliary streams are copied once from the original source during final assembly. A segment contract records its first PTS, exclusive end boundary (validated within one stream time-base tick), frame count, codec parameters and extradata fingerprint, pixel/color profile, encoder identity, file byte count and SHA-256. Each segment starts with an independently decodable keyframe. All segments must expose compatible codec parameters before stream-copy stitching is allowed.

On resume the pipeline currently decodes the source from its beginning to rebuild the previous/current source-frame pair required by RIFE. It discards scheduled output before the first uncommitted output-frame index, then generates every remaining frame once. This avoids a source-seeking boundary bug at the cost of re-decoding earlier input; inference is skipped for the committed output prefix. Boundary overlap is input context only; it never duplicates a committed output frame.

## Commit ordering and crash behavior

1. Encode a segment to a unique same-directory temporary path; do not overwrite an existing artifact.
2. Flush encoder and muxer, close the file, and validate it by decoding its full video stream, checking frame count, PTS order, exact rational boundary, color contract, codec parameters and first-frame keyframe.
3. Compute SHA-256 and byte count only after close. Publish the segment into the task-owned artifact directory with no-replace semantics.
4. In one SQLite transaction, insert the segment record only if the manifest exists, the task is active, and the frame range extends the current contiguous prefix.

A crash before step 4 leaves an unregistered orphan. Recovery never trusts it based on its filename. On an explicit resume request, after manifest and committed-segment verification, the queue removes only generated segment files and temporary writers at or beyond the committed prefix inside that task-owned directory; unrelated files are preserved. A crash after step 4 leaves a resumable record, subject to full identity and on-disk hash/contract verification. Partial temporary files are never treated as checkpoints.

SQLite stores the immutable manifest and ordered segment journal. The queue manifest binds the serialized job/output, verified source/model/device/backend identity, media runtime version and segment size; changes reject resume. The journal enforces manifest immutability, active-task ownership, SHA-256 syntax, positive file size and contiguous frame ranges. The media writer validates a standalone video stream and its first encoded packet before no-replace publication. `MediaEngine::verifyVideoSegment` validates each finalized segment against its media/timeline contract, while `MediaEngine::assembleVideoSegments` validates and combines the segment set with source auxiliary streams. `application::CheckpointArtifacts` connects these checks to content hashes, task-owned paths and journal records. The queue exposes explicit resume for interrupted tasks in the desktop and CLI.

## Final assembly and publication

Once the segment prefix covers the expected output-frame count, the assembler opens all validated segments and the original source. It copies encoded video packets with timestamp rescaling and their global presentation times, and copies compatible auxiliary streams from the source exactly once. Packet ordering uses DTS; validation uses PTS, stream boundaries and the source's exclusive end. Container metadata, language/disposition tags, chapters, rotation and supported side data follow the existing media-preservation contract.

Assembly writes to a unique temporary output. The ordinary full-file validator confirms expected video frame count, monotonic PTS, target cadence, duration, color/bit depth, auxiliary streams and audio boundaries. Before atomic no-replace publication, the queue hashes the closed, validated temporary file and commits its SHA-256, byte count and final result JSON to a separate SQLite publication record. If assembly or validation fails, committed segments remain intact and the final path remains untouched. If the process stops after publication but before the task-completion commit, explicit resume hashes the existing final file and adopts it only when it exactly matches that durable proof and its frame count matches the verified segments. A changed or unproven file is preserved and resume refuses it; it is never overwritten.

## Artifact ownership and cleanup

Task-owned segment paths are stored explicitly in SQLite. Cleanup may remove only a recorded artifact whose path remains inside that task's dedicated directory and whose bytes still match its recorded hash. Cleanup requires the queue's exclusive OS ownership lease and occurs only after a validated final output is committed, or after an explicit user action. Recovery never deletes by wildcard, follows junctions/symlinks, replaces user files, or treats a stale PID/marker as proof of ownership.

## Resume evidence and remaining acceptance

- `Queue.Integration` kills a native writer after a verified segment has been journaled while the following segment has an active partial file. Recovery recognizes the interrupted task, rejects restart-as-retry, verifies and reuses the committed segment, leaves the partial file untouched, and produces the complete 66-frame video with PCM-identical audio.
- A second native crash test pauses after every expected frame has been finalized into four segments but before final assembly. Recovery has no in-memory pending frame; it assembles the existing 64-frame prefix without running duplicate interpolation and preserves PCM-identical audio.
- A third native crash test stops after atomic final-output publication but before SQLite marks the task complete. Recovery adopts the byte-identical validated output without rebuilding it; a one-byte modification is rejected and preserved.
- Unit tests cover manifest/segment serialization and task state transitions. The native integration test uses 16-frame segments to reach the crash point quickly; production queue segments default to 256 output frames.
- Still required: fault injection at every segment publish/journal/assembly boundary; altered source/model/runtime/segment rejection cases; multiple nonzero PTS origins and fractional rates; subtitle/chapter/color comparisons; disk-full, cancellation, encoder and corrupt-segment cases; multi-hour runs and wider hardware/content coverage.
