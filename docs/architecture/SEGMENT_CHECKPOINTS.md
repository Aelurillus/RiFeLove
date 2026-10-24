# Safe segment checkpoints and recovery

Status: the checkpoint contract, SQLite journal, video-only segment writer, media-level segment verifier, and verified stream-copy assembler are implemented. The writer preserves global PTS and omits source auxiliary streams. The verifier checks the single-video-stream contract, codec/dimensions/pixel and color tags, first-packet keyframe, full decoded frame count, each frame's global rational timestamp, and the exclusive end boundary. The assembler rejects gaps, validates codec parameters and extradata, interleaves each source auxiliary stream once, preserves chapters, validates the assembled output, and publishes without replacement. The processing pipeline does not yet create/register resumable segments, hash and re-verify their files on recovery, or expose a resume action.

## Identity boundary

Each task has one immutable, versioned checkpoint manifest. It binds the source content hash and selected stream, exact source interval and rational target rate, model ID/version/content hash, inference backend and precision, color profile, encoder/container/options, output time base, and the relevant application and media-runtime versions. A path or task ID alone never establishes compatibility. A changed source, model, processing configuration, color contract, encoder, or runtime invalidates reuse; the UI must explain the mismatch and offer a new run from frame zero.

The manifest is committed before the first segment. A second manifest for the same task must match byte-for-byte. It is not edited to make an old segment appear compatible.

## Segment boundary and file contract

Segments own half-open output-frame ranges `[firstOutputFrame, endOutputFrame)`. The first range starts at frame zero. Every committed segment must start exactly where the preceding segment ended; gaps, overlaps and out-of-order records are rejected. The intended timestamps are derived from the global integer-index rational CFR clock, never from a segment-local floating-point counter.

Each segment is a separate, finalized video-only container file. Audio, subtitles, chapters and other auxiliary streams are copied once from the original source during final assembly. A segment contract records its exact first PTS, exclusive end boundary, frame count, codec parameters and extradata fingerprint, pixel/color profile, encoder settings fingerprint, file byte count and SHA-256. Each segment starts with an independently decodable keyframe. All segments must expose compatible codec parameters before stream-copy stitching is allowed.

The pipeline may seek to a keyframe before a resumed boundary to reconstruct the previous/current source-frame pair required by RIFE. It discards scheduled output before the first uncommitted output-frame index, then generates every remaining frame once. Boundary overlap is input context only; it never duplicates a committed output frame.

## Commit ordering and crash behavior

1. Encode a segment to a unique same-directory temporary path; do not overwrite an existing artifact.
2. Flush encoder and muxer, close the file, and validate it by decoding its full video stream, checking frame count, PTS order, exact rational boundary, color contract, codec parameters and first-frame keyframe.
3. Compute SHA-256 and byte count only after close. Publish the segment into the task-owned artifact directory with no-replace semantics.
4. In one SQLite transaction, insert the segment record only if the manifest exists, the task is active, and the frame range extends the current contiguous prefix.

A crash before step 4 leaves an unregistered orphan. Recovery never trusts it based on its filename; it remains available for diagnosis and is not deleted automatically. A crash after step 4 leaves a resumable record, subject to full identity and on-disk hash/contract verification. Partial temporary files are never treated as checkpoints.

SQLite stores the immutable manifest and the ordered segment journal. The current journal API enforces manifest immutability, active-task ownership, SHA-256 syntax, positive file size, and contiguous frame ranges. The media writer validates a standalone video stream and its first encoded packet before no-replace publication. `MediaEngine::verifyVideoSegment` validates a finalized segment against its media/timeline contract, while `MediaEngine::assembleVideoSegments` validates and combines the segment set with source auxiliary streams. The caller remains responsible for hashing each committed file and re-running verification when recovering it.

## Final assembly and publication

Once the segment prefix covers the expected output-frame count, the assembler opens all validated segments and the original source. It copies encoded video packets with exact timestamp rescaling and their global presentation times, and copies compatible auxiliary streams from the source exactly once. Packet ordering uses DTS; validation uses PTS, stream boundaries and the source's exclusive end. Container metadata, language/disposition tags, chapters, rotation and supported side data follow the existing media-preservation contract.

Assembly writes to a unique temporary output. The ordinary full-file validator must confirm expected video frame count, monotonic PTS, target cadence, duration, color/bit depth, auxiliary streams and audio boundaries before atomic no-replace publication. If assembly or validation fails, committed segments remain intact and the final path remains untouched. A crash after final publication but before the SQLite completion commit must be recognized as an existing output; it is never overwritten or silently deleted.

## Artifact ownership and cleanup

Task-owned segment paths are stored explicitly in SQLite. Cleanup may remove only a recorded artifact whose path remains inside that task's dedicated directory and whose bytes still match its recorded hash. Cleanup requires the queue's exclusive OS ownership lease and occurs only after a validated final output is committed, or after an explicit user action. Recovery never deletes by wildcard, follows junctions/symlinks, replaces user files, or treats a stale PID/marker as proof of ownership.

## Resume acceptance

- Kill the writer at each pre-publish, post-publish/pre-journal, post-journal and final-assembly boundary; confirm only journaled, verified segments are reused.
- Alter source, model, manifest, runtime/encoder contract, or a segment byte; resume must fail closed and preserve all existing files.
- Resume at multiple nonzero PTS origins and fractional rates; verify the first resumed output index and every following timestamp against the uninterrupted baseline.
- Compare decoded output frames and PCM audio against an uninterrupted run; verify subtitles, chapters, language/disposition, rotation and color metadata for supported containers.
- Exercise disk-full, cancellation, encoder failure, corrupt segments and process termination. Confirm no published output is truncated, overwritten, or misreported as complete.
- Run multi-hour tests before claiming the release recovery gate.

Until the media writer and end-to-end fault tests satisfy these gates, the application must continue to describe crash recovery as restart-from-zero rather than checkpoint resume.
